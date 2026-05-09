/*
 * spu.c - PS1 Sound Processing Unit register and direct ADPCM voice model.
 *
 * This is intentionally still a compact hardware model: it accepts SPU
 * register reads/writes, DMA4 transfers into 512KB SPU RAM, and mixes the
 * 24 direct ADPCM voices. Reverb, noise, sweep volumes, XA/CD input and IRQ
 * timing are not modeled yet.
 */

#include "spu.h"

#include <string.h>

#define SPU_RAM_SIZE       (512 * 1024)
#define SPU_REG_COUNT      256
#define SPU_VOICE_COUNT    24
#define SPU_BLOCK_SAMPLES  28

static uint8_t  spu_ram[SPU_RAM_SIZE];
static uint16_t spu_regs[SPU_REG_COUNT];
static uint32_t transfer_addr;
static uint32_t key_on_count;
static uint64_t render_frames;
static uint64_t nonzero_frames;
static int32_t last_peak;
static int32_t peak;

typedef struct {
    int active;
    uint32_t cur_addr;
    uint32_t repeat_addr;
    int16_t samples[SPU_BLOCK_SAMPLES];
    int sample_idx;
    uint32_t phase;
    int16_t hist1;
    int16_t hist2;
    uint8_t flags;
} SpuVoice;

static SpuVoice voices[SPU_VOICE_COUNT];

static inline int16_t clamp16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static inline int32_t abs32(int32_t v) {
    return v < 0 ? -v : v;
}

static inline uint32_t reg_index(uint32_t addr) {
    return (addr - 0x1F801C00u) >> 1;
}

static inline uint16_t voice_reg(int voice, int reg) {
    return spu_regs[(uint32_t)voice * 8u + (uint32_t)reg];
}

static inline int16_t direct_volume(uint16_t raw) {
    int32_t v;
    if (raw & 0x8000u) {
        /* Sweep mode is not modeled; use the magnitude as a direct volume. */
        v = (int32_t)(raw & 0x7FFFu);
    } else {
        v = (int32_t)(raw & 0x7FFFu);
        if (v & 0x4000) v -= 0x8000;
    }
    if (v > 0x3FFF) v = 0x3FFF;
    if (v < -0x4000) v = -0x4000;
    return (int16_t)v;
}

static void decode_block(SpuVoice *v) {
    static const int f0[5] = { 0, 60, 115, 98, 122 };
    static const int f1[5] = { 0, 0, -52, -55, -60 };

    uint32_t addr = v->cur_addr & (SPU_RAM_SIZE - 1u);
    if (addr + 16u > SPU_RAM_SIZE) addr = 0;

    uint8_t header = spu_ram[addr + 0u];
    uint8_t flags = spu_ram[addr + 1u];
    int shift = header & 0x0F;
    int filter = (header >> 4) & 0x0F;
    if (filter > 4) filter = 0;
    if (shift > 12) shift = 12;

    int out = 0;
    for (int b = 0; b < 14; b++) {
        uint8_t packed = spu_ram[addr + 2u + (uint32_t)b];
        for (int n = 0; n < 2; n++) {
            int sample4 = (n == 0) ? (packed & 0x0F) : (packed >> 4);
            if (sample4 & 0x08) sample4 -= 0x10;

            int32_t s = sample4 << 12;
            s >>= shift;
            s += ((int32_t)v->hist1 * f0[filter] +
                  (int32_t)v->hist2 * f1[filter] + 32) >> 6;
            s = clamp16(s);
            v->hist2 = v->hist1;
            v->hist1 = (int16_t)s;
            v->samples[out++] = (int16_t)s;
        }
    }

    if (flags & 0x04u) v->repeat_addr = addr;
    v->flags = flags;
    v->sample_idx = 0;
    v->cur_addr = (addr + 16u) & (SPU_RAM_SIZE - 1u);
}

static int16_t voice_next_sample(int idx) {
    SpuVoice *v = &voices[idx];
    if (!v->active) return 0;

    if (v->sample_idx >= SPU_BLOCK_SAMPLES) {
        if (v->flags & 0x01u) {
            if (v->flags & 0x02u) {
                v->cur_addr = v->repeat_addr & (SPU_RAM_SIZE - 1u);
            } else {
                v->active = 0;
                return 0;
            }
        }
        decode_block(v);
    }

    int16_t s = v->samples[v->sample_idx];
    uint32_t pitch = voice_reg(idx, 2) & 0x3FFFu;
    if (pitch == 0) pitch = 0x1000u;
    v->phase += pitch;
    while (v->phase >= 0x1000u) {
        v->phase -= 0x1000u;
        v->sample_idx++;
        if (v->sample_idx >= SPU_BLOCK_SAMPLES) break;
    }
    return s;
}

static void key_on(uint32_t mask) {
    for (int i = 0; i < SPU_VOICE_COUNT; i++) {
        if (!(mask & (1u << i))) continue;
        SpuVoice *v = &voices[i];
        memset(v, 0, sizeof(*v));
        v->active = 1;
        v->cur_addr = ((uint32_t)voice_reg(i, 3) << 3) & (SPU_RAM_SIZE - 1u);
        v->repeat_addr = ((uint32_t)voice_reg(i, 7) << 3) & (SPU_RAM_SIZE - 1u);
        v->sample_idx = SPU_BLOCK_SAMPLES;
        key_on_count++;
    }
}

static void key_off(uint32_t mask) {
    for (int i = 0; i < SPU_VOICE_COUNT; i++) {
        if (mask & (1u << i)) voices[i].active = 0;
    }
}

