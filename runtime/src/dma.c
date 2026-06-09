/* dma.c — PS1 DMA controller simulation (Phase 3).
 *
 * Implements all 7 channel register reads/writes, DPCR, DICR,
 * and transfer execution for:
 *   - Ch2 (GPU): block mode and linked-list mode
 *   - Ch6 (OTC): ordering table clear
 *
 * Most channels execute synchronously when CHCR start bit is written. MDEC
 * request-mode transfers are advanced from the guest cycle clock so games
 * which synchronize video decode through DMA busy/request state see realistic
 * backpressure.
 * Reference: nocash PSX specs, DuckStation src/core/dma.cpp
 */

#include "dma.h"
#include "cdrom.h"
#include "dirty_ram_interp.h"
#include "gpu.h"
#include "mdec.h"
#include "overlay_capture.h"
#include "spu.h"
#include "event_ring.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Memory access — defined in memory.c */
extern uint32_t psx_read_word(uint32_t addr);
extern void     psx_write_word(uint32_t addr, uint32_t val);

/* Interrupt status — defined in memory.c */
extern uint32_t i_stat;
extern uint32_t g_debug_current_func_addr;
extern uint32_t g_debug_last_store_pc;
extern uint64_t s_frame_count;

/* ---- Per-channel registers ---- */

typedef struct {
    uint32_t madr;  /* +0x00: Memory address */
    uint32_t bcr;   /* +0x04: Block control */
    uint32_t chcr;  /* +0x08: Channel control */
} DMAChannel;

static DMAChannel channels[7];

typedef struct {
    uint8_t active;
    uint8_t debug_started;
    uint32_t total_words;
    uint32_t remaining_words;
    uint32_t cycles_accum;
    uint32_t start_addr;   /* madr at transfer start (CD overlay capture) */
} DMAAsyncChannel;

static DMAAsyncChannel mdec_async[2];
static DMAAsyncChannel cdrom_async;

/* ---- CD DMA transfer log ---- */
/* Every forward CH3 DMA that lands below 0x1C0000 (game data region) records
 * (setloc_lba, dest_addr, size). Transfers to 0x1C0000+ are FMV/streaming
 * buffers and are excluded to keep the ring focused on overlay loads.
 * Surfaced via the cd_read_log TCP command; pairs with overlay_dump to map
 * overlay regions back to disc positions for extract_overlays.py. */
#define CD_DMA_LOG_CAP 65536
typedef struct { int lba; uint32_t dest; uint32_t size; } CdDmaEntry;
static CdDmaEntry cd_dma_log[CD_DMA_LOG_CAP];
static uint32_t   cd_dma_log_head  = 0;
static uint32_t   cd_dma_log_total = 0;

static void cd_dma_log_push(int lba, uint32_t dest, uint32_t size) {
    cd_dma_log[cd_dma_log_head % CD_DMA_LOG_CAP].lba  = lba;
    cd_dma_log[cd_dma_log_head % CD_DMA_LOG_CAP].dest = dest;
    cd_dma_log[cd_dma_log_head % CD_DMA_LOG_CAP].size = size;
    cd_dma_log_head = (cd_dma_log_head + 1) % CD_DMA_LOG_CAP;
    cd_dma_log_total++;
}

uint32_t cd_dma_log_get_total(void) { return cd_dma_log_total; }
void     cd_dma_log_get_entry(uint32_t idx, int *lba, uint32_t *dest, uint32_t *size) {
    uint32_t cap   = CD_DMA_LOG_CAP;
    uint32_t oldest = cd_dma_log_total > cap ? cd_dma_log_total - cap : 0;
    if (idx < oldest || idx >= cd_dma_log_total) { *lba = -1; return; }
    uint32_t slot = idx % cap;
    *lba  = cd_dma_log[slot].lba;
    *dest = cd_dma_log[slot].dest;
    *size = cd_dma_log[slot].size;
}

