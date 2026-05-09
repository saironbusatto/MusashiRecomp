/* fntrace.c — runtime side of psx_dispatch call ring. See fntrace.h. */

#include "fntrace.h"
#include <string.h>

FntraceEntry g_fntrace_ring[FNTRACE_RING_CAP];
uint64_t     g_fntrace_seq = 0;

/* Frame counter shared with debug_server.c / dirty_ram_interp.c. */
extern uint64_t s_frame_count;

static uint32_t s_arm_targets[FNTRACE_ARM_MAX];
static uint32_t s_arm_count = 0;
static uint32_t s_arm_record_all = 0;  /* opt-in via fntrace_arm(0xFFFFFFFF) */

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
    if (!armed_match(target)) return;
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

void fntrace_clear(void) {
    g_fntrace_seq = 0;
    /* Storage left in place; consumer dumps relative to seq. */
}
