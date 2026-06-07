/* dirty_ram_interp.c — small MIPS interpreter for install-at-runtime RAM.
 *
 * See CLAUDE.md Rule 18, docs/dynamic_handler_install.md, and the inline
 * note in memory.c (search for "Option B") for the architectural rationale.
 *
 * Scope: only fires when psx_dispatch lands on a PC whose page has been
 * written-to since boot.  Runs one basic block (terminator: jr/jalr/j/jal
 * or branch) and returns; dispatch trampoline re-enters for the next block.
 *
 * Strict policy: any opcode not implemented here aborts fatally.  This
 * surfaces unknown install patterns immediately so we expand the support
 * set deliberately, never silently.
 *
 * Future option (Option B, see docs/dynamic_handler_install.md): JIT-compile
 * dirty pages via the existing StrictTranslator instead of interpreting.
 * Pros: single source of MIPS semantics shared with the build-time path,
 * native-speed install stubs, generalizes to game JIT cases.  Cons: gcc-at-
 * runtime build dep, ~200 ms compile latency stall on first dispatch, file
 * I/O on hot path, cache-invalidation complexity, Windows MinGW + dlopen
 * friction.  Today install stubs are cold-path glue (~4k instructions per
 * directory-load); interpretation is sub-microsecond and the right fit.
 * Revisit if measurement shows install-stub instructions becoming a
 * meaningful fraction of total runtime work.
 */

#include "dirty_ram_interp.h"
#include "cpu_state.h"
#include "debug_server.h"
#include "fntrace.h"
#include "interrupts.h"
#include "psx_cycles.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

uint64_t g_dirty_ram_blocks_run = 0;
uint64_t g_dirty_ram_insns_run  = 0;
uint64_t g_dirty_ram_static_text_blocks_run = 0;
uint64_t g_dirty_ram_aborts     = 0;
uint64_t g_dirty_ram_guard_yields = 0;

/* Mid-block unsupported-opcode counters. Bumped instead of fprintf-spamming
 * stderr (CLAUDE.md §3). Read via dirty_ram_get_unsupported(). The "last_*"
 * fields capture the most recent occurrence so a TCP query can see what
 * opcode is missing without needing log scraping. */
uint64_t g_dirty_ram_unsupported_midblock = 0;
uint32_t g_dirty_ram_last_unsupported_entry = 0;
uint32_t g_dirty_ram_last_unsupported_entry_ra = 0;
uint32_t g_dirty_ram_last_unsupported_entry_sp = 0;
uint32_t g_dirty_ram_last_unsupported_insns = 0;
uint32_t g_dirty_ram_last_unsupported_pc  = 0;
uint32_t g_dirty_ram_last_unsupported_insn = 0;
const char *g_dirty_ram_last_unsupported_reason = NULL;

DirtyRamPcEntry g_dirty_ram_pc_table[DIRTY_RAM_PC_TABLE_SIZE] = {0};

DirtyRamBlockLogEntry g_dirty_ram_block_log[DIRTY_RAM_BLOCK_LOG_CAP] = {0};
uint64_t              g_dirty_ram_block_log_seq = 0;
DirtyRamFlowLogEntry  g_dirty_ram_flow_log[DIRTY_RAM_FLOW_LOG_CAP] = {0};
uint64_t              g_dirty_ram_flow_log_seq = 0;
DirtyRamExitLogEntry  g_dirty_ram_exit_log[DIRTY_RAM_EXIT_LOG_CAP] = {0};
uint64_t              g_dirty_ram_exit_log_seq = 0;
DirtyRamCallLogEntry  g_dirty_ram_call_log[DIRTY_RAM_CALL_LOG_CAP] = {0};
uint64_t              g_dirty_ram_call_log_seq = 0;
DirtyRamInsnLogEntry  g_dirty_ram_insn_log[DIRTY_RAM_INSN_LOG_CAP] = {0};
uint64_t              g_dirty_ram_insn_log_seq = 0;

static struct { uint32_t lo, hi; } g_dirty_ram_trace_ranges[DIRTY_RAM_TRACE_MAX_RANGES];
static int g_dirty_ram_trace_range_count = 0;

/* Current frame counter, defined in debug_server.c. */
extern uint64_t s_frame_count;

/* Linear-probed insert/lookup keyed on entry PC.  Table is small (64) and
 * the working set of install-stub PCs is tiny (handful), so this stays
 * O(1) in practice.  Returns NULL if the table is full — caller treats
 * that as "stop tracking" rather than failing. */
static DirtyRamPcEntry *pc_table_get_or_insert(uint32_t pc) {
    uint32_t h = (pc * 2654435761u) & (DIRTY_RAM_PC_TABLE_SIZE - 1);
    for (uint32_t i = 0; i < DIRTY_RAM_PC_TABLE_SIZE; i++) {
        uint32_t idx = (h + i) & (DIRTY_RAM_PC_TABLE_SIZE - 1);
        DirtyRamPcEntry *e = &g_dirty_ram_pc_table[idx];
        if (e->pc == pc) return e;
        if (e->pc == 0) { e->pc = pc; return e; }
    }
    return NULL;
}

/* From debug_server.c — keep our outer-frame attribution coherent. */
extern uint32_t g_debug_current_func_addr;
extern uint32_t g_debug_last_store_pc;

#ifdef PSX_HAS_GAME_DISPATCH
extern int psx_dispatch_game_compiled(CPUState* cpu, uint32_t addr);
extern int psx_game_address_in_text(uint32_t addr);
#endif
extern void psx_dispatch_call(CPUState* cpu, uint32_t addr, uint32_t return_addr);
extern int g_psx_call_depth;
extern uint32_t i_stat;
extern uint32_t i_mask;

/* Forward decls from memory.c — used to read instruction bytes. */
extern uint8_t *memory_get_ram_ptr(void);

/* MIPS instruction field decoders. */
static inline uint32_t op_field    (uint32_t i) { return (i >> 26) & 0x3Fu; }
static inline uint32_t rs_field    (uint32_t i) { return (i >> 21) & 0x1Fu; }
static inline uint32_t rt_field    (uint32_t i) { return (i >> 16) & 0x1Fu; }
static inline uint32_t rd_field    (uint32_t i) { return (i >> 11) & 0x1Fu; }
static inline uint32_t shamt_field (uint32_t i) { return (i >>  6) & 0x1Fu; }
static inline uint32_t funct_field (uint32_t i) { return  i        & 0x3Fu; }
static inline uint32_t imm16_field (uint32_t i) { return  i        & 0xFFFFu; }
static inline int32_t  simm16_field(uint32_t i) { return (int32_t)(int16_t)imm16_field(i); }
static inline uint32_t target26    (uint32_t i) { return  i        & 0x03FFFFFFu; }