#define DMA_MDEC_IN_CYCLES_PER_WORD   1u
#define DMA_MDEC_OUT_CYCLES_PER_WORD 14u
#define DMA_GPU_CYCLES_PER_WORD       1u
#define DMA_CDROM_CYCLES_PER_WORD     1u

typedef struct {
    uint8_t active;
    uint32_t total_words;
    uint32_t cycles_remaining;
} DMADelayedComplete;

static DMADelayedComplete delayed_complete[7];

/* ---- Global registers ---- */

static uint32_t dpcr;  /* 0x1F8010F0: DMA control (enable bits) */
static uint32_t dicr;  /* 0x1F8010F4: DMA interrupt control */

#define DICR_WRITE_MASK 0x00FF803Fu
#define DICR_RESET_MASK 0x7F000000u

static DMATraceEntry dma_trace[DMA_TRACE_CAP];
static uint64_t dma_trace_seq;

static void trace_dma(uint32_t kind, int ch, uint32_t total_words,
                      uint32_t dicr_before, uint32_t i_stat_before) {
    DMATraceEntry *e = &dma_trace[dma_trace_seq % DMA_TRACE_CAP];
    e->seq = dma_trace_seq++;
    e->frame = (uint32_t)s_frame_count;
    e->kind = kind;
    e->channel = (uint32_t)ch;
    e->total_words = total_words;
    e->madr = (ch >= 0 && ch < 7) ? channels[ch].madr : 0;
    e->bcr = (ch >= 0 && ch < 7) ? channels[ch].bcr : 0;
    e->chcr = (ch >= 0 && ch < 7) ? channels[ch].chcr : 0;
    e->dpcr = dpcr;
    e->dicr_before = dicr_before;
    e->dicr_after = dicr;
    e->i_stat_before = i_stat_before;
    e->i_stat_after = i_stat;
    e->func = g_debug_current_func_addr;
    e->pc = g_debug_last_store_pc;
}

/* ---- Helpers ---- */

static void update_master_irq(void) {
    /* DICR bit 31 (master IRQ flag) is read-only, calculated as:
     * bit31 = bit15 OR (bit23 AND ((bits16-22 AND bits24-30) != 0))
     *
     * We store bits 0-30 in dicr and compute bit 31 on read.
     *
     * I_STAT bit 3 is set by direct assertion in complete_transfer(),
     * not here.  The BIOS shell DMA-IRQ handler at 0x80058628 only
     * clears DICR bit 27 (ch3 flag) — any other channel flag (e.g. ch2
     * after a GPU DMA) stays high.  If this routine re-asserted
     * i_stat |= (1<<3) on every DICR read or write, the BIOS handler's
     * "lw $v0, 0($v1)" of DICR right after writing 0xFFFFFFF7 to I_STAT
     * would put bit 3 back, and the kernel would loop forever in the
     * exception handler.  Discless BIOS boot (memcard / CD player
     * screen) is the case that exhibits this.  */
}

static int channel_enabled(int ch) {
    /* DPCR: each channel has 4 bits, bit 3 of each group is enable.
     * Ch0 = bits 0-3, Ch1 = bits 4-7, etc. Enable = bit (ch*4 + 3). */
    return (dpcr >> (ch * 4 + 3)) & 1;
}

static int channel_irq_enabled(int ch) {
    return ((dicr >> (16 + ch)) & 1u) && ((dicr >> 23) & 1u);
}

static uint32_t transfer_word_count(int ch) {
    uint32_t chcr = channels[ch].chcr;
    uint32_t sync_mode = (chcr >> 9) & 3;
    uint32_t block_size = channels[ch].bcr & 0xFFFFu;
    uint32_t block_count = (channels[ch].bcr >> 16) & 0xFFFFu;

    if (sync_mode == 1) {
        if (block_size == 0) block_size = 0x10000u;
        if (block_count == 0) block_count = 1u;
        return block_size * block_count;
    }

    if (block_size == 0) block_size = 0x10000u;
    return block_size;
}

