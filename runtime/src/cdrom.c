/*
 * cdrom.c — PS1 CD-ROM controller (adapted from psxrecomp-v3)
 *
 * Registers at 0x1F801800-0x1F801803 with index-based register banking.
 * Reference: nocash PSX specs — CDROM Controller section
 *
 * v4 adaptation: removed event_deliver (HLE), removed CPUState dependency,
 * removed hard-coded RAM writes, removed fprintf. IRQ delivery is via
 * i_stat bit 2 (IRQ_CDROM); the recompiled BIOS exception handler
 * processes events natively.
 */

#include "cdrom.h"
#include <string.h>
#include <stdlib.h>

/* C wrappers for the C++ ISOReader (defined in iso_reader_c.cpp) */
extern void* iso_open(const char* path);
extern int iso_read_sector(void* handle, uint32_t lba, uint8_t* buffer, int size);
extern void iso_close(void* handle);

/* I_STAT owned by memory.c — set bit 2 for CDROM IRQ */
extern uint32_t i_stat;

/* CD-ROM state */
static uint8_t index_reg;
static uint8_t stat_reg;
static uint8_t irq_enable;
static uint8_t irq_flag;

/* Parameter FIFO */
#define PARAM_FIFO_SIZE 16
static uint8_t param_fifo[PARAM_FIFO_SIZE];
static int param_count;

/* Response FIFO */
#define RESPONSE_FIFO_SIZE 16
static uint8_t response_fifo[RESPONSE_FIFO_SIZE];
static int response_read;
static int response_count;

/* Data buffer (one sector = 2048 bytes for mode 2) */
#define SECTOR_SIZE 2048
static uint8_t sector_buffer[SECTOR_SIZE];
static int sector_read_pos;
static int sector_available;

/* Seek target */
static uint8_t seek_min, seek_sec, seek_sect;

/* Read state */
static int reading;
static int read_min, read_sec, read_sect;

/* Pending command */
typedef struct {
    uint8_t cmd;
    int pending;
    int delay;
    int phase;
} PendingCmd;
static PendingCmd pending;

/* ISO reader */
static void* iso_handle = NULL;

static int has_disc(void) {
    return iso_handle != NULL;
}

/* CD status bits */
#define CDSTAT_ERROR    0x01
#define CDSTAT_MOTOR    0x02
#define CDSTAT_SEEKERR  0x04
#define CDSTAT_IDERROR  0x08
#define CDSTAT_SHELL    0x10
#define CDSTAT_READ     0x20
#define CDSTAT_SEEK     0x40
#define CDSTAT_PLAY     0x80

/* IRQ types */
#define CDIRQ_DATA_READY  1
#define CDIRQ_COMPLETE    2
#define CDIRQ_ACK         3
#define CDIRQ_DATA_END    4
#define CDIRQ_ERROR       5

static void response_clear(void) {
    response_read = 0;
    response_count = 0;
}

static void response_push(uint8_t val) {
    if (response_count < RESPONSE_FIFO_SIZE) {
        response_fifo[response_count++] = val;
    }
}

static void set_irq(int type) {
    irq_flag = (uint8_t)type;
}

/* Fire CDROM IRQ into the interrupt controller */
static void fire_cdrom_irq(void) {
    if (irq_flag && (irq_enable & (1 << (irq_flag - 1)))) {
        i_stat |= (1u << 2); /* IRQ_CDROM */
    }
}