void spu_init(void) {
    memset(spu_ram, 0, sizeof(spu_ram));
    memset(spu_regs, 0, sizeof(spu_regs));
    memset(voices, 0, sizeof(voices));
    transfer_addr = 0;
    key_on_count = 0;
    render_frames = 0;
    nonzero_frames = 0;
    last_peak = 0;
    peak = 0;
}

void spu_render(int16_t* out_stereo, int frames) {
    if (!out_stereo || frames <= 0) return;

    uint16_t ctrl = spu_regs[reg_index(0x1F801DAAu)];
    int enabled = (ctrl & 0x8000u) != 0;
    int16_t main_l = direct_volume(spu_regs[reg_index(0x1F801D80u)]);
    int16_t main_r = direct_volume(spu_regs[reg_index(0x1F801D82u)]);

    int32_t block_peak = 0;
    for (int f = 0; f < frames; f++) {
        int32_t mix_l = 0;
        int32_t mix_r = 0;

        if (enabled) {
            for (int v = 0; v < SPU_VOICE_COUNT; v++) {
                int16_t s = voice_next_sample(v);
                if (!s) continue;
                int16_t vl = direct_volume(voice_reg(v, 0));
                int16_t vr = direct_volume(voice_reg(v, 1));
                mix_l += ((int32_t)s * vl) >> 14;
                mix_r += ((int32_t)s * vr) >> 14;
            }
            mix_l = (mix_l * main_l) >> 14;
            mix_r = (mix_r * main_r) >> 14;
        }

        out_stereo[f * 2 + 0] = clamp16(mix_l);
        out_stereo[f * 2 + 1] = clamp16(mix_r);
        int32_t frame_peak = abs32(out_stereo[f * 2 + 0]);
        int32_t right_peak = abs32(out_stereo[f * 2 + 1]);
        if (right_peak > frame_peak) frame_peak = right_peak;
        if (frame_peak) nonzero_frames++;
        if (frame_peak > block_peak) block_peak = frame_peak;
    }
    render_frames += (uint64_t)frames;
    last_peak = block_peak;
    if (block_peak > peak) peak = block_peak;
}

void spu_debug_info(SpuDebugInfo* out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->ctrl = spu_regs[reg_index(0x1F801DAAu)];
    out->main_l = direct_volume(spu_regs[reg_index(0x1F801D80u)]);
    out->main_r = direct_volume(spu_regs[reg_index(0x1F801D82u)]);
    for (int i = 0; i < SPU_VOICE_COUNT; i++) {
        if (voices[i].active) out->active_mask |= (1u << i);
    }
    out->key_on_count = key_on_count;
    out->render_frames = render_frames;
    out->nonzero_frames = nonzero_frames;
    out->last_peak = last_peak;
    out->peak = peak;
}

uint32_t spu_read(uint32_t addr) {
    if (addr >= 0x1F801C00u && addr <= 0x1F801DFFu) {
        uint32_t idx = reg_index(addr);
        if (idx < SPU_REG_COUNT) {
            if (addr == 0x1F801DAEu) {
                return 0x0400; /* SPUSTAT: ready */
            }
            if (addr == 0x1F801D9Cu) {
                uint32_t bits = 0;
                for (int i = 0; i < 16; i++)
                    if (voices[i].active) bits |= (1u << i);
                return bits;
            }
            if (addr == 0x1F801D9Eu) {
                uint32_t bits = 0;
                for (int i = 16; i < SPU_VOICE_COUNT; i++)
                    if (voices[i].active) bits |= (1u << (i - 16));
                return bits;
            }
            return spu_regs[idx];
        }
    }

    return 0;
}

void spu_write(uint32_t addr, uint32_t value) {
    if (addr >= 0x1F801C00u && addr <= 0x1F801DFFu) {
        uint32_t idx = reg_index(addr);
        if (idx < SPU_REG_COUNT) {
            spu_regs[idx] = (uint16_t)value;

            if (addr == 0x1F801D88u) key_on((uint32_t)(uint16_t)value);
            if (addr == 0x1F801D8Au) key_on((uint32_t)(uint16_t)value << 16);
            if (addr == 0x1F801D8Cu) key_off((uint32_t)(uint16_t)value);
            if (addr == 0x1F801D8Eu) key_off((uint32_t)(uint16_t)value << 16);

            if (addr == 0x1F801DA6u) {
                transfer_addr = ((uint32_t)(uint16_t)value) << 3;
                if (transfer_addr >= SPU_RAM_SIZE) transfer_addr = 0;
            }

            if (addr == 0x1F801DA8u) {
                if (transfer_addr + 1 < SPU_RAM_SIZE) {
                    spu_ram[transfer_addr]     = (uint8_t)(value & 0xFF);
                    spu_ram[transfer_addr + 1] = (uint8_t)((value >> 8) & 0xFF);
                }
                transfer_addr = (transfer_addr + 2) % SPU_RAM_SIZE;
            }
        }
    }
}

void spu_dma_write(uint32_t word) {
    if (transfer_addr + 3 < SPU_RAM_SIZE) {
        spu_ram[transfer_addr]     = (uint8_t)(word & 0xFF);
        spu_ram[transfer_addr + 1] = (uint8_t)((word >> 8) & 0xFF);
        spu_ram[transfer_addr + 2] = (uint8_t)((word >> 16) & 0xFF);
        spu_ram[transfer_addr + 3] = (uint8_t)((word >> 24) & 0xFF);
    }
    transfer_addr = (transfer_addr + 4) % SPU_RAM_SIZE;
}

int spu_dma_ready(void) {
    return 1;
}

const uint8_t* spu_get_ram(void) {
    return spu_ram;
}