static void complete_transfer(int ch) {
    uint32_t dicr_before = dicr;
    uint32_t i_stat_before = i_stat;
    channels[ch].chcr &= ~((1u << 24) | (1u << 28));
    if (channel_irq_enabled(ch)) {
        dicr |= (1u << (24 + ch));
        update_master_irq();
        /* Edge-trigger I_STAT bit 3 once per completion. See comment in
         * update_master_irq() for why this is not done there. */
        i_stat |= (1u << 3);
    }
    trace_dma('C', ch, 0, dicr_before, i_stat_before);
    event_ring_record_aux(EV_DMA_DONE, (uint8_t)ch, channels[ch].chcr);
    event_ring_record_aux(EV_DEQ, (uint8_t)(SRC_DMA0 + ch), channels[ch].chcr);
}

/* ---- Transfer execution ---- */

static void cancel_async_transfer(int ch) {
    if (ch >= 0 && ch < 2) {
        mdec_async[ch].active = 0;
        mdec_async[ch].debug_started = 0;
        mdec_async[ch].total_words = 0;
        mdec_async[ch].remaining_words = 0;
        mdec_async[ch].cycles_accum = 0;
    }
    if (ch == 3) {
        cdrom_async.active = 0;
        cdrom_async.debug_started = 0;
        cdrom_async.total_words = 0;
        cdrom_async.remaining_words = 0;
        cdrom_async.cycles_accum = 0;
    }
    if (ch >= 0 && ch < 7) {
        delayed_complete[ch].active = 0;
        delayed_complete[ch].total_words = 0;
        delayed_complete[ch].cycles_remaining = 0;
    }
}

static void schedule_delayed_complete(int ch, uint32_t total_words,
                                      uint32_t cycles_per_word) {
    if (total_words == 0 || cycles_per_word == 0) {
        complete_transfer(ch);
        return;
    }

    uint64_t cycles = (uint64_t)total_words * (uint64_t)cycles_per_word;
    if (cycles > UINT32_MAX) cycles = UINT32_MAX;

    delayed_complete[ch].active = 1;
    delayed_complete[ch].total_words = total_words;
    delayed_complete[ch].cycles_remaining = (uint32_t)cycles;
    event_ring_record_aux(EV_DMA_SCHED, (uint8_t)ch, channels[ch].chcr);
}

static void advance_delayed_complete(int ch, uint32_t cycles) {
    DMADelayedComplete *d = &delayed_complete[ch];
    if (!d->active) return;
    if (cycles < d->cycles_remaining) {
        d->cycles_remaining -= cycles;
        return;
    }

    d->active = 0;
    d->total_words = 0;
    d->cycles_remaining = 0;
    complete_transfer(ch);
}

static void start_async_mdec_transfer(int ch) {
    DMAAsyncChannel *a = &mdec_async[ch];
    if (a->active) return;

    a->active = 1;
    a->debug_started = 0;
    a->total_words = transfer_word_count(ch);
    a->remaining_words = a->total_words;
    a->cycles_accum = 0;

    if (ch == 0) {
        mdec_debug_dma_in_start(channels[0].madr & 0x1FFFFCu, a->remaining_words);
    } else {
        mdec_debug_dma_out_start(channels[1].madr & 0x1FFFFCu, a->remaining_words);
    }
    a->debug_started = 1;
}

static void finish_async_mdec_transfer(int ch, uint32_t final_addr, uint32_t total_words) {
    if (ch == 0) {
        mdec_debug_dma_in_end(final_addr, total_words);
    } else {
        mdec_debug_dma_out_end(final_addr, total_words);
    }
    cancel_async_transfer(ch);
    complete_transfer(ch);
}

static void start_async_cdrom_transfer(void) {
    DMAAsyncChannel *a = &cdrom_async;
    if (a->active) return;

    a->active = 1;
    a->debug_started = 0;
    a->total_words = transfer_word_count(3);
    a->remaining_words = a->total_words;
    a->cycles_accum = 0;
    a->start_addr = channels[3].madr & 0x1FFFFCu;

    if (a->total_words == 0) {
        cancel_async_transfer(3);
        complete_transfer(3);
    }
}