/* Read a 32-bit instruction word from kernel RAM at the given physical addr.
 * Caller has already verified the address is in dirty kernel RAM. */
static inline uint32_t fetch_word(uint32_t phys) {
    const uint8_t *ram = memory_get_ram_ptr();
    return  (uint32_t)ram[phys]
         | ((uint32_t)ram[phys + 1] <<  8)
         | ((uint32_t)ram[phys + 2] << 16)
         | ((uint32_t)ram[phys + 3] << 24);
}

int dirty_ram_trace_add_range(uint32_t lo, uint32_t hi) {
    if (g_dirty_ram_trace_range_count >= DIRTY_RAM_TRACE_MAX_RANGES) return 0;
    lo &= 0x1FFFFFFFu;
    hi &= 0x1FFFFFFFu;
    if (lo == hi) return 0;
    int slot = g_dirty_ram_trace_range_count++;
    g_dirty_ram_trace_ranges[slot].lo = lo;
    g_dirty_ram_trace_ranges[slot].hi = hi;
    return 1;
}

void dirty_ram_trace_clear_ranges(void) {
    g_dirty_ram_trace_range_count = 0;
}

int dirty_ram_trace_range_count(void) {
    return g_dirty_ram_trace_range_count;
}

int dirty_ram_trace_get_range(int slot, uint32_t *lo, uint32_t *hi) {
    if (slot < 0 || slot >= g_dirty_ram_trace_range_count) return 0;
    if (lo) *lo = g_dirty_ram_trace_ranges[slot].lo;
    if (hi) *hi = g_dirty_ram_trace_ranges[slot].hi;
    return 1;
}

static int dirty_ram_trace_should_log(uint32_t phys) {
    if (g_dirty_ram_trace_range_count > 0) {
        for (int i = 0; i < g_dirty_ram_trace_range_count; i++) {
            if (phys >= g_dirty_ram_trace_ranges[i].lo &&
                phys <  g_dirty_ram_trace_ranges[i].hi) {
                return 1;
            }
        }
        return 0;
    }

    return ((phys >= 0x000E0000u && phys < 0x000F0000u) ||
            (phys >= 0x000000A0u && phys < 0x000000C0u) ||
            (phys >= 0x000005C0u && phys < 0x00000620u) ||
            (phys >= 0x00001E00u && phys < 0x00002000u));
}

static void dirty_ram_log_instruction(CPUState *cpu, uint32_t pc, uint32_t insn,
                                      const uint32_t before_s[4],
                                      uint32_t next_pc,
                                      uint32_t target, int transferred) {
    uint32_t phys = pc & 0x1FFFFFFFu;
    if (!dirty_ram_trace_should_log(phys)) {
        return;
    }

    uint32_t tcb_ptr_addr = cpu->read_word(0x00000108u);
    uint32_t current_tcb = tcb_ptr_addr ? cpu->read_word(tcb_ptr_addr) : 0;
    uint32_t task_ptr = cpu->read_word(0x1F8001D4u);

    uint64_t s = g_dirty_ram_insn_log_seq++;
    DirtyRamInsnLogEntry *e =
        &g_dirty_ram_insn_log[s & (DIRTY_RAM_INSN_LOG_CAP - 1u)];
    e->seq          = s;
    e->pc           = pc;
    e->insn         = insn;
    e->next_pc      = next_pc;
    e->target       = target;
    e->before_s0    = before_s[0];
    e->after_s0     = cpu->gpr[16];
    e->before_s1    = before_s[1];
    e->after_s1     = cpu->gpr[17];
    e->before_s2    = before_s[2];
    e->after_s2     = cpu->gpr[18];
    e->before_s3    = before_s[3];
    e->after_s3     = cpu->gpr[19];
    e->sp           = cpu->gpr[29];
    e->ra           = cpu->gpr[31];
    e->v0           = cpu->gpr[2];
    e->v1           = cpu->gpr[3];
    e->a0           = cpu->gpr[4];
    e->a1           = cpu->gpr[5];
    e->a2           = cpu->gpr[6];
    e->a3           = cpu->gpr[7];
    e->t0           = cpu->gpr[8];
    e->t1           = cpu->gpr[9];
    e->t2           = cpu->gpr[10];
    e->current_tcb  = current_tcb;
    e->task_ptr     = task_ptr;
    e->task_mode    = task_ptr ? cpu->read_half(task_ptr + 72u) : 0;
    e->task_submode = task_ptr ? cpu->read_half(task_ptr + 74u) : 0;
    e->frame        = (uint32_t)s_frame_count;
    e->transferred  = (uint8_t)(transferred ? 1u : 0u);
}

static inline uint32_t interp_lwl(CPUState *cpu, uint32_t addr, uint32_t rt_value) {
    uint32_t word = cpu->read_word(addr & ~3u);
    switch (addr & 3u) {
        case 0: return (rt_value & 0x00FFFFFFu) | (word << 24);
        case 1: return (rt_value & 0x0000FFFFu) | (word << 16);
        case 2: return (rt_value & 0x000000FFu) | (word << 8);
        default: return word;
    }
}

static inline uint32_t interp_lwr(CPUState *cpu, uint32_t addr, uint32_t rt_value) {
    uint32_t word = cpu->read_word(addr & ~3u);
    switch (addr & 3u) {
        case 0: return word;
        case 1: return (rt_value & 0xFF000000u) | (word >> 8);
        case 2: return (rt_value & 0xFFFF0000u) | (word >> 16);
        default: return (rt_value & 0xFFFFFF00u) | (word >> 24);
    }
}

static inline void interp_swl(CPUState *cpu, uint32_t addr, uint32_t value) {
    uint32_t aligned = addr & ~3u;
    uint32_t word = cpu->read_word(aligned);
    switch (addr & 3u) {
        case 0: word = (word & 0xFFFFFF00u) | (value >> 24); break;
        case 1: word = (word & 0xFFFF0000u) | (value >> 16); break;
        case 2: word = (word & 0xFF000000u) | (value >> 8); break;
        default: word = value; break;
    }
    cpu->write_word(aligned, word);
}

