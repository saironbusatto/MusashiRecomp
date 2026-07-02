/* fntrace.c — runtime side of psx_dispatch call ring. See fntrace.h. */

#include "fntrace.h"
#include <string.h>
#include <stdlib.h>

FntraceEntry g_fntrace_ring[FNTRACE_RING_CAP];
uint64_t     g_fntrace_seq = 0;

/* Frame counter shared with debug_server.c / dirty_ram_interp.c. */
extern uint64_t s_frame_count;

static uint32_t s_arm_targets[FNTRACE_ARM_MAX];
static uint32_t s_arm_count = 0;
static uint32_t s_arm_record_all = 0;  /* opt-in via fntrace_arm(0xFFFFFFFF) */

/* One-shot game-start detection: fire cdrom_notify_game_started() the first
 * time the game's entry_pc is dispatched. Using entry_pc (not a range) avoids
 * false-triggering on the BIOS shell, which runs from RAM 0x30000-0x5B000 —
 * overlapping the game text range but never at entry_pc. */
static uint32_t s_game_entry_phys = 0;
static int      s_game_started = 0;
extern void cdrom_notify_game_started(void);
extern void boot_state_trigger_capture(const CPUState* cpu);

void fntrace_set_game_range(uint32_t lo, uint32_t hi) {
    /* lo is treated as entry_pc; hi is ignored (kept for API compat). */
    (void)hi;
    s_game_entry_phys = lo & 0x1FFFFFFFu;
    s_game_started = 0;
}

int fntrace_is_game_started(void) { return s_game_started; }

static inline int armed_match(uint32_t target) {
    if (s_arm_record_all) return 1;
    /* Hot path: when nothing is armed, record nothing. Recording every
     * dispatch by default makes the ring fill in seconds and burns ~10%
     * of host CPU. Investigators arm specific targets — see
     * fntrace_arm(0xFFFFFFFF) for the legacy "record-all" behavior. */
    if (s_arm_count == 0) return 0;
    for (uint32_t i = 0; i < s_arm_count; i++) {
        if (s_arm_targets[i] == target) return 1;
    }
    return 0;
}

void fntrace_record(CPUState* cpu, uint32_t target) {
    if (!s_game_started && s_game_entry_phys != 0) {
        if ((target & 0x1FFFFFFFu) == s_game_entry_phys) {
            s_game_started = 1;
            cdrom_notify_game_started();
            boot_state_trigger_capture(cpu);
        }
    }
    /* Honor the one-shot capture freeze (insn_freeze): once latched, the ring
     * preserves the pre-divergence window instead of evicting it. */
    extern int g_insn_log_frozen;
    int was_frozen = g_insn_log_frozen;
    /* Null-page dispatch guard: a dispatch whose phys target sits below the
     * exception vectors (0x80/0xA0) is a jump through a null/garbage pointer —
     * the wedge itself, never legitimate code. Latch the capture freeze so the
     * ring preserves the window that led here (this culprit entry, with its
     * ra, still records; the wedge storm after it does not). */
    if ((target & 0x1FFFFFFFu) < 0x40u) g_insn_log_frozen = 1;
    if (!armed_match(target)) return;
    if (was_frozen) return;
    uint64_t idx = g_fntrace_seq++ & (FNTRACE_RING_CAP - 1);
    FntraceEntry* e = &g_fntrace_ring[idx];
    e->frame  = (uint32_t)s_frame_count;
    e->target = target;
    e->ra     = cpu->gpr[31];
    e->a0     = cpu->gpr[4];
    e->a1     = cpu->gpr[5];
    e->a2     = cpu->gpr[6];
    e->a3     = cpu->gpr[7];
    e->s3     = cpu->gpr[19];
    e->sp     = cpu->gpr[29];
}

void fntrace_arm(uint32_t target) {
    if (target == 0) { fntrace_arm_clear(); return; }
    if (target == 0xFFFFFFFFu) { s_arm_record_all = 1; return; }
    /* Dedup: don't double-add. */
    for (uint32_t i = 0; i < s_arm_count; i++) {
        if (s_arm_targets[i] == target) return;
    }
    if (s_arm_count >= FNTRACE_ARM_MAX) return;
    s_arm_targets[s_arm_count++] = target;
}

void fntrace_arm_clear(void) {
    s_arm_count = 0;
    s_arm_record_all = 0;
    memset(s_arm_targets, 0, sizeof(s_arm_targets));
}

uint32_t fntrace_arm_count(void) { return s_arm_count; }
uint32_t fntrace_arm_get(uint32_t i) {
    return (i < s_arm_count) ? s_arm_targets[i] : 0;
}

void fntrace_arm_from_env(const char *env_name) {
    const char *spec = getenv(env_name);
    if (!spec || !*spec) return;

    const char *p = spec;
    while (*p) {
        while (*p == ',' || *p == ';' || *p == ' ' || *p == '\t' ||
               *p == '\r' || *p == '\n') {
            p++;
        }
        if (!*p) break;

        if ((p[0] == 'a' || p[0] == 'A') &&
            (p[1] == 'l' || p[1] == 'L') &&
            (p[2] == 'l' || p[2] == 'L') &&
            (p[3] == '\0' || p[3] == ',' || p[3] == ';' ||
             p[3] == ' ' || p[3] == '\t' || p[3] == '\r' || p[3] == '\n')) {
            fntrace_arm(0xFFFFFFFFu);
            p += 3;
            continue;
        }

        char *end = NULL;
        unsigned long value = strtoul(p, &end, 0);
        if (end == p) {
            while (*p && *p != ',' && *p != ';' && *p != ' ' && *p != '\t' &&
                   *p != '\r' && *p != '\n') {
                p++;
            }
            continue;
        }

        fntrace_arm((uint32_t)value);
        p = end;
    }
}

void fntrace_clear(void) {
    g_fntrace_seq = 0;
    /* Storage left in place; consumer dumps relative to seq. */
}