static void finish_async_cdrom_transfer(uint32_t final_addr) {
    DMAAsyncChannel *a = &cdrom_async;
    uint32_t step       = (channels[3].chcr >> 1) & 1u;
    uint32_t load_start = a->start_addr;
    uint32_t size       = a->total_words * 4u;

    /* Log and capture game-data transfers only.
     * Transfers to 0x1C0000+ are FMV/streaming buffers; skip them. */
    if (!step && size > 0 && load_start < 0x1C0000u) {
        int lba = cdrom_get_setloc_lba();
        if (lba >= 0) cd_dma_log_push(lba, load_start, size);

        /* B-1: capture overlay bytes into the write-once capture set.
         * overlay_capture_on_dma auto-activates after game handoff and is a
         * no-op unless the overlay cache is enabled in config. */
        extern uint8_t *memory_get_ram_ptr(void);
        uint8_t *ram = memory_get_ram_ptr();
        overlay_capture_on_dma(load_start, size, ram + load_start);
    }

    channels[3].madr = final_addr;
    cancel_async_transfer(3);
    complete_transfer(3);
}

static void advance_mdec_channel(int ch, uint32_t cycles) {
    DMAAsyncChannel *a = &mdec_async[ch];
    if (!a->active) return;
    if (!((channels[ch].chcr >> 24) & 1u) || !channel_enabled(ch)) return;

    uint32_t chcr = channels[ch].chcr;
    uint32_t direction = chcr & 1u;
    uint32_t step = (chcr >> 1) & 1u;
    int32_t addr_step = step ? -4 : 4;
    uint32_t cycles_per_word = (ch == 0) ? DMA_MDEC_IN_CYCLES_PER_WORD : DMA_MDEC_OUT_CYCLES_PER_WORD;

    if ((ch == 0 && direction == 0) || (ch == 1 && direction != 0)) {
        uint32_t words = a->total_words;
        uint32_t addr = channels[ch].madr & 0x1FFFFCu;
        finish_async_mdec_transfer(ch, addr, words);
        return;
    }

    if (ch == 0) {
        if (!mdec_dma_write_ready()) return;
    } else {
        if (!mdec_dma_read_ready()) return;
    }

    if (cycles > UINT32_MAX - a->cycles_accum) {
        a->cycles_accum = UINT32_MAX;
    } else {
        a->cycles_accum += cycles;
    }

    uint32_t words_budget = a->cycles_accum / cycles_per_word;
    if (words_budget == 0) return;

    uint32_t addr = channels[ch].madr & 0x1FFFFCu;
    uint32_t moved = 0;
    while (a->remaining_words > 0 && words_budget > 0) {
        if (ch == 0) {
            if (!mdec_dma_write_ready()) break;
            mdec_dma_write_word(psx_read_word(addr));
        } else {
            if (!mdec_dma_read_ready()) break;
            psx_write_word(addr, mdec_dma_read_word());
        }

        addr = (addr + addr_step) & 0x1FFFFCu;
        a->remaining_words--;
        words_budget--;
        moved++;
    }

    if (moved == 0) return;

    a->cycles_accum -= moved * cycles_per_word;
    channels[ch].madr = addr;

    if (a->remaining_words == 0) {
        finish_async_mdec_transfer(ch, addr, a->total_words);
    }
}