static int bcd_to_bin(uint8_t bcd) {
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

static int msf_to_lba(int m, int s, int f) {
    return (m * 60 + s) * 75 + f - 150;
}

static void read_sector_at(int min, int sec, int sect) {
    int lba = msf_to_lba(min, sec, sect);
    if (iso_handle) {
        iso_read_sector(iso_handle, lba, sector_buffer, SECTOR_SIZE);
    } else {
        memset(sector_buffer, 0, SECTOR_SIZE);
    }
    sector_read_pos = 0;
    sector_available = 1;
}

static void advance_msf(int* m, int* s, int* f) {
    (*f)++;
    if (*f >= 75) { *f = 0; (*s)++; }
    if (*s >= 60) { *s = 0; (*m)++; }
}

static void exec_command(uint8_t cmd) {
    response_clear();

    switch (cmd) {
    case 0x01: /* GetStat */
        if (has_disc()) {
            stat_reg |= CDSTAT_MOTOR;
        } else {
            stat_reg |= CDSTAT_SHELL;
        }
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        break;

    case 0x02: /* SetLoc */
        if (param_count >= 3) {
            seek_min = bcd_to_bin(param_fifo[0]);
            seek_sec = bcd_to_bin(param_fifo[1]);
            seek_sect = bcd_to_bin(param_fifo[2]);
        }
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        break;

    case 0x06: /* ReadN */
        if (!has_disc()) {
            response_push(stat_reg | CDSTAT_ERROR);
            set_irq(CDIRQ_ERROR);
            break;
        }
        read_min = seek_min;
        read_sec = seek_sec;
        read_sect = seek_sect;
        reading = 1;
        stat_reg |= CDSTAT_READ;
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = 0x06;
        pending.pending = 1;
        pending.delay = 50000;
        pending.phase = 1;
        break;

    case 0x09: /* Pause */
        reading = 0;
        stat_reg &= ~CDSTAT_READ;
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = 0x09;
        pending.pending = 1;
        pending.delay = 10000;
        pending.phase = 1;
        break;

    case 0x0A: /* Init */
        reading = 0;
        stat_reg = has_disc() ? CDSTAT_MOTOR : CDSTAT_SHELL;
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = 0x0A;
        pending.pending = 1;
        pending.delay = 50000;
        pending.phase = 1;
        break;

    case 0x0C: /* Demute */
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        break;

    case 0x0E: /* SetMode */
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        break;

    case 0x15: /* SeekL */
        if (!has_disc()) {
            response_push(stat_reg | CDSTAT_ERROR | CDSTAT_SEEKERR);
            set_irq(CDIRQ_ERROR);
            break;
        }
        stat_reg |= CDSTAT_SEEK;
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = 0x15;
        pending.pending = 1;
        pending.delay = 20000;
        pending.phase = 1;
        break;

    case 0x1A: /* GetID */
        /* GetID always sends INT3 (ACK) first, then a pending
         * second response: INT2 (COMPLETE) with disc ID if present,
         * or INT5 (ERROR) if no disc / lid open. */
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = 0x1A;
        pending.pending = 1;
        pending.delay = 30000;
        pending.phase = 1;
        break;

    case 0x1B: /* ReadS */
        if (!has_disc()) {
            response_push(stat_reg | CDSTAT_ERROR);
            set_irq(CDIRQ_ERROR);
            break;
        }
        read_min = seek_min;
        read_sec = seek_sec;
        read_sect = seek_sect;
        reading = 1;
        stat_reg |= CDSTAT_READ;
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = 0x1B;
        pending.pending = 1;
        pending.delay = 50000;
        pending.phase = 1;
        break;

    case 0x1E: /* ReadTOC */
        response_push(stat_reg);
        set_irq(CDIRQ_ACK);
        pending.cmd = 0x1E;
        pending.pending = 1;
        pending.delay = 100000;
        pending.phase = 1;
        break;

    case 0x19: /* Test */
        if (param_count >= 1 && param_fifo[0] == 0x20) {
            response_push(0x97);
            response_push(0x01);
            response_push(0x10);
            response_push(0xC2);
            set_irq(CDIRQ_ACK);
        } else {
            response_push(stat_reg);
            set_irq(CDIRQ_ACK);
        }
        break;

    default:
        response_push(stat_reg | CDSTAT_ERROR);
        set_irq(CDIRQ_ERROR);
        break;
    }

    param_count = 0;

    /* Fire the CDROM IRQ for the immediate response.
     * Delayed responses (pending) fire from process_pending(). */
    fire_cdrom_irq();
}

static void process_pending(void) {
    if (!pending.pending) return;

    pending.delay -= 33868;
    if (pending.delay > 0) return;

    pending.pending = 0;
    response_clear();

    switch (pending.cmd) {
    case 0x06: /* ReadN data ready */
    case 0x1B: /* ReadS data ready */
        if (reading) {
            read_sector_at(read_min, read_sec, read_sect);
            advance_msf(&read_min, &read_sec, &read_sect);
            response_push(stat_reg);
            set_irq(CDIRQ_DATA_READY);
            fire_cdrom_irq();
        }
        break;

    case 0x09: /* Pause complete */
        stat_reg &= ~CDSTAT_READ;
        response_push(stat_reg);
        set_irq(CDIRQ_COMPLETE);
        fire_cdrom_irq();
        break;

    case 0x0A: /* Init complete */
        response_push(stat_reg);
        set_irq(CDIRQ_COMPLETE);
        fire_cdrom_irq();
        break;

    case 0x15: /* SeekL complete */
        stat_reg &= ~CDSTAT_SEEK;
        response_push(stat_reg);
        set_irq(CDIRQ_COMPLETE);
        fire_cdrom_irq();
        break;

    case 0x1A: { /* GetID result */
        if (!has_disc()) {
            response_push(stat_reg | CDSTAT_IDERROR);
            response_push(0x80);
            set_irq(CDIRQ_ERROR);
        } else {
            response_push(stat_reg);
            response_push(0x00);
            response_push(0x20);
            response_push(0x00);
            response_push('S');
            response_push('C');
            response_push('E');
            response_push('I');
            set_irq(CDIRQ_COMPLETE);
        }
        fire_cdrom_irq();
        break;
    }

    case 0x1E: /* ReadTOC complete */
        response_push(stat_reg);
        set_irq(CDIRQ_COMPLETE);
        fire_cdrom_irq();
        break;

    default:
        break;
    }
}

void cdrom_init(const char* cue_path) {
    memset(param_fifo, 0, sizeof(param_fifo));
    memset(response_fifo, 0, sizeof(response_fifo));
    memset(sector_buffer, 0, sizeof(sector_buffer));

    index_reg = 0;
    irq_enable = 0x1F;
    irq_flag = 0;
    param_count = 0;
    response_read = 0;
    response_count = 0;
    sector_read_pos = 0;
    sector_available = 0;
    reading = 0;
    pending.pending = 0;
    seek_min = seek_sec = seek_sect = 0;

    if (cue_path) {
        iso_handle = iso_open(cue_path);
    }

    stat_reg = has_disc() ? CDSTAT_MOTOR : CDSTAT_SHELL;
}

uint32_t cdrom_read(uint32_t addr) {
    switch (addr) {
    case 0x1F801800: {
        uint8_t s = index_reg & 0x03;
        s |= (1 << 2); /* ADPCM empty */
        if (param_count == 0) s |= (1 << 3);
        if (param_count < PARAM_FIFO_SIZE) s |= (1 << 4);
        if (response_read < response_count) s |= (1 << 5);
        if (sector_available) s |= (1 << 6);
        return s;
    }

    case 0x1F801801:
        if (response_read < response_count) {
            return response_fifo[response_read++];
        }
        return 0;

    case 0x1F801802:
        if (sector_available && sector_read_pos < SECTOR_SIZE) {
            return sector_buffer[sector_read_pos++];
        }
        return 0;

    case 0x1F801803:
        if (index_reg == 0 || index_reg == 2) {
            return irq_enable;
        } else {
            return irq_flag | 0xE0;
        }

    default:
        return 0;
    }
}

void cdrom_write(uint32_t addr, uint32_t value) {
    uint8_t val = (uint8_t)value;

    switch (addr) {
    case 0x1F801800:
        index_reg = val & 0x03;
        break;

    case 0x1F801801:
        if (index_reg == 0) {
            exec_command(val);
            fire_cdrom_irq();
        }
        break;

    case 0x1F801802:
        if (index_reg == 0) {
            if (param_count < PARAM_FIFO_SIZE) {
                param_fifo[param_count++] = val;
            }
        } else if (index_reg == 1) {
            irq_enable = val & 0x1F;
        }
        break;

    case 0x1F801803:
        if (index_reg == 1) {
            irq_flag &= ~(val & 0x1F);
            if (val & 0x40) {
                param_count = 0;
            }
        }
        break;

    default:
        break;
    }
}

void cdrom_tick(void) {
    process_pending();

    if (reading && !sector_available && !pending.pending) {
        pending.cmd = 0x06;
        pending.pending = 1;
        pending.delay = 30000;
        pending.phase = 1;
    }
}

uint32_t cdrom_dma_read(void) {
    if (sector_available && sector_read_pos + 4 <= SECTOR_SIZE) {
        uint32_t val;
        memcpy(&val, sector_buffer + sector_read_pos, 4);
        sector_read_pos += 4;
        if (sector_read_pos >= SECTOR_SIZE) {
            sector_available = 0;
        }
        return val;
    }
    return 0;
}

int cdrom_dma_ready(void) {
    return sector_available && (sector_read_pos < SECTOR_SIZE);
}