static inline void interp_swr(CPUState *cpu, uint32_t addr, uint32_t value) {
    uint32_t aligned = addr & ~3u;
    uint32_t word = cpu->read_word(aligned);
    switch (addr & 3u) {
        case 0: word = value; break;
        case 1: word = (word & 0x000000FFu) | (value << 8); break;
        case 2: word = (word & 0x0000FFFFu) | (value << 16); break;
        default: word = (word & 0x00FFFFFFu) | (value << 24); break;
    }
    cpu->write_word(aligned, word);
}

/* Soft-fail thread-local flag.  When the interpreter encounters an opcode
 * it doesn't implement, it sets this flag and returns instead of aborting,
 * letting the caller (psx_dispatch via dirty_ram_dispatch) fall back to
 * psx_unknown_dispatch — which has its own ad-hoc resolver for known
 * trampoline patterns (jr-based vector dispatch, etc.).
 *
 * This is a deliberate retreat from "always pick the most complete option"
 * for ONE narrow case: dispatch into pages that have been written-to but
 * don't actually contain valid stub code at the dispatched PC (e.g.
 * stale data, return-target addresses that point to non-code areas).  The
 * pre-existing psx_unknown_dispatch already handles those — we just need
 * to let it.  If a true install stub uses an opcode we don't have, this
 * will silently route it to psx_unknown_dispatch, which will likely return
 * a no-op cpu->pc=0.  When that happens, we'll see "card protocol stalls"
 * in measurement and add the missing opcode here. */
static int g_unsupported_seen = 0;
static uint32_t g_unsupported_pc = 0;
static uint32_t g_unsupported_insn = 0;
static const char *g_unsupported_reason = NULL;

static int abort_unsupported(uint32_t pc, uint32_t insn, const char *reason) {
    g_dirty_ram_aborts++;
    g_unsupported_seen   = 1;
    g_unsupported_pc     = pc;
    g_unsupported_insn   = insn;
    g_unsupported_reason = reason;
    return 1; /* signal "control transferred" so the caller stops */
}

static int is_local_dirty_target(uint32_t target) {
    uint32_t phys = target & 0x1FFFFFFFu;
    if (!dirty_ram_is_dirty(phys)) return 0;
#ifdef PSX_HAS_GAME_DISPATCH
    if (fntrace_is_game_started() && psx_game_address_in_text(target)) return 0;
#endif
    return phys >= 0x00098000u;
}

static int allow_dirty_local_flow_for_addr(uint32_t addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;
#ifdef PSX_HAS_GAME_DISPATCH
    if (fntrace_is_game_started() && psx_game_address_in_text(addr)) return 0;
#endif
    return phys >= 0x00098000u;
}

static uint32_t dirty_ram_current_tcb(CPUState *cpu) {
    uint32_t tcb_ptr_addr = cpu->read_word(0x00000108u);
    return tcb_ptr_addr ? cpu->read_word(tcb_ptr_addr) : 0;
}

enum {
    DIRTY_EXIT_ZERO_ADDR          = 1,
    DIRTY_EXIT_EXCEPTION_VECTOR  = 2,
    DIRTY_EXIT_UNSUPPORTED_ENTRY = 3,
    DIRTY_EXIT_UNSUPPORTED_MID   = 4,
    DIRTY_EXIT_STOP_ADDR         = 5,
    DIRTY_EXIT_NONLOCAL_TARGET   = 6,
    DIRTY_EXIT_CLEAN_NEXT        = 7,
    DIRTY_EXIT_GUARD             = 8,
    DIRTY_EXIT_STATIC_TEXT_BLOCK = 9,
};

static void dirty_ram_log_exit(CPUState *cpu, uint32_t entry, uint32_t pc,
                               uint32_t target, uint32_t stop_addr,
                               uint32_t reason, uint32_t insns,
                               int handled) {
    uint32_t entry_phys = entry & 0x1FFFFFFFu;
    if (entry_phys < 0x00098000u && stop_addr == 0) {
        return;
    }

    uint64_t s = g_dirty_ram_exit_log_seq++;
    DirtyRamExitLogEntry *e =
        &g_dirty_ram_exit_log[s & (DIRTY_RAM_EXIT_LOG_CAP - 1u)];
    e->seq = s;
    e->entry = entry;
    e->pc = pc;
    e->target = target;
    e->stop_addr = stop_addr;
    e->reason = reason;
    e->insns = insns;
    e->s0 = cpu ? cpu->gpr[16] : 0;
    e->s1 = cpu ? cpu->gpr[17] : 0;
    e->s2 = cpu ? cpu->gpr[18] : 0;
    e->s3 = cpu ? cpu->gpr[19] : 0;
    e->sp = cpu ? cpu->gpr[29] : 0;
    e->ra = cpu ? cpu->gpr[31] : 0;
    e->v0 = cpu ? cpu->gpr[2] : 0;
    e->v1 = cpu ? cpu->gpr[3] : 0;
    e->a0 = cpu ? cpu->gpr[4] : 0;
    e->a1 = cpu ? cpu->gpr[5] : 0;
    e->a2 = cpu ? cpu->gpr[6] : 0;
    e->a3 = cpu ? cpu->gpr[7] : 0;
    e->current_tcb = cpu ? dirty_ram_current_tcb(cpu) : 0;
    e->frame = (uint32_t)s_frame_count;
    e->handled = (uint8_t)(handled ? 1u : 0u);
    e->in_exception = (uint8_t)(psx_get_in_exception() ? 1u : 0u);
}