static void execute_ch0_mdec_in(void) {
    uint32_t chcr = channels[0].chcr;
    uint32_t direction = chcr & 1;           /* 1=from RAM to MDEC */
    uint32_t step = (chcr >> 1) & 1;
    uint32_t total_words = transfer_word_count(0);
    uint32_t addr = channels[0].madr & 0x1FFFFCu;
    int32_t addr_step = step ? -4 : 4;

    if (direction != 0) {
        mdec_debug_dma_in_start(addr, total_words);
        for (uint32_t i = 0; i < total_words; i++) {
            mdec_dma_write_word(psx_read_word(addr));
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
        mdec_debug_dma_in_end(addr, total_words);
        channels[0].madr = addr;
    }

    complete_transfer(0);
}

static void execute_ch1_mdec_out(void) {
    uint32_t chcr = channels[1].chcr;
    uint32_t direction = chcr & 1;           /* 0=from MDEC to RAM */
    uint32_t step = (chcr >> 1) & 1;
    uint32_t total_words = transfer_word_count(1);
    uint32_t addr = channels[1].madr & 0x1FFFFCu;
    int32_t addr_step = step ? -4 : 4;

    if (direction == 0) {
        mdec_debug_dma_out_start(addr, total_words);
        for (uint32_t i = 0; i < total_words; i++) {
            psx_write_word(addr, mdec_dma_read_word());
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
        mdec_debug_dma_out_end(addr, total_words);
        channels[1].madr = addr;
    }

    complete_transfer(1);
}

static uint32_t execute_ch2_gpu(void) {
    uint32_t chcr = channels[2].chcr;
    uint32_t direction = chcr & 1;           /* 0=to RAM, 1=from RAM (to device) */
    uint32_t step = (chcr >> 1) & 1;         /* 0=forward(+4), 1=backward(-4) */
    uint32_t sync_mode = (chcr >> 9) & 3;    /* 0=burst, 1=block, 2=linked-list */
    uint32_t actual_words = 0;

    if (direction == 0) {
        /* GPU → RAM (VRAM read): read pixel data via GPUREAD.
         * A prior GP0(C0h) command must have set up the VRAM read region. */
        if (sync_mode == 1) {
            uint32_t block_size = channels[2].bcr & 0xFFFF;
            uint32_t block_count = (channels[2].bcr >> 16) & 0xFFFF;
            uint32_t total_words = block_size * block_count;
            uint32_t addr = channels[2].madr & 0x1FFFFCu;
            int32_t  addr_step = step ? -4 : 4;
            for (uint32_t i = 0; i < total_words; i++) {
                uint32_t pixel_data = gpu_read_gpuread();
                psx_write_word(addr, pixel_data);
                addr = (addr + addr_step) & 0x1FFFFCu;
            }
            channels[2].madr = addr;
            actual_words = total_words;
        }
        return actual_words;
    }

    /* direction == 1: RAM → GPU */
    if (sync_mode == 1) {
        /* Block mode: BCR bits 0-15 = block size (words), bits 16-31 = block count */
        uint32_t block_size = channels[2].bcr & 0xFFFF;
        uint32_t block_count = (channels[2].bcr >> 16) & 0xFFFF;
        uint32_t total_words = block_size * block_count;
        uint32_t addr = channels[2].madr & 0x1FFFFCu; /* mask to RAM, word-aligned */
        int32_t  addr_step = step ? -4 : 4;

        for (uint32_t i = 0; i < total_words; i++) {
            uint32_t word = psx_read_word(addr);
            gpu_set_gp0_source(addr);
            gpu_write_gp0(word);
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
        channels[2].madr = addr;
        actual_words = total_words;
    } else if (sync_mode == 2) {
        /* Linked-list mode: follow ordering table in RAM.
         * Each node: bits 24-31 = number of words following header,
         *            bits 0-23  = next node address (0xFFFFFF = end).
         * The words following the header are sent to GP0. */
        uint32_t addr = channels[2].madr & 0x1FFFFCu;
        uint32_t safety = 0;
        const uint32_t MAX_NODES = 0x40000; /* prevent infinite loops */

        for (;;) {
            if (safety++ > MAX_NODES) {
                /* Abort this transfer — linked list has a cycle or is corrupt.
                 * Don't crash; the shell may recover on the next frame. */
                break;
            }

            uint32_t header = psx_read_word(addr);
            uint32_t num_words = (header >> 24) & 0xFF;
            uint32_t word_addr = (addr + 4) & 0x1FFFFCu;
            actual_words += 1u;

            for (uint32_t i = 0; i < num_words; i++) {
                uint32_t word = psx_read_word(word_addr);
                gpu_set_gp0_source(word_addr);
                gpu_write_gp0(word);
                word_addr = (word_addr + 4) & 0x1FFFFCu;
            }
            actual_words += num_words;

            /* Next node */
            uint32_t next = header & 0xFFFFFFu;
            if (next == 0xFFFFFFu) {
                channels[2].madr = 0x00FFFFFFu;
                break; /* end of list */
            }
            addr = next & 0x1FFFFCu;
        }
        if (safety > MAX_NODES) {
            channels[2].madr = addr;
        }
    } else {
        /* Burst mode (sync_mode == 0) */
        uint32_t word_count = channels[2].bcr & 0xFFFF;
        if (word_count == 0) word_count = 0x10000; /* 0 means 0x10000 */
        uint32_t addr = channels[2].madr & 0x1FFFFCu;
        int32_t  addr_step = step ? -4 : 4;

        for (uint32_t i = 0; i < word_count; i++) {
            uint32_t word = psx_read_word(addr);
            gpu_set_gp0_source(addr);
            gpu_write_gp0(word);
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
        channels[2].madr = addr;
        actual_words = word_count;
    }

    return actual_words;
}

static void execute_ch3_cdrom(void) {
    uint32_t chcr = channels[3].chcr;
    uint32_t direction = chcr & 1;           /* 0=to RAM, 1=from RAM */

    if (direction != 0) {
        channels[3].chcr &= ~((1u << 24) | (1u << 28));
        return;
    }

    start_async_cdrom_transfer();
}

static void execute_ch4_spu(void) {
    uint32_t chcr = channels[4].chcr;
    uint32_t direction = chcr & 1;           /* 1=from RAM to SPU, 0=SPU to RAM */
    uint32_t step = (chcr >> 1) & 1;
    uint32_t total_words = transfer_word_count(4);
    uint32_t addr = channels[4].madr & 0x1FFFFCu;
    int32_t addr_step = step ? -4 : 4;

    if (direction != 0) {
        for (uint32_t i = 0; i < total_words; i++) {
            spu_dma_write(psx_read_word(addr));
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
    } else {
        for (uint32_t i = 0; i < total_words; i++) {
            psx_write_word(addr, 0);
            addr = (addr + addr_step) & 0x1FFFFCu;
        }
    }

    channels[4].madr = addr;
    complete_transfer(4);
}

static void execute_ch6_otc(void) {
    /* OTC (Ordering Table Clear): writes a backward-linked list to RAM.
     * Node N = address of node N-1, node 0 = 0xFFFFFF (end marker).
     * Direction is always to-RAM, step is always backward.
     * BCR bits 0-15 = number of entries. */
    uint32_t num_entries = channels[6].bcr & 0xFFFF;
    if (num_entries == 0) num_entries = 0x10000;
    uint32_t addr = channels[6].madr & 0x1FFFFCu;

    for (uint32_t i = 0; i < num_entries; i++) {
        uint32_t val;
        if (i == num_entries - 1) {
            /* Last entry (first in memory): end marker */
            val = 0x00FFFFFFu;
        } else {
            /* Points to the previous entry (addr - 4) */
            val = (addr - 4) & 0x00FFFFFFu;
        }
        psx_write_word(addr, val);
        addr = (addr - 4) & 0x1FFFFCu;
    }

    complete_transfer(6);
}

static void try_execute(int ch) {
    uint32_t chcr = channels[ch].chcr;

    /* Transfer starts when bit 24 (start/busy) is set AND channel is enabled in DPCR */
    if (!((chcr >> 24) & 1)) return;
    if (!channel_enabled(ch)) return;

    channels[ch].chcr &= ~(1u << 28);
    trace_dma('S', ch, transfer_word_count(ch), dicr, i_stat);
    event_ring_record_aux(EV_DMA_KICK, (uint8_t)ch, channels[ch].chcr);
    event_ring_record_aux(EV_ENQ, (uint8_t)(SRC_DMA0 + ch), transfer_word_count(ch));

    switch (ch) {
        case 0:
            start_async_mdec_transfer(0);
            break;
        case 1:
            start_async_mdec_transfer(1);
            break;
        case 2:
            schedule_delayed_complete(2, execute_ch2_gpu(),
                                      DMA_GPU_CYCLES_PER_WORD);
            break;
        case 3:
            execute_ch3_cdrom();
            break;
        case 4:
            execute_ch4_spu();
            break;
        case 6:
            execute_ch6_otc();
            break;
        default:
            /* Other channels not implemented yet — fatal if transfer is triggered */
            fprintf(stderr, "DMA ch%d: transfer triggered but not implemented (CHCR=0x%08X)\n", ch, chcr);
            fflush(stderr);
            exit(1);
    }
}

/* ---- Public interface ---- */

uint32_t dma_get_dicr(void) { update_master_irq(); return dicr; }
uint32_t dma_get_dpcr(void) { return dpcr; }

void dma_advance(uint32_t cycles) {
    if (cycles == 0) return;
    advance_mdec_channel(0, cycles);
    advance_mdec_channel(1, cycles);
    DMAAsyncChannel *a = &cdrom_async;
    if (a->active && ((channels[3].chcr >> 24) & 1u) && channel_enabled(3)) {
        uint32_t chcr = channels[3].chcr;
        uint32_t direction = chcr & 1u;
        uint32_t step = (chcr >> 1) & 1u;
        int32_t addr_step = step ? -4 : 4;

        if (direction != 0) {
            cancel_async_transfer(3);
            channels[3].chcr &= ~((1u << 24) | (1u << 28));
        } else if (cdrom_dma_ready()) {
            if (cycles > UINT32_MAX - a->cycles_accum) {
                a->cycles_accum = UINT32_MAX;
            } else {
                a->cycles_accum += cycles;
            }

            uint32_t words_budget = a->cycles_accum / DMA_CDROM_CYCLES_PER_WORD;
            uint32_t addr = channels[3].madr & 0x1FFFFCu;
            uint32_t moved = 0;
            while (a->remaining_words > 0 && words_budget > 0 && cdrom_dma_ready()) {
                uint32_t word = cdrom_dma_read();
                psx_write_word(addr, word);
                dirty_ram_mark_executable_range(addr, 4);
                addr = (addr + addr_step) & 0x1FFFFCu;
                a->remaining_words--;
                words_budget--;
                moved++;
            }

            if (moved > 0) {
                a->cycles_accum -= moved * DMA_CDROM_CYCLES_PER_WORD;
                channels[3].madr = addr;
                if (a->remaining_words == 0) {
                    finish_async_cdrom_transfer(addr);
                }
            }
        }
    }
    advance_delayed_complete(2, cycles);
}

void dma_init(void) {
    memset(channels, 0, sizeof(channels));
    memset(mdec_async, 0, sizeof(mdec_async));
    memset(&cdrom_async, 0, sizeof(cdrom_async));
    memset(delayed_complete, 0, sizeof(delayed_complete));
    dpcr = 0x07654321u; /* default: priorities set, no channels enabled */
    dicr = 0;
    dma_debug_clear_trace();
}

uint32_t dma_read(uint32_t addr) {
    /* DPCR */
    if (addr == 0x1F8010F0u) return dpcr;
    /* DICR */
    if (addr == 0x1F8010F4u) {
        update_master_irq();
        return dicr;
    }

    /* Per-channel registers: 0x1F801080 + ch*0x10 + offset */
    if (addr >= 0x1F801080u && addr <= 0x1F8010EFu) {
        uint32_t offset = addr - 0x1F801080u;
        int ch = offset / 0x10;
        int reg = offset % 0x10;

        if (ch > 6) goto bad;
        switch (reg) {
            case 0x00: return channels[ch].madr;
            case 0x04: return channels[ch].bcr;
            case 0x08: return channels[ch].chcr;
            case 0x0C: return 0;
            default: goto bad;
        }
    }

bad:
    fprintf(stderr, "DMA read from unknown address 0x%08X\n", addr);
    fflush(stderr);
    exit(1);
    return 0;
}

void dma_write_masked(uint32_t addr, uint32_t val, uint32_t mask) {
    /* DPCR */
    if (addr == 0x1F8010F0u) {
        dpcr = (dpcr & ~mask) | (val & mask);
        return;
    }
    /* DICR: selected low/control bits are writable; bits 24-30 are write-1-to-acknowledge. */
    if (addr == 0x1F8010F4u) {
        /* Bits 0-5: unknown/unused but writable */
        /* Bits 6-14: unused/read-only */
        /* Bit 15: bus-error flag */
        /* Bits 16-22: per-channel IRQ enable */
        /* Bit 23: master IRQ enable */
        /* Bits 24-30: IRQ flags, write 1 to clear */
        /* Bit 31: master flag, read-only (computed) */
        uint32_t write_mask = DICR_WRITE_MASK & mask;
        uint32_t reset_mask = DICR_RESET_MASK & mask;
        dicr = (dicr & ~write_mask) | (val & write_mask);
        dicr &= ~(val & reset_mask);
        update_master_irq();
        return;
    }

    /* Per-channel registers */
    if (addr >= 0x1F801080u && addr <= 0x1F8010EFu) {
        uint32_t offset = addr - 0x1F801080u;
        int ch = offset / 0x10;
        int reg = offset % 0x10;

        if (ch > 6) goto bad;
        switch (reg) {
            case 0x00:
                channels[ch].madr = (channels[ch].madr & ~mask) | (val & mask);
                return;
            case 0x04:
                channels[ch].bcr = (channels[ch].bcr & ~mask) | (val & mask);
                return;
            case 0x08:
                channels[ch].chcr = (channels[ch].chcr & ~mask) | (val & mask);
                if ((mask & (1u << 24)) && !((channels[ch].chcr >> 24) & 1u)) {
                    cancel_async_transfer(ch);
                }
                /* Writing CHCR's start bit set triggers transfer. */
                if ((mask & (1u << 24)) && ((channels[ch].chcr >> 24) & 1)) {
                    try_execute(ch);
                }
                return;
            case 0x0C:
                return;
            default:
                goto bad;
        }
    }

bad:
    fprintf(stderr, "DMA write to unknown address 0x%08X = 0x%08X\n", addr, val);
    fflush(stderr);
    exit(1);
}

void dma_write(uint32_t addr, uint32_t val) {
    dma_write_masked(addr, val, 0xFFFFFFFFu);
}

uint64_t dma_debug_get_trace(const DMATraceEntry** out_entries) {
    if (out_entries) *out_entries = dma_trace;
    return dma_trace_seq;
}

void dma_debug_clear_trace(void) {
    memset(dma_trace, 0, sizeof(dma_trace));
    dma_trace_seq = 0;
}

void dma_debug_get_state(DMADebugState* out) {
    if (!out) return;
    out->dpcr = dpcr;
    out->dicr = dma_get_dicr();
    for (int i = 0; i < 7; i++) {
        out->channels[i].madr = channels[i].madr;
        out->channels[i].bcr = channels[i].bcr;
        out->channels[i].chcr = channels[i].chcr;
        out->channels[i].active =
            ((i < 2) ? mdec_async[i].active : 0) ||
            ((i == 3) ? cdrom_async.active : 0) ||
            delayed_complete[i].active;
        out->channels[i].remaining_words =
            (i < 2 && mdec_async[i].active) ? mdec_async[i].remaining_words :
            (i == 3 && cdrom_async.active) ? cdrom_async.remaining_words :
            delayed_complete[i].total_words;
        out->channels[i].cycles_accum =
            (i < 2 && mdec_async[i].active) ? mdec_async[i].cycles_accum :
            (i == 3 && cdrom_async.active) ? cdrom_async.cycles_accum :
            delayed_complete[i].cycles_remaining;
    }
}