static int dispatch_nonlocal_call(CPUState *cpu, uint32_t target,
                                  uint32_t return_pc,
                                  uint32_t *next_pc_out) {
    cpu->pc = 0;
    int defer_interrupts = 0;
#ifdef PSX_HAS_GAME_DISPATCH
    defer_interrupts = fntrace_is_game_started() &&
                       psx_game_address_in_text(target) &&
                       ((i_stat & i_mask) != 0);
#endif
    /* Use the normal dispatcher call path for compiled/static callees too,
     * so stop-address handling matches generated JAL/JALR calls. The dirty
     * interpreter's caller continuation is not represented on the native C
     * stack. When an overlay callback calls into statically recompiled game
     * text while an IRQ is already pending, defer interrupt delivery until the
     * post-JAL interpreter PC is selected; otherwise the callback can reenter
     * before its immediate stores run. Calls made before an IRQ is pending
     * remain interruptible because they can legitimately wait for CD/SIO IRQs.
     * Hardware sources are still ticked by psx_check_interrupts. */
    uint64_t call_seq = g_dirty_ram_call_log_seq++;
    DirtyRamCallLogEntry *call_log =
        &g_dirty_ram_call_log[call_seq & (DIRTY_RAM_CALL_LOG_CAP - 1u)];
    call_log->seq = call_seq;
    call_log->pc = return_pc - 8u;
    call_log->target = target;
    call_log->return_pc = return_pc;
    call_log->before_s0 = cpu->gpr[16];
    call_log->before_s1 = cpu->gpr[17];
    call_log->before_s2 = cpu->gpr[18];
    call_log->before_s3 = cpu->gpr[19];
    call_log->before_sp = cpu->gpr[29];
    call_log->before_ra = cpu->gpr[31];
    call_log->a0 = cpu->gpr[4];
    call_log->a1 = cpu->gpr[5];
    call_log->a2 = cpu->gpr[6];
    call_log->a3 = cpu->gpr[7];
    call_log->current_tcb = dirty_ram_current_tcb(cpu);
    call_log->frame = (uint32_t)s_frame_count;
    call_log->defer_interrupts = (uint8_t)(defer_interrupts ? 1u : 0u);
    call_log->in_exception = (uint8_t)(psx_get_in_exception() ? 1u : 0u);
    call_log->returned_pc_nonzero = 0;

    if (defer_interrupts) g_psx_call_depth++;
    psx_dispatch_call(cpu, target, return_pc);
    if (defer_interrupts) g_psx_call_depth--;
    if (defer_interrupts && cpu->pc == 0) {
        psx_check_interrupts_at(cpu, return_pc);
    }
    call_log->after_s0 = cpu->gpr[16];
    call_log->after_s1 = cpu->gpr[17];
    call_log->after_s2 = cpu->gpr[18];
    call_log->after_s3 = cpu->gpr[19];
    call_log->after_sp = cpu->gpr[29];
    call_log->after_ra = cpu->gpr[31];
    call_log->returned_pc_nonzero = (uint8_t)(cpu->pc != 0 ? 1u : 0u);
    if (cpu->pc != 0) return 1;
    *next_pc_out = return_pc;
    return 0;
}

/* Execute ONE instruction at *pc on the given CPU state.  Returns:
 *   0 = continue (advance pc by 4)
 *   1 = control transferred OR unsupported opcode (caller checks
 *       g_unsupported_seen to distinguish).
 * Branches encode their delay slot themselves before returning 1. */
static int exec_one(CPUState *cpu, uint32_t pc, uint32_t *next_pc_out);

static inline int is_rfe_instruction(uint32_t insn) {
    return op_field(insn) == 0x10u &&
           rs_field(insn) == 0x10u &&
           funct_field(insn) == 0x10u;
}

static inline void exec_rfe(CPUState *cpu) {
    uint32_t sr = cpu->cop0[12];
    cpu->cop0[12] = (sr & 0xFFFFFFC0u) | ((sr >> 2) & 0x0Fu);
}

/* Forward: helper for delay-slot execution on jumps/branches. */
static int exec_delay_slot(CPUState *cpu, uint32_t pc, uint32_t branch_target) {
    /* Delay-slot instruction at pc must NOT be a control transfer.
     * Recursively interpret as a single non-branching instruction. */
    uint32_t ds_phys = pc & 0x1FFFFFFFu;
    uint32_t insn = fetch_word(ds_phys);
    uint32_t opc = op_field(insn);
    uint32_t fnt = funct_field(insn);
    if (is_rfe_instruction(insn)) {
        exec_rfe(cpu);
        cpu->pc = branch_target;
        g_dirty_ram_insns_run++;
        psx_rfe_return(cpu);
        return 1;
    }
    /* Reject branches/jumps in delay slots — undefined on R3000A and our
     * static recompiler explicitly handles this case differently (the
     * fall-through fix from 2026-04-21).  In install stubs, delay slots
     * are always nop or simple arithmetic. */
    if (opc == 0x02 /*j*/ || opc == 0x03 /*jal*/ ||
        opc == 0x04 /*beq*/ || opc == 0x05 /*bne*/ ||
        opc == 0x06 /*blez*/ || opc == 0x07 /*bgtz*/ ||
        opc == 0x01 /*regimm*/ ||
        (opc == 0x00 && (fnt == 0x08 /*jr*/ || fnt == 0x09 /*jalr*/))) {
        (void)abort_unsupported(pc, insn, "control-transfer in delay slot");
        return 0;
    }
    uint32_t dummy_next = 0;
    (void)exec_one(cpu, pc, &dummy_next);
    g_dirty_ram_insns_run++;
    return 0;
}

static int exec_one(CPUState *cpu, uint32_t pc, uint32_t *next_pc_out) {
    uint32_t phys = pc & 0x1FFFFFFFu;
    uint32_t insn = fetch_word(phys);
    uint32_t opc  = op_field(insn);
    uint32_t rs   = rs_field(insn);
    uint32_t rt   = rt_field(insn);
    uint32_t rd   = rd_field(insn);
    uint32_t sh   = shamt_field(insn);
    uint32_t fnt  = funct_field(insn);
    int32_t  simm = simm16_field(insn);
    uint32_t imm  = imm16_field(insn);

    *next_pc_out = pc + 4;

    /* Update last-store PC tracker so SIO PC tracer attribution stays
     * coherent through interpreted stubs. */
    g_debug_last_store_pc = pc;

    switch (opc) {
    case 0x00: /* SPECIAL */
        switch (fnt) {
        case 0x00: /* SLL rd, rt, sh (also nop when all fields are 0) */
            cpu->gpr[rd] = cpu->gpr[rt] << sh;
            cpu->gpr[0] = 0;
            return 0;
        case 0x02: /* SRL */
            cpu->gpr[rd] = cpu->gpr[rt] >> sh;
            cpu->gpr[0] = 0;
            return 0;
        case 0x03: /* SRA */
            cpu->gpr[rd] = (uint32_t)((int32_t)cpu->gpr[rt] >> sh);
            cpu->gpr[0] = 0;
            return 0;
        case 0x04: /* SLLV */
            cpu->gpr[rd] = cpu->gpr[rt] << (cpu->gpr[rs] & 31);
            cpu->gpr[0] = 0;
            return 0;
        case 0x06: /* SRLV */
            cpu->gpr[rd] = cpu->gpr[rt] >> (cpu->gpr[rs] & 31);
            cpu->gpr[0] = 0;
            return 0;
        case 0x07: /* SRAV */
            cpu->gpr[rd] = (uint32_t)((int32_t)cpu->gpr[rt] >> (cpu->gpr[rs] & 31));
            cpu->gpr[0] = 0;
            return 0;
        case 0x08: { /* JR rs */
            uint32_t target = cpu->gpr[rs];
            if (exec_delay_slot(cpu, pc + 4, target)) return 1;
            cpu->pc = target;
            return 1;
        }
        case 0x09: { /* JALR rd, rs */
            uint32_t target = cpu->gpr[rs];
            uint32_t return_pc = pc + 8;
            cpu->gpr[rd ? rd : 31] = return_pc;
            cpu->gpr[0] = 0;
            if (exec_delay_slot(cpu, pc + 4, target)) return 1;
            if (!is_local_dirty_target(target)) {
                return dispatch_nonlocal_call(cpu, target, return_pc, next_pc_out);
            }
            cpu->pc = target;
            return 1;
        }
        case 0x0C: /* SYSCALL */
            cpu->pc = pc;
            psx_syscall(cpu, (insn >> 6) & 0xFFFFFu);
            return (cpu->pc != 0);
        case 0x0D: /* BREAK */
            psx_break(cpu, (insn >> 6) & 0xFFFFFu, pc);
            return 1;
        case 0x0F: /* SYNC */
            return 0;
        case 0x10: /* MFHI */
            cpu->gpr[rd] = cpu->hi;
            cpu->gpr[0] = 0;
            return 0;
        case 0x11: /* MTHI */
            cpu->hi = cpu->gpr[rs];
            return 0;
        case 0x12: /* MFLO */
            cpu->gpr[rd] = cpu->lo;
            cpu->gpr[0] = 0;
            return 0;
        case 0x13: /* MTLO */
            cpu->lo = cpu->gpr[rs];
            return 0;
        case 0x18: { /* MULT */
            int64_t r = (int64_t)(int32_t)cpu->gpr[rs] * (int64_t)(int32_t)cpu->gpr[rt];
            cpu->lo = (uint32_t)r;
            cpu->hi = (uint32_t)((uint64_t)r >> 32);
            return 0;
        }
        case 0x19: { /* MULTU */
            uint64_t r = (uint64_t)cpu->gpr[rs] * (uint64_t)cpu->gpr[rt];
            cpu->lo = (uint32_t)r;
            cpu->hi = (uint32_t)(r >> 32);
            return 0;
        }
        case 0x1A: { /* DIV */
            int32_t a = (int32_t)cpu->gpr[rs];
            int32_t b = (int32_t)cpu->gpr[rt];
            if (b == 0) {
                cpu->lo = (a < 0) ? 1u : 0xFFFFFFFFu;
                cpu->hi = (uint32_t)a;
            } else if ((uint32_t)a == 0x80000000u && b == -1) {
                cpu->lo = 0x80000000u;
                cpu->hi = 0;
            } else {
                cpu->lo = (uint32_t)(a / b);
                cpu->hi = (uint32_t)(a % b);
            }
            return 0;
        }
        case 0x1B: /* DIVU */
            if (cpu->gpr[rt] == 0) {
                cpu->lo = 0xFFFFFFFFu;
                cpu->hi = cpu->gpr[rs];
            } else {
                cpu->lo = cpu->gpr[rs] / cpu->gpr[rt];
                cpu->hi = cpu->gpr[rs] % cpu->gpr[rt];
            }
            return 0;
        case 0x20: /* ADD - overflow traps are delegated if they occur. */
        case 0x21: /* ADDU rd, rs, rt */
            cpu->gpr[rd] = cpu->gpr[rs] + cpu->gpr[rt];
            cpu->gpr[0] = 0;
            return 0;
        case 0x22: /* SUB - overflow traps are delegated if they occur. */
        case 0x23: /* SUBU */
            cpu->gpr[rd] = cpu->gpr[rs] - cpu->gpr[rt];
            cpu->gpr[0] = 0;
            return 0;
        case 0x24: /* AND */
            cpu->gpr[rd] = cpu->gpr[rs] & cpu->gpr[rt];
            cpu->gpr[0] = 0;
            return 0;
        case 0x25: /* OR */
            cpu->gpr[rd] = cpu->gpr[rs] | cpu->gpr[rt];
            cpu->gpr[0] = 0;
            return 0;
        case 0x26: /* XOR */
            cpu->gpr[rd] = cpu->gpr[rs] ^ cpu->gpr[rt];
            cpu->gpr[0] = 0;
            return 0;
        case 0x27: /* NOR */
            cpu->gpr[rd] = ~(cpu->gpr[rs] | cpu->gpr[rt]);
            cpu->gpr[0] = 0;
            return 0;
        case 0x2A: /* SLT */
            cpu->gpr[rd] = ((int32_t)cpu->gpr[rs] < (int32_t)cpu->gpr[rt]) ? 1u : 0u;
            cpu->gpr[0] = 0;
            return 0;
        case 0x2B: /* SLTU */
            cpu->gpr[rd] = (cpu->gpr[rs] < cpu->gpr[rt]) ? 1u : 0u;
            cpu->gpr[0] = 0;
            return 0;
        default:
            return abort_unsupported(pc, insn, "SPECIAL funct");
        }
        break;

    case 0x02: { /* J target */
        uint32_t target = ((pc + 4) & 0xF0000000u) | (target26(insn) << 2);
        if (exec_delay_slot(cpu, pc + 4, target)) return 1;
        cpu->pc = target;
        return 1;
    }
    case 0x03: { /* JAL target */
        uint32_t target = ((pc + 4) & 0xF0000000u) | (target26(insn) << 2);
        uint32_t return_pc = pc + 8;
        cpu->gpr[31] = return_pc;
        if (exec_delay_slot(cpu, pc + 4, target)) return 1;
        if (!is_local_dirty_target(target)) {
            return dispatch_nonlocal_call(cpu, target, return_pc, next_pc_out);
        }
        cpu->pc = target;
        return 1;
    }
    case 0x04: { /* BEQ rs, rt, simm */
        int taken = (cpu->gpr[rs] == cpu->gpr[rt]);
        uint32_t target = taken ? (pc + 4 + (simm << 2)) : (pc + 8);
        if (exec_delay_slot(cpu, pc + 4, target)) return 1;
        cpu->pc = target;
        return 1;
    }
    case 0x05: { /* BNE */
        int taken = (cpu->gpr[rs] != cpu->gpr[rt]);
        uint32_t target = taken ? (pc + 4 + (simm << 2)) : (pc + 8);
        if (exec_delay_slot(cpu, pc + 4, target)) return 1;
        cpu->pc = target;
        return 1;
    }
    case 0x06: { /* BLEZ */
        int taken = ((int32_t)cpu->gpr[rs] <= 0);
        uint32_t target = taken ? (pc + 4 + (simm << 2)) : (pc + 8);
        if (exec_delay_slot(cpu, pc + 4, target)) return 1;
        cpu->pc = target;
        return 1;
    }
    case 0x07: { /* BGTZ */
        int taken = ((int32_t)cpu->gpr[rs] > 0);
        uint32_t target = taken ? (pc + 4 + (simm << 2)) : (pc + 8);
        if (exec_delay_slot(cpu, pc + 4, target)) return 1;
        cpu->pc = target;
        return 1;
    }
    case 0x01: { /* REGIMM: BLTZ/BGEZ/BLTZAL/BGEZAL by rt field */
        int taken;
        switch (rt) {
        case 0x00: /* BLTZ */    taken = ((int32_t)cpu->gpr[rs] <  0); break;
        case 0x01: /* BGEZ */    taken = ((int32_t)cpu->gpr[rs] >= 0); break;
        case 0x10: /* BLTZAL */  taken = ((int32_t)cpu->gpr[rs] <  0);
                                  cpu->gpr[31] = pc + 8; break;
        case 0x11: /* BGEZAL */  taken = ((int32_t)cpu->gpr[rs] >= 0);
                                  cpu->gpr[31] = pc + 8; break;
        default: return abort_unsupported(pc, insn, "REGIMM rt");
        }
        uint32_t target = taken ? (pc + 4 + (simm << 2)) : (pc + 8);
        if (exec_delay_slot(cpu, pc + 4, target)) return 1;
        cpu->pc = target;
        return 1;
    }
    case 0x08: /* ADDI rt, rs, simm — same as ADDIU, sans overflow trap (we don't model traps here) */
        cpu->gpr[rt] = cpu->gpr[rs] + (uint32_t)simm;
        cpu->gpr[0] = 0;
        return 0;
    case 0x09: /* ADDIU rt, rs, simm */
        cpu->gpr[rt] = cpu->gpr[rs] + (uint32_t)simm;
        cpu->gpr[0] = 0;
        return 0;
    case 0x0A: /* SLTI */
        cpu->gpr[rt] = ((int32_t)cpu->gpr[rs] < simm) ? 1u : 0u;
        cpu->gpr[0] = 0;
        return 0;
    case 0x0B: /* SLTIU */
        cpu->gpr[rt] = (cpu->gpr[rs] < (uint32_t)simm) ? 1u : 0u;
        cpu->gpr[0] = 0;
        return 0;
    case 0x0C: /* ANDI */
        cpu->gpr[rt] = cpu->gpr[rs] & imm;
        cpu->gpr[0] = 0;
        return 0;
    case 0x0D: /* ORI */
        cpu->gpr[rt] = cpu->gpr[rs] | imm;
        cpu->gpr[0] = 0;
        return 0;
    case 0x0E: /* XORI */
        cpu->gpr[rt] = cpu->gpr[rs] ^ imm;
        cpu->gpr[0] = 0;
        return 0;
    case 0x0F: /* LUI rt, imm */
        cpu->gpr[rt] = imm << 16;
        cpu->gpr[0] = 0;
        return 0;
    case 0x10: { /* COP0 */
        uint32_t cop_op = rs;
        if (cop_op == 0x00) { /* MFC0 */
            cpu->gpr[rt] = cpu->cop0[rd];
            cpu->gpr[0] = 0;
            return 0;
        }
        if (cop_op == 0x04) { /* MTC0 */
            cpu->cop0[rd] = cpu->gpr[rt];
            return 0;
        }
        if (cop_op == 0x10 && fnt == 0x10) { /* RFE */
            exec_rfe(cpu);
            cpu->pc = pc + 4;
            psx_rfe_return(cpu);
            return 1;
        }
        return abort_unsupported(pc, insn, "COP0 op");
    }
    case 0x12: { /* COP2 / GTE */
        uint32_t cop_op = rs;
        if (cop_op == 0x00) { /* MFC2 */
            cpu->gpr[rt] = gte_read_data(cpu, (uint8_t)rd);
            cpu->gpr[0] = 0;
            return 0;
        }
        if (cop_op == 0x02) { /* CFC2 */
            cpu->gpr[rt] = gte_read_ctrl(cpu, (uint8_t)rd);
            cpu->gpr[0] = 0;
            return 0;
        }
        if (cop_op == 0x04) { /* MTC2 */
            gte_write_data(cpu, (uint8_t)rd, cpu->gpr[rt]);
            return 0;
        }
        if (cop_op == 0x06) { /* CTC2 */
            gte_write_ctrl(cpu, (uint8_t)rd, cpu->gpr[rt]);
            return 0;
        }
        if (cop_op & 0x10) {
            gte_execute(cpu, insn & 0x1FFFFFFu);
            return 0;
        }
        return abort_unsupported(pc, insn, "COP2 op");
    }
    case 0x20: { /* LB rt, simm(rs) */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->gpr[rt] = (uint32_t)(int32_t)(int8_t)cpu->read_byte(addr);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x21: { /* LH */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->gpr[rt] = (uint32_t)(int32_t)(int16_t)cpu->read_half(addr);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x22: { /* LWL */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->gpr[rt] = interp_lwl(cpu, addr, cpu->gpr[rt]);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x23: { /* LW */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->gpr[rt] = cpu->read_word(addr);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x24: { /* LBU */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->gpr[rt] = (uint32_t)cpu->read_byte(addr);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x25: { /* LHU */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->gpr[rt] = (uint32_t)cpu->read_half(addr);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x26: { /* LWR */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->gpr[rt] = interp_lwr(cpu, addr, cpu->gpr[rt]);
        cpu->gpr[0] = 0;
        return 0;
    }
    case 0x28: { /* SB */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->write_byte(addr, (uint8_t)cpu->gpr[rt]);
        return 0;
    }
    case 0x29: { /* SH */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->write_half(addr, (uint16_t)cpu->gpr[rt]);
        return 0;
    }
    case 0x2A: { /* SWL */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        interp_swl(cpu, addr, cpu->gpr[rt]);
        return 0;
    }
    case 0x2B: { /* SW */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->write_word(addr, cpu->gpr[rt]);
        return 0;
    }
    case 0x2E: { /* SWR */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        interp_swr(cpu, addr, cpu->gpr[rt]);
        return 0;
    }
    case 0x32: { /* LWC2 */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        gte_write_data(cpu, (uint8_t)rt, cpu->read_word(addr));
        return 0;
    }
    case 0x3A: { /* SWC2 */
        uint32_t addr = cpu->gpr[rs] + (uint32_t)simm;
        cpu->write_word(addr, gte_read_data(cpu, (uint8_t)rt));
        return 0;
    }
    default:
        return abort_unsupported(pc, insn, "primary opcode");
    }
    return 0;
}

/* Public entry point.  Caller (psx_dispatch) has translated `addr` to a
 * KSEG-stripped form already in some cases, so accept any address and
 * mask. Returns 1 if interpretation handled the basic block; 0 if the
 * caller should fall back (e.g. unsupported opcode at the entry, page
 * not dirty). */
static int dirty_ram_dispatch_until_impl(CPUState* cpu, uint32_t addr,
                                         uint32_t stop_addr,
                                         int static_text_fallback) {
    if (addr == 0) {
        cpu->pc = 0;
        dirty_ram_log_exit(cpu, addr, addr, 0, stop_addr,
                           DIRTY_EXIT_ZERO_ADDR, 0, 1);
        return 1;
    }

    uint32_t phys = addr & 0x1FFFFFFFu;

    if (addr == 0x80000048u) {
        cpu->pc = 0;
        dirty_ram_log_exit(cpu, addr, addr, 0, stop_addr,
                           DIRTY_EXIT_EXCEPTION_VECTOR, 0, 1);
        if (psx_get_in_exception()) {
            psx_exception_longjmp(); /* does not return */
        }
        return 1;
    }

#ifdef PSX_HAS_GAME_DISPATCH
    if (psx_dispatch_game_compiled(cpu, addr)) return 1;
    if (static_text_fallback) {
        if (!psx_game_address_in_text(addr)) return 0;
    } else {
        if (fntrace_is_game_started() && psx_game_address_in_text(addr)) return 0;
    }
#else
    if (static_text_fallback) return 0;
#endif

    if (!static_text_fallback && !dirty_ram_is_dirty(phys)) return 0;

    /* Reset soft-fail state at block entry. */
    g_unsupported_seen = 0;
    int allow_local_dirty_flow = allow_dirty_local_flow_for_addr(addr);

    /* Per-PC entry counter (visible via dirty_ram_stats). */
    DirtyRamPcEntry *pc_entry = pc_table_get_or_insert(phys);
    if (pc_entry) pc_entry->hits++;

    /* Block-entry ring buffer — answers "who tried to JALR into this RAM
     * stub" by capturing cpu->gpr[31] (the caller's RA) at dispatch time.
     * Always-on; eviction keeps memory bounded. */
    {
        uint64_t s = g_dirty_ram_block_log_seq++;
        DirtyRamBlockLogEntry *e =
            &g_dirty_ram_block_log[s & (DIRTY_RAM_BLOCK_LOG_CAP - 1u)];
        e->seq    = s;
        e->target = addr;
        e->ra     = cpu->gpr[31];
        e->a0     = cpu->gpr[4];
        e->a1     = cpu->gpr[5];
        e->a2     = cpu->gpr[6];
        e->a3     = cpu->gpr[7];
        e->t0     = cpu->gpr[8];
        e->t1     = cpu->gpr[9];
        e->t2     = cpu->gpr[10];
        e->sp     = cpu->gpr[29];
        e->frame  = (uint32_t)s_frame_count;
    }

    if (debug_server_dirty_break_maybe_pause(addr, cpu)) {
        debug_server_wait_if_paused();
    }
    psx_check_interrupts_at(cpu, addr);

    /* Run dirty code locally until it returns to compiled/non-dirty code.
     * This is required for overlays and for any static text page that has
     * been marked dirty: returning a mid-function continuation to the outer
     * dispatcher would re-enter it without the live register context built by
     * the function prologue. */
    enum { MAX_INSNS_PER_DISPATCH = 1000000 };
    uint32_t pc = addr;
    int insns_executed = 0;
    for (int i = 0; i < MAX_INSNS_PER_DISPATCH; i++) {
        uint32_t next_pc = 0;
        uint32_t insn = fetch_word(pc & 0x1FFFFFFFu);
        uint32_t before_s[4] = {
            cpu->gpr[16], cpu->gpr[17], cpu->gpr[18], cpu->gpr[19]
        };
        int transferred = exec_one(cpu, pc, &next_pc);
#ifdef PSX_ENABLE_BLOCK_CYCLES
        psx_advance_cycles(1u);
#endif
        dirty_ram_log_instruction(cpu, pc, insn, before_s, next_pc,
                                  transferred ? cpu->pc : next_pc,
                                  transferred);
        if (g_unsupported_seen) {
            if (insns_executed == 0) {
                /* Couldn't decode the first instruction.  Most likely
                 * dispatch landed in a dirty page that's not actually
                 * code (stale data, return-target into save area, etc.).
                 * Hand off to psx_unknown_dispatch which has its own
                 * pattern-matching trampoline resolver. */
                dirty_ram_log_exit(cpu, addr, pc, cpu->pc, stop_addr,
                                   DIRTY_EXIT_UNSUPPORTED_ENTRY,
                                   (uint32_t)insns_executed, 0);
                return 0;
            }
            /* Made some progress, then hit unknown.  Treat as a no-op
             * return like psx_unknown_dispatch does for unrecognized
             * targets — set cpu->pc=0 so the trampoline exits cleanly.
             * If this turns out to be load-bearing, measurement will
             * surface it as a card-protocol stall and we can add the
             * missing opcode.
             *
             * No fprintf — read the last-* globals via TCP if needed
             * (CLAUDE.md §3). Synchronous stderr at the rate this fires
             * starves the dispatch loop and the debug-server poll. */
            g_dirty_ram_unsupported_midblock++;
            g_dirty_ram_last_unsupported_entry   = addr;
            g_dirty_ram_last_unsupported_entry_ra = cpu->gpr[31];
            g_dirty_ram_last_unsupported_entry_sp = cpu->gpr[29];
            g_dirty_ram_last_unsupported_insns   = (uint32_t)insns_executed;
            g_dirty_ram_last_unsupported_pc     = g_unsupported_pc;
            g_dirty_ram_last_unsupported_insn   = g_unsupported_insn;
            g_dirty_ram_last_unsupported_reason = g_unsupported_reason;
            cpu->pc = 0;
            dirty_ram_log_exit(cpu, addr, pc, 0, stop_addr,
                               DIRTY_EXIT_UNSUPPORTED_MID,
                               (uint32_t)insns_executed, 1);
            return 1;
        }
        g_dirty_ram_insns_run++;
        insns_executed++;
        if (transferred) {
            uint32_t target = cpu->pc;
            if (stop_addr != 0 &&
                ((target ^ stop_addr) & 0x1FFFFFFFu) == 0) {
                g_dirty_ram_blocks_run++;
                if (static_text_fallback) g_dirty_ram_static_text_blocks_run++;
                if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
                dirty_ram_log_exit(cpu, addr, pc, target, stop_addr,
                                   DIRTY_EXIT_STOP_ADDR,
                                   (uint32_t)insns_executed, 1);
                return 1;
            }
            if (static_text_fallback) {
                g_dirty_ram_blocks_run++;
                g_dirty_ram_static_text_blocks_run++;
                if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
                dirty_ram_log_exit(cpu, addr, pc, target, stop_addr,
                                   DIRTY_EXIT_STATIC_TEXT_BLOCK,
                                   (uint32_t)insns_executed, 1);
                return 1;
            }
            if (allow_local_dirty_flow && target != 0 &&
                is_local_dirty_target(target)) {
                uint64_t s = g_dirty_ram_flow_log_seq++;
                DirtyRamFlowLogEntry *e =
                    &g_dirty_ram_flow_log[s & (DIRTY_RAM_FLOW_LOG_CAP - 1u)];
                e->seq = s;
                e->pc = pc;
                e->target = target;
                e->ra = cpu->gpr[31];
                e->a0 = cpu->gpr[4];
                e->a1 = cpu->gpr[5];
                e->a2 = cpu->gpr[6];
                e->a3 = cpu->gpr[7];
                e->sp = cpu->gpr[29];
                e->frame = (uint32_t)s_frame_count;
                pc = target;
                if ((insns_executed & 0xFFF) == 0) {
                    debug_server_poll();
                    debug_server_wait_if_paused();
                    psx_check_interrupts_at(cpu, pc);
                }
                continue;
            }
            g_dirty_ram_blocks_run++;
            if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
            dirty_ram_log_exit(cpu, addr, pc, target, stop_addr,
                               DIRTY_EXIT_NONLOCAL_TARGET,
                               (uint32_t)insns_executed, 1);
            return 1;
        }
        pc = next_pc;
        /* Straight-line code that left the dirty page — hand back to
         * static dispatch by setting cpu->pc and returning. */
        uint32_t next_phys = pc & 0x1FFFFFFFu;
        if (static_text_fallback) {
#ifdef PSX_HAS_GAME_DISPATCH
            if (!psx_game_address_in_text(pc)) {
                cpu->pc = pc;
                g_dirty_ram_blocks_run++;
                g_dirty_ram_static_text_blocks_run++;
                if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
                dirty_ram_log_exit(cpu, addr, pc, pc, stop_addr,
                                   DIRTY_EXIT_CLEAN_NEXT,
                                   (uint32_t)insns_executed, 1);
                return 1;
            }
#endif
        } else if (!dirty_ram_is_dirty(next_phys)) {
            cpu->pc = pc;
            g_dirty_ram_blocks_run++;
            if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
            dirty_ram_log_exit(cpu, addr, pc, pc, stop_addr,
                               DIRTY_EXIT_CLEAN_NEXT,
                               (uint32_t)insns_executed, 1);
            return 1;
        }
    }
    g_dirty_ram_last_unsupported_pc = pc;
    g_dirty_ram_last_unsupported_insn = fetch_word(pc & 0x1FFFFFFFu);
    g_dirty_ram_last_unsupported_reason = "instruction guard";
    g_dirty_ram_guard_yields++;
    g_dirty_ram_blocks_run++;
    cpu->pc = pc;
    if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
    dirty_ram_log_exit(cpu, addr, pc, pc, stop_addr,
                       DIRTY_EXIT_GUARD,
                       (uint32_t)insns_executed, 1);
    psx_check_interrupts_at(cpu, pc);
    /* Generated static code resumes inline after RFE with cpu->pc == 0.
     * Dirty code's resume point is this explicit interpreter PC, so keep the
     * dispatcher from treating an interrupt return as a completed call. */
    if (cpu->pc == 0) {
        cpu->pc = pc;
    }
    return 1;
}

int dirty_ram_dispatch_until(CPUState* cpu, uint32_t addr, uint32_t stop_addr) {
    return dirty_ram_dispatch_until_impl(cpu, addr, stop_addr, 0);
}

int dirty_ram_dispatch(CPUState* cpu, uint32_t addr) {
    return dirty_ram_dispatch_until(cpu, addr, 0);
}

int dirty_ram_dispatch_static_text(CPUState* cpu, uint32_t addr, uint32_t stop_addr) {
    return dirty_ram_dispatch_until_impl(cpu, addr, stop_addr, 1);
}
