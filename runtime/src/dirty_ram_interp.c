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
#include "interrupts.h"
#include "psx_cycles.h"
#include "gpu.h"   /* psx_ws_is_backdrop_site / psx_ws_backdrop_x (interp hook) */
#include "ws_backdrop_detect.h"  /* shared backdrop-window detector (auto_backdrop) */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

uint64_t g_dirty_ram_blocks_run = 0;
uint64_t g_dirty_ram_insns_run  = 0;
uint64_t g_dirty_window_dispatches = 0;  /* capture-window interp dispatches */
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
DirtyRamPcEntry g_dirty_ram_exec_pc_table[DIRTY_RAM_PC_TABLE_SIZE] = {0};

DirtyRamBlockLogEntry g_dirty_ram_block_log[DIRTY_RAM_BLOCK_LOG_CAP] = {0};
uint64_t              g_dirty_ram_block_log_seq = 0;
DirtyRamFlowLogEntry  g_dirty_ram_flow_log[DIRTY_RAM_FLOW_LOG_CAP] = {0};
uint64_t              g_dirty_ram_flow_log_seq = 0;
DirtyRamInsnLogEntry  g_dirty_ram_insn_log[DIRTY_RAM_INSN_LOG_CAP] = {0};
uint64_t              g_dirty_ram_insn_log_seq = 0;

/* Current frame counter, defined in debug_server.c. */
extern uint64_t s_frame_count;

/* Linear-probed insert/lookup keyed on entry PC.  Table is small (64) and
 * the working set of install-stub PCs is tiny (handful), so this stays
 * O(1) in practice.  Returns NULL if the table is full — caller treats
 * that as "stop tracking" rather than failing. */
static DirtyRamPcEntry *pc_table_get_or_insert_in(DirtyRamPcEntry *table, uint32_t pc) {
    uint32_t h = (pc * 2654435761u) & (DIRTY_RAM_PC_TABLE_SIZE - 1);
    for (uint32_t i = 0; i < DIRTY_RAM_PC_TABLE_SIZE; i++) {
        uint32_t idx = (h + i) & (DIRTY_RAM_PC_TABLE_SIZE - 1);
        DirtyRamPcEntry *e = &table[idx];
        if (e->pc == pc) return e;
        if (e->pc == 0) { e->pc = pc; return e; }
    }
    return NULL;
}

static DirtyRamPcEntry *pc_table_get_or_insert(uint32_t pc) {
    return pc_table_get_or_insert_in(g_dirty_ram_pc_table, pc);
}

/* Record every PC the interpreter executes (not just block entries) so
 * overlay_capture can report execution-verified seeds for the region. */
static void exec_pc_table_record(uint32_t pc) {
    DirtyRamPcEntry *e = pc_table_get_or_insert_in(g_dirty_ram_exec_pc_table,
                                                   pc & 0x1FFFFFFFu);
    if (e) e->hits++;
}

/* From debug_server.c — keep our outer-frame attribution coherent. */
extern uint32_t g_debug_current_func_addr;
extern uint32_t g_debug_last_store_pc;

/* Execution-mode tag for the event-timeline ring: 1 while a dirty_ram_dispatch
 * call is on the stack. Cleared defensively at the psx_check_interrupts longjmp
 * landing (interrupts.c) so the EPC-sentinel longjmp at dirty_ram_dispatch
 * can't leave it stuck on. NATIVE_OVERLAY (inprogress!=0) takes precedence in
 * the ring's mode resolution, so this only distinguishes INTERP from STATIC. */
int g_dirty_interp_active = 0;
int dirty_ram_interp_is_active(void) { return g_dirty_interp_active; }

/* TCP-armed instruction-window capture (native↔interp divergence drill).
 * g_insn_gate_*: an extra runtime-settable PC range that the per-insn log
 * records (on top of the hardwired kernel ranges). Freeze latch: on the Nth
 * dispatch of candidate g_insn_freeze_addr the insn ring stops recording, so
 * its tail preserves the window immediately BEFORE that dispatch — query the
 * ring afterward at leisure (ring-first; no arm-then-hope). */
uint32_t g_insn_gate_lo = 0;       /* extra always-log phys range [lo,hi)      */
uint32_t g_insn_gate_hi = 0;       /* 0 = extra range disabled                 */
uint32_t g_insn_freeze_addr  = 0;  /* candidate phys entry to watch            */
uint32_t g_insn_freeze_nth   = 0;  /* freeze before its Nth dispatch           */
uint32_t g_insn_freeze_count = 0;
int      g_insn_log_frozen   = 0;

#ifdef PSX_HAS_GAME_DISPATCH
extern int psx_dispatch_game_compiled(CPUState* cpu, uint32_t addr);
#endif
extern void psx_dispatch_call(CPUState* cpu, uint32_t addr, uint32_t return_addr);

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

/* Widescreen render-funnel cull detection for the interpreter ([widescreen.cull]
 * auto_screen_x). A `sltiu …,0x140/0x141` is widened ONLY when its enclosing
 * render function also carries a `sltiu …,0xE0/0xF1` (the GTE per-vertex trivial-
 * reject signature) — a lone 0x140 elsewhere stays vanilla. The interp has no
 * function boundaries, so we scan a +/-512-byte window around the site (clamped
 * to main RAM) via the shared psx_ws_func_has_screen_cull, cached per-PC so a
 * hot render loop pays the scan once. Single-threaded interp => static buffers ok. */
static int ws_cull_site(uint32_t pc) {
    enum { WIN = 128 };                       /* +/- 128 words = +/- 512 bytes */
    static struct { uint32_t pc; int8_t flag; } cache[256];
    uint32_t slot = (pc >> 2) & 255u;
    if (cache[slot].pc == pc) return cache[slot].flag;
    uint32_t phys = pc & 0x1FFFFFFFu;
    uint32_t lo = (phys > (uint32_t)(WIN * 4)) ? phys - (uint32_t)(WIN * 4) : 0u;
    uint32_t hi = phys + (uint32_t)(WIN * 4);
    if (hi > 0x200000u) hi = 0x200000u;       /* 2 MB main RAM */
    static uint32_t words[2 * WIN + 1];
    int n = 0;
    for (uint32_t a = lo; a + 4u <= hi && n < (int)(2 * WIN + 1); a += 4u)
        words[n++] = fetch_word(a);
    int flag = psx_ws_func_has_screen_cull(words, n);
    cache[slot].pc = pc; cache[slot].flag = (int8_t)flag;
    return flag;
}

/* Widescreen far-backdrop column-PRELOAD site classification for the interpreter
 * ([widescreen.cull] auto_backdrop). The scrolling-backdrop column-window
 * generators run interpreted in the dev build, so the recompiler emit can't
 * reach them; the interp re-derives the same START/END rewrite sites via the
 * shared detector over a +/-512-byte window around the PC (physical space, so
 * the detector's absolute-PC math matches the masked PC), cached per-PC so a hot
 * generator pays the scan once. Returns WS_BD_NONE / WS_BD_START_ZERO /
 * WS_BD_END_WIDEN. Single-threaded interp => static buffers are safe. */
static int ws_backdrop_site_kind(uint32_t pc, int *out_cols) {
    enum { WIN = 128 };                          /* +/- 128 words = +/- 512 bytes */
    static struct { uint32_t pc; int8_t kind; int16_t cols; } cache[256];
    uint32_t slot = (pc >> 2) & 255u;
    if (cache[slot].pc == pc) { if (out_cols) *out_cols = cache[slot].cols; return cache[slot].kind; }
    uint32_t phys = pc & 0x1FFFFFFFu;
    uint32_t lo = (phys > (uint32_t)(WIN * 4)) ? phys - (uint32_t)(WIN * 4) : 0u;
    uint32_t hi = phys + (uint32_t)(WIN * 4 + 4);
    if (hi > 0x200000u) hi = 0x200000u;          /* 2 MB main RAM */
    static uint32_t words[2 * WIN + 2];
    int n = 0;
    for (uint32_t a = lo; a + 4u <= hi && n < (int)(2 * WIN + 2); a += 4u)
        words[n++] = fetch_word(a);
    int cols = 0;
    int kind = psx_ws_backdrop_kind_at(words, n, lo, phys, &cols);  /* all physical-space */
    cache[slot].pc = pc; cache[slot].kind = (int8_t)kind; cache[slot].cols = (int16_t)cols;
    if (out_cols) *out_cols = cols;
    return kind;
}

static void dirty_ram_log_instruction(CPUState *cpu, uint32_t pc, uint32_t insn,
                                      uint32_t before_s0, uint32_t next_pc,
                                      uint32_t target, int transferred) {
    if (g_insn_log_frozen) return;
    uint32_t phys = pc & 0x1FFFFFFFu;
    if (!((g_insn_gate_hi != 0u && phys >= g_insn_gate_lo && phys < g_insn_gate_hi) ||
          (phys >= 0x000E0000u && phys < 0x000F0000u) ||
          (phys >= 0x000000A0u && phys < 0x000000C0u) ||
          (phys >= 0x000005C0u && phys < 0x00000620u) ||
          (phys >= 0x00001E00u && phys < 0x00002000u))) {
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
    e->before_s0    = before_s0;
    e->after_s0     = cpu->gpr[16];
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

/* Marker entries delimit NATIVE candidate executions in the insn ring (native
 * code emits no per-insn entries; markers keep the two runs' timelines
 * alignable). transferred: 200 = native entry, 201 = native exit. */
void dirty_ram_log_marker(uint32_t addr, uint32_t tag, int kind) {
    if (g_insn_log_frozen) return;
    uint64_t s = g_dirty_ram_insn_log_seq++;
    DirtyRamInsnLogEntry *e =
        &g_dirty_ram_insn_log[s & (DIRTY_RAM_INSN_LOG_CAP - 1u)];
    memset(e, 0, sizeof *e);
    e->seq         = s;
    e->pc          = addr;
    e->target      = tag;
    e->frame       = (uint32_t)s_frame_count;
    e->transferred = (uint8_t)(200 + kind);
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

/* Local-flow gate: overlay region ONLY, by design. Kernel-window code
 * (phys < DIRTY_RAM_KERNEL_WINDOW_END) keeps the verified per-block dispatch
 * cadence — it runs in exception context, where interrupt-check cadence and
 * EPC handling are delicate (see dirty_ram_interp.h window note). Kernel
 * native coverage comes from overlay_loader candidates, not interp chaining. */
static int is_local_dirty_target(uint32_t target) {
    uint32_t phys = target & 0x1FFFFFFFu;
    return phys >= OVERLAY_REGION_FLOOR && dirty_ram_is_dirty(phys);
}

/* Target the last interp run handed back to the dispatch loop (chained
 * continuation). Consumed by the next dirty dispatch to tell apart
 * external entries (from native code) from the interpreter's own
 * block-to-block chaining. */
static uint32_t g_dirty_interp_chain_target = 0;

/* A dirty-RAM target only deserves interpretation if its first word decodes
 * as a plausible MIPS instruction.  A scatter-loaded overlay leaves data
 * bytes in dirty pages; jumping into those and interpreting them as code is
 * how the original cache build produced black/blue screens.  When the target
 * word doesn't look like code, fall back to the normal dispatch path. */
static int dirty_ram_word_looks_decodable(uint32_t insn) {
    if (insn == 0xFFFFFFFFu || insn == 0xFFFFFFFDu) return 0;

    uint32_t op = op_field(insn);
    uint32_t fn = funct_field(insn);
    uint32_t rt = rt_field(insn);

    if (op == 0x00u) {
        switch (fn) {
        case 0x00u: case 0x02u: case 0x03u: case 0x04u:
        case 0x06u: case 0x07u: case 0x08u: case 0x09u:
        case 0x0Cu: case 0x0Du:
        case 0x10u: case 0x11u: case 0x12u: case 0x13u:
        case 0x18u: case 0x19u: case 0x1Au: case 0x1Bu:
        case 0x20u: case 0x21u: case 0x22u: case 0x23u:
        case 0x24u: case 0x25u: case 0x26u: case 0x27u:
        case 0x2Au: case 0x2Bu:
            return 1;
        default:
            return 0;
        }
    }
    if (op == 0x01u) {
        return rt == 0x00u || rt == 0x01u || rt == 0x10u || rt == 0x11u;
    }

    switch (op) {
    case 0x02u: case 0x03u: case 0x04u: case 0x05u:
    case 0x06u: case 0x07u:
    case 0x08u: case 0x09u: case 0x0Au: case 0x0Bu:
    case 0x0Cu: case 0x0Du: case 0x0Eu: case 0x0Fu:
    case 0x10u: case 0x12u:
    case 0x20u: case 0x21u: case 0x22u: case 0x23u:
    case 0x24u: case 0x25u: case 0x26u:
    case 0x28u: case 0x29u: case 0x2Au: case 0x2Bu:
    case 0x2Eu:
    case 0x30u: case 0x32u: case 0x38u: case 0x3Au:
        return 1;
    default:
        return 0;
    }
}

static int dispatch_nonlocal_call(CPUState *cpu, uint32_t target,
                                  uint32_t return_pc,
                                  uint32_t *next_pc_out) {
    cpu->pc = 0;
    psx_dispatch_call(cpu, target, return_pc);
    /* psx_dispatch_call validated the (return_pc, sp) contract; a bail
     * unwind in progress surfaces with cpu->pc = the guest's true target. */
    if (g_psx_call_bail) return 1;
    if (cpu->pc != 0) return 1;
    *next_pc_out = return_pc;
    return 0;
}

/* ── Mixed interp<->compiled dispatch owner (host-stack recursion fix) ──────
 * The dirty interpreter nests psx_dispatch_game_compiled when an interpreted
 * block transfers into compiled code. A guest tail-dispatch loop that crosses the
 * boundary (an interpreted overlay <-> the main-EXE per-frame loop func_8001A954)
 * grows the HOST stack unboundedly while the GUEST stack stays flat — the long-run
 * freeze (docs/RECURSION_BUG.md). The compiled side keeps tail-call loops flat via
 * the psx_dispatch_impl trampoline + g_psx_call_bail unwind; the interpreter
 * bypassed it by nesting directly.
 *
 * Fix: when an interp->compiled crossing would push the HOST stack past a safe
 * watermark, publish the target as a wild-flow bail instead of nesting. The
 * existing contract (compiled frames honor `if (g_psx_call_bail) return;`) unwinds
 * it to the OUTERMOST psx_dispatch_impl, whose flatten path clears the bail and
 * re-dispatches cpu->pc in its loop — so the mutual tail-recursion iterates with
 * bounded native depth. Runtime-only (reuses the existing bail + flatten; no regen).
 *
 * The trigger is the ACTUAL host-stack usage (TEB StackBase - rsp), NOT a nesting
 * counter. A counter LEAKS across the fiber exception path (psx_exception_longjmp
 * never returns, so interp_enter_compiled's post-call decrement is skipped) and
 * creeps up until it falsely trips on a benign call — observed as the FIRST dialogue
 * freezing. Stack usage can't leak and is the real resource we protect: legitimate
 * code (boot, dialogue, deep-but-bounded recursion) stays far below the watermark;
 * only the runaway approaches it. Toggle off with PSX_MIXED_OWNER=0; watermark via
 * PSX_MIXED_STACK_KB (default 700; fiber ~1MB, native-stack guard ~768KB used). */
static int psx_mixed_owner_enabled(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("PSX_MIXED_OWNER"); v = (e && e[0] == '0') ? 0 : 1; }
    return v;
}

#ifdef _WIN32
#include <intrin.h>   /* __readgsqword — fiber TEB StackBase */
static size_t interp_host_stack_used(void) {
    char probe;
    uintptr_t base = (uintptr_t)__readgsqword(0x08);   /* TEB StackBase (high end) */
    uintptr_t sp   = (uintptr_t)&probe;
    return (base > sp) ? (size_t)(base - sp) : 0;
}
#else
static size_t interp_host_stack_used(void) { return 0; }
#endif

static size_t psx_mixed_stack_watermark(void) {
    static long v = -1;
    if (v < 0) { const char *e = getenv("PSX_MIXED_STACK_KB");
                 v = (e && *e) ? atol(e) : 700; if (v < 64) v = 64; }
    return (size_t)v * 1024u;
}

/* ── Flight recorder: per-frame count of the re-entry edge interp->0x8001A954 ──
 * (RECURSION_BUG.md §15). The long-run freeze is a per-frame update that, at
 * ~frame 50k, re-enters func_8001A954 (the per-frame loop head) unboundedly via
 * an interpreted overlay. Record, per emulated frame, how many times that edge
 * fires + the cycle counter, in a ring the crash report dumps. Answers: is the
 * re-entry ORDINARY bounded per-frame behavior that stops TERMINATING at ~50k
 * (count goes unbounded), or a brand-new edge — and whether cycles advance.
 * Only the INTERP->compiled re-entry passes through here; the normal compiled
 * per-frame entry to func_8001A954 does not, so this isolates the suspect edge. */
extern uint64_t psx_cycle_count;
#define SITE_CAP 256u
typedef struct { uint32_t frame; uint32_t count; uint64_t cycle; } SiteEntry;
typedef struct { SiteEntry ring[SITE_CAP]; uint64_t seq; uint32_t cur;
                 uint32_t last_frame; uint32_t maxf; } SiteRec;
/* Per-frame dispatch-count recorders for the suspect loop sites (RECURSION_BUG.md
 * §15). g_site_interp (interp->0x8001A954) was found ORDINARY (1/frame); the
 * recursion re-dispatches the compiled loop via dirty_ram_dispatch's entry because
 * the CD-DMA dirty bitmap is saturated. So record dirty_ram_dispatch(0x8001A954)
 * = loop head and dirty_ram_dispatch(0x80046264) = loop tail. Bounded normally,
 * spike at the freeze => "the per-frame loop stopped terminating." */
static SiteRec g_site_interp = { .last_frame = 0xFFFFFFFFu };  /* interp -> 0x8001A954 */
static SiteRec g_site_dd954  = { .last_frame = 0xFFFFFFFFu };  /* dirty_ram_dispatch(0x8001A954) */
static SiteRec g_site_dd264  = { .last_frame = 0xFFFFFFFFu };  /* dirty_ram_dispatch(0x80046264) */

static void site_note(SiteRec *s) {
    uint32_t f = (uint32_t)s_frame_count;
    if (f != s->last_frame) {
        if (s->last_frame != 0xFFFFFFFFu) {
            SiteEntry *e = &s->ring[s->seq++ & (SITE_CAP - 1u)];
            e->frame = s->last_frame; e->count = s->cur; e->cycle = psx_cycle_count;
            if (s->cur > s->maxf) s->maxf = s->cur;
        }
        s->cur = 0; s->last_frame = f;
    }
    s->cur++;
}

static int site_json(char *out, int cap, const char *name, const SiteRec *s) {
    int n = snprintf(out, cap, "  \"%s\": {\"max_per_frame\":%u,\"cur_frame\":%u,\"cur_count\":%u,\"history\":[",
                     name, s->maxf, s->last_frame, s->cur);
    uint64_t total = s->seq;
    int avail = (total < SITE_CAP) ? (int)total : (int)SITE_CAP;
    uint64_t start = total - (uint64_t)avail;
    for (int i = 0; i < avail && n < cap - 96; i++) {
        const SiteEntry *e = &s->ring[(start + (uint64_t)i) & (SITE_CAP - 1u)];
        n += snprintf(out + n, cap - n, "%s{\"f\":%u,\"n\":%u,\"cyc\":%llu}",
                      i ? "," : "", e->frame, e->count, (unsigned long long)e->cycle);
    }
    n += snprintf(out + n, cap - n, "]},\n");
    return n;
}

/* Emit all three loop-site recorders as JSON (called by crash_trace). */
int dirty_ram_re954_json(char *out, int cap) {
    int n = 0;
    n += site_json(out + n, cap - n, "reentry_interp_8001A954", &g_site_interp);
    n += site_json(out + n, cap - n, "dispatch_8001A954",       &g_site_dd954);
    n += site_json(out + n, cap - n, "dispatch_80046264",       &g_site_dd264);
    return n;
}

/* Enter compiled code from the interpreter for a guest control transfer. Returns
 * like psx_dispatch_game_compiled (1 = handled, OR a stack-watermark bail was
 * surfaced; 0 = not a compiled target — caller falls through to overlay/dirty/
 * nonlocal paths). On surface: cpu->pc = target and g_psx_call_bail set. */
static int interp_enter_compiled(CPUState *cpu, uint32_t target) {
    if (target == 0x8001A954u) site_note(&g_site_interp);
    if (psx_mixed_owner_enabled()
        && interp_host_stack_used() > psx_mixed_stack_watermark()) {
        cpu->pc = target;
        g_psx_call_bail = 1;
        return 1;
    }
    return psx_dispatch_game_compiled(cpu, target);
}

/* Execute ONE instruction at *pc on the given CPU state.  Returns:
 *   0 = continue (advance pc by 4)
 *   1 = control transferred OR unsupported opcode (caller checks
 *       g_unsupported_seen to distinguish).
 * Branches encode their delay slot themselves before returning 1. */
static int exec_one(CPUState *cpu, uint32_t pc, uint32_t *next_pc_out);

/* Forward: helper for delay-slot execution on jumps/branches. */
static void exec_delay_slot(CPUState *cpu, uint32_t pc) {
    /* Delay-slot instruction at pc must NOT be a control transfer.
     * Recursively interpret as a single non-branching instruction. */
    uint32_t ds_phys = pc & 0x1FFFFFFFu;
    uint32_t insn = fetch_word(ds_phys);
    uint32_t opc = op_field(insn);
    uint32_t fnt = funct_field(insn);
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
        return;
    }
    uint32_t dummy_next = 0;
    (void)exec_one(cpu, pc, &dummy_next);
    g_dirty_ram_insns_run++;
}

static int exec_one(CPUState *cpu, uint32_t pc, uint32_t *next_pc_out) {
    exec_pc_table_record(pc);
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

    /* Widescreen far-backdrop column PRELOAD (auto_backdrop). At a detected
     * window START/END finalize, force the loop bound (START->0 / END->sentinel)
     * so the generator's own clamps preload the WHOLE finite tile row into the
     * revealed 16:9 margin. The site is a move/addiu that writes exactly one GPR
     * and has no other effect, so substituting the value and advancing pc+4 is
     * complete. Gated on the runtime predicate first => 4:3 pays nothing. Each
     * rewrite is recorded to the always-on backdrop ring with the live extent
     * (s7), camera-X (scratchpad 0x176) and DL count for `ws_backdrop_ring`. */
    if (psx_ws_backdrop_preload()) {
        int wcols = 0;
        int bk = ws_backdrop_site_kind(pc, &wcols);
        if (bk != WS_BD_NONE) {
            uint32_t dest = (opc == 0x00u && (fnt == 0x21u || fnt == 0x25u)) ? rd
                          : (opc == 0x09u || opc == 0x08u)                   ? rt
                          : 0xFFFFFFFFu;
            if (dest != 0xFFFFFFFFu) {
                /* orig = the instruction's normal result, so the widen shifts the
                 * real camera-tracked bound (move: gpr[src]; addiu: gpr[rs]+simm). */
                uint32_t orig = (opc == 0x00u)
                    ? ((rt == 0u) ? cpu->gpr[rs] : cpu->gpr[rt])
                    : (cpu->gpr[rs] + (uint32_t)simm);
                /* Tell psx_ws_backdrop_value the interp will record the rich ring
                 * entry (it skips its own note when this flag is set). */
                g_ws_bd_from_interp = 1;
                uint32_t finalv = psx_ws_backdrop_value(orig, bk == WS_BD_END, wcols);
                cpu->gpr[dest] = finalv;
                cpu->gpr[0] = 0;
                /* Rich snapshot for the ring: s7 (gpr[23]) = byte tile-row extent;
                 * s0 (gpr[16]) = DL object, count byte at +3; camera-X = scratchpad
                 * halfword 0x176. */
                {
                    int      extent = (int)(int16_t)cpu->gpr[23];
                    int      camx   = (int)(int16_t)(cpu->read_word(0x1F800174u) >> 16);
                    uint32_t s0a    = cpu->gpr[16] + 3u;
                    int      count  = (int)((cpu->read_word(s0a & ~3u) >> ((s0a & 3u) * 8u)) & 0xFFu);
                    /* base = s4 (gpr[20]) = backdrop data ptr (extent@+0, table@+4);
                     * dl = s0 (gpr[16]) = the ordering-table object. */
                    uint32_t a1 = cpu->gpr[20];
                    psx_ws_backdrop_ring_note(pc, bk, wcols, orig, finalv, extent, camx,
                                              count, a1, cpu->gpr[16]);
                    /* Publish the backdrop structure's address range so the GL
                     * 2D-stretch gate can match tile prims by gp0_cmd_source_addr.
                     * Tiles live at a1 + table[col]; bound by the LAST table entry
                     * (table is ascending) + packet slack. */
                    if (extent > 0 && extent <= 256) {
                        extern uint32_t g_ws_backdrop_lo, g_ws_backdrop_hi;
                        uint32_t tbl_last = cpu->read_word(a1 + 4u + (uint32_t)(extent - 1) * 4u);
                        g_ws_backdrop_lo = a1;
                        g_ws_backdrop_hi = a1 + tbl_last + 0x400u;
                    }
                }
                return 0;
            }
        }
    }

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
            exec_delay_slot(cpu, pc + 4);
            cpu->pc = target;
            return 1;
        }
        case 0x09: { /* JALR rd, rs */
            uint32_t target = cpu->gpr[rs];
            uint32_t return_pc = pc + 8;
            cpu->gpr[rd ? rd : 31] = return_pc;
            cpu->gpr[0] = 0;
            exec_delay_slot(cpu, pc + 4);
            uint32_t site_sp = cpu->gpr[29];  /* call contract: sp at the call */
#ifdef PSX_HAS_GAME_DISPATCH
            cpu->pc = 0;
            if (interp_enter_compiled(cpu, target)) {
                if (g_psx_call_bail) return 1;  /* wild unwind: cpu->pc = true target */
                if (cpu->pc != 0) return 1;
                if (rd == 0 || rd == 31) {
                    if (psx_call_contract(cpu, return_pc, site_sp)) return 1;
                }
                *next_pc_out = return_pc;
                return 0;
            }
#endif
            /* Native overlay candidates get the SAME call contract as
             * statically-compiled callees: run as a unit, resume at
             * return_pc. A bare pc-chain here loses the return obligation
             * when the callee runs natively (C return, pc==0) — the dispatch
             * loop unwinds past the suspended caller, leaking its frame. */
            {
                extern int overlay_loader_call_native(CPUState *cpu, uint32_t addr);
                cpu->pc = 0;
                if (overlay_loader_call_native(cpu, target)) {
                    if (g_psx_call_bail) return 1;
                    if (cpu->pc != 0) return 1;
                    if (rd == 0 || rd == 31) {
                        if (psx_call_contract(cpu, return_pc, site_sp)) return 1;
                    }
                    *next_pc_out = return_pc;
                    return 0;
                }
            }
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
        exec_delay_slot(cpu, pc + 4);
        cpu->pc = target;
        return 1;
    }
    case 0x03: { /* JAL target */
        uint32_t target = ((pc + 4) & 0xF0000000u) | (target26(insn) << 2);
        uint32_t return_pc = pc + 8;
        cpu->gpr[31] = return_pc;
        exec_delay_slot(cpu, pc + 4);
        uint32_t site_sp = cpu->gpr[29];  /* call contract: sp at the call */
#ifdef PSX_HAS_GAME_DISPATCH
        cpu->pc = 0;
        if (interp_enter_compiled(cpu, target)) {
            if (g_psx_call_bail) return 1;  /* wild unwind: cpu->pc = true target */
            if (cpu->pc != 0) return 1;
            if (psx_call_contract(cpu, return_pc, site_sp)) return 1;
            *next_pc_out = return_pc;
            return 0;
        }
#endif
        /* Native overlay candidates get the SAME call contract as statically-
         * compiled callees: run as a unit, resume at return_pc. A bare
         * pc-chain here loses the return obligation when the callee runs
         * natively (C return, pc==0) — the dispatch loop unwinds past the
         * suspended caller, leaking its frame (dwarf->overworld root cause:
         * F=0x800338A8's epilogue skipped, sp leaked 0x18 per entity). */
        {
            extern int overlay_loader_call_native(CPUState *cpu, uint32_t addr);
            cpu->pc = 0;
            if (overlay_loader_call_native(cpu, target)) {
                if (g_psx_call_bail) return 1;
                if (cpu->pc != 0) return 1;
                if (psx_call_contract(cpu, return_pc, site_sp)) return 1;
                *next_pc_out = return_pc;
                return 0;
            }
        }
        if (!is_local_dirty_target(target)) {
            return dispatch_nonlocal_call(cpu, target, return_pc, next_pc_out);
        }
        if (!dirty_ram_word_looks_decodable(fetch_word(target & 0x1FFFFFFFu))) {
            return dispatch_nonlocal_call(cpu, target, return_pc, next_pc_out);
        }
        cpu->pc = target;
        return 1;
    }
    case 0x04: { /* BEQ rs, rt, simm */
        int taken = (cpu->gpr[rs] == cpu->gpr[rt]);
        exec_delay_slot(cpu, pc + 4);
        cpu->pc = taken ? (pc + 4 + (simm << 2)) : (pc + 8);
        return 1;
    }
    case 0x05: { /* BNE */
        int taken = (cpu->gpr[rs] != cpu->gpr[rt]);
        exec_delay_slot(cpu, pc + 4);
        cpu->pc = taken ? (pc + 4 + (simm << 2)) : (pc + 8);
        return 1;
    }
    case 0x06: { /* BLEZ */
        int taken = ((int32_t)cpu->gpr[rs] <= 0);
        exec_delay_slot(cpu, pc + 4);
        cpu->pc = taken ? (pc + 4 + (simm << 2)) : (pc + 8);
        return 1;
    }
    case 0x07: { /* BGTZ */
        int taken = ((int32_t)cpu->gpr[rs] > 0);
        exec_delay_slot(cpu, pc + 4);
        cpu->pc = taken ? (pc + 4 + (simm << 2)) : (pc + 8);
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
        exec_delay_slot(cpu, pc + 4);
        cpu->pc = taken ? (pc + 4 + (simm << 2)) : (pc + 8);
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
        /* Widescreen render-funnel cull widening (auto_screen_x): always apply
         * the shared helper for a flagged render-cull site — it is byte-identical
         * to the vanilla compare at 4:3 (margin 0) and widens at 16:9, so the one
         * code path serves both aspects (no widescreen-specific caching). */
        if ((imm == 0x140 || imm == 0x141) && ws_cull_site(pc))
            cpu->gpr[rt] = (uint32_t)psx_ws_cull_sltiu(cpu->gpr[rs], imm);
        else
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
            uint32_t sr = cpu->cop0[12];
            cpu->cop0[12] = (sr & 0xFFFFFFF0u) | ((sr >> 2) & 0x0Fu);
            return 0;
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
        uint16_t val  = (uint16_t)cpu->gpr[rt];
        /* Widescreen backdrop screenX squash on the interpreter path: mirrors
         * the recompiler emit at [widescreen.backdrop] x_sites. Overlay code
         * (the parallax backdrop handlers) runs interpreted when no cache DLL
         * is loaded, so without this the squash never happens. Identity at 4:3
         * (psx_ws_backdrop_x gates on ws_active). */
        if (psx_ws_is_backdrop_site(pc))
            val = (uint16_t)psx_ws_backdrop_x((int16_t)val);
        cpu->write_half(addr, val);
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

static int dirty_ram_dispatch_inner(CPUState* cpu, uint32_t addr, uint32_t stop_addr);

/* Public entry point.  Caller (psx_dispatch) has translated `addr` to a
 * KSEG-stripped form already in some cases, so accept any address and
 * mask. Returns 1 if interpretation handled the basic block; 0 if the
 * caller should fall back (e.g. unsupported opcode at the entry, page
 * not dirty).
 *
 * Thin wrapper that brackets the body with g_dirty_interp_active so the
 * event-timeline ring can tag events as INTERP vs STATIC. The EPC-sentinel
 * branch inside can longjmp out (not restoring here); psx_check_interrupts
 * clears the flag at its longjmp landing to cover that case. */
/* Always-on runaway-recursion guard. The host C stack mirrors the guest call
 * graph (each guest call = a nested psx_dispatch_call -> dirty_ram_dispatch C
 * frame), so an unbounded self-recursion — e.g. a bogus jump-table handler that
 * re-enters its own entity loop (issue #1 seesaw crash: v0=0x8001DFF8) — silently
 * overflows the host stack with no diagnostic (raw STATUS_STACK_OVERFLOW). Trip
 * well below overflow: dump full state (crash report carries dispatch_depth + the
 * dirty_block cycle that NAMES the recursing PC) and halt-and-serve, so the next
 * reproduction (whoever hits it) is captured cleanly + live-inspectable instead of
 * a bare SEH. Normal guest nesting is shallow (<~64); the default 256 sits far
 * above that and below overflow (~500+ frames on a 1 MB stack). Override via
 * PSX_RECURSION_LIMIT. One-shot. NOT a fix — the upstream bogus-pointer divergence
 * still needs the oracle; this converts a hard crash into a catchable, diagnosed
 * bail (Rule 15: make the failure observable). */
int dirty_ram_dispatch(CPUState* cpu, uint32_t addr, uint32_t stop_addr) {
    extern int g_psx_dispatch_depth;
    extern void psx_fatal_halt(const char *reason);
    static int  s_rec_guard = 0;
    static int  s_rec_limit = 0;
    if (s_rec_limit == 0) {
        const char *e = getenv("PSX_RECURSION_LIMIT");
        s_rec_limit = (e && *e) ? atoi(e) : 256;
        if (s_rec_limit < 32) s_rec_limit = 32;
    }
    if (!s_rec_guard && g_psx_dispatch_depth > s_rec_limit) {
        s_rec_guard = 1;   /* one-shot: first trip wins the report */
        psx_fatal_halt("dispatch recursion guard tripped — runaway self-call "
                       "(see dispatch_depth + dirty_block cycle for the recursing PC)");
    }
    if (addr == 0x8001A954u)      site_note(&g_site_dd954);   /* loop head re-dispatch */
    else if (addr == 0x80046264u) site_note(&g_site_dd264);   /* loop tail re-dispatch */
    int prev = g_dirty_interp_active;
    g_dirty_interp_active = 1;
    int r = dirty_ram_dispatch_inner(cpu, addr, stop_addr);
    g_dirty_interp_active = prev;
    return r;
}

static int dirty_ram_dispatch_inner(CPUState* cpu, uint32_t addr, uint32_t stop_addr) {
    uint32_t phys = addr & 0x1FFFFFFFu;

    if (addr == 0x80000048u) {
        cpu->pc = 0;
        if (psx_get_in_exception()) {
            psx_exception_longjmp(); /* does not return */
        }
        return 1;
    }

#ifdef PSX_HAS_GAME_DISPATCH
    if (psx_dispatch_game_compiled(cpu, addr)) return 1;
#endif

    /* B-2: statically-compiled overlay functions (generated/overlays_static.c).
     * Inert unless a game provides an overlays_static.c at build time. */
#ifdef PSX_HAS_OVERLAY_DISPATCH
    {
        extern int psx_overlay_dispatch(CPUState *cpu, uint32_t addr);
        if (psx_overlay_dispatch(cpu, addr)) return 1;
    }
#endif

    /* A-1: dynamically-loaded overlay DLL functions, checked before the
     * interpreter.  No-op (returns 0) until overlay_loader_init() runs, which
     * only happens when the overlay cache is enabled in config. */
    /* §5-E native↔interp fingerprint: capture entry register state for a
     * candidate overlay function, so native and interpreted runs can be diffed
     * by sequence. Additive only — no control-flow change. */
    extern int      overlay_loader_is_candidate(uint32_t phys);
    extern void     overlay_regs_snap(uint32_t out[34], const CPUState *cpu);
    extern void     overlay_fp_log(uint32_t addr, const uint32_t *in_regs,
                                   const CPUState *cpu, int native);
    int      _ovfp = overlay_cache_window_contains(phys) &&
                     overlay_loader_is_candidate(phys);
    uint32_t _in_regs[34];
    if (_ovfp) {
        overlay_regs_snap(_in_regs, cpu);
        /* One-shot insn-ring freeze BEFORE the Nth dispatch of the watched
         * candidate — the ring tail then holds the pre-divergence window. */
        if (g_insn_freeze_addr && phys == g_insn_freeze_addr &&
            ++g_insn_freeze_count == g_insn_freeze_nth)
            g_insn_log_frozen = 1;
    }

    {
        extern int overlay_loader_dispatch(CPUState *cpu, uint32_t addr);
        if (overlay_loader_dispatch(cpu, addr)) {
            if (_ovfp) overlay_fp_log(addr, _in_regs, cpu, 1);
            return 1;
        }
    }
#define OV_FPLOG_RET1() do { if (_ovfp) overlay_fp_log(addr, _in_regs, cpu, 0); return 1; } while (0)

    if (!dirty_ram_is_dirty(phys)) return 0;

    /* Interp-pressure signal for variant-capture automation (step 2.8):
     * counts dispatches the interpreter actually handles inside a capture
     * window. The autocapture tick reads-and-resets this to decide whether
     * an unseen region variant is being interp-executed right now. */
    if (overlay_cache_window_contains(phys)) g_dirty_window_dispatches++;

    /* Reset soft-fail state at block entry. */
    g_unsupported_seen = 0;
    /* Overlay region only — kernel window stays per-block (see
     * is_local_dirty_target). */
    int allow_local_dirty_flow = (phys >= OVERLAY_REGION_FLOOR);

    /* Per-PC entry counter (visible via dirty_ram_stats). */
    DirtyRamPcEntry *pc_entry = pc_table_get_or_insert(phys);
    if (pc_entry) pc_entry->hits++;

    /* External-entry attribution: when the previous interp run exited by
     * handing a target back to the dispatch loop, the very next dirty
     * dispatch at that target is the SAME logical execution continuing —
     * not a new entry. Anything else arrived from native code (a call or
     * a fresh dispatch) and is real interior-entry evidence for alias
     * seeding. */
    if (pc_entry && addr != g_dirty_interp_chain_target) pc_entry->entry_hits++;
    g_dirty_interp_chain_target = 0;

    /* Block-entry ring buffer — answers "who tried to JALR into this RAM
     * stub" by capturing cpu->gpr[31] (the caller's RA) at dispatch time.
     * Always-on; eviction keeps memory bounded. Honors the capture freeze. */
    if (!g_insn_log_frozen) {
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

    /* Run dirty code locally until it returns to compiled/non-dirty code.
     * Runtime-loaded overlays are larger than BIOS install stubs, so stopping
     * at every local branch burns the dispatch loop. */
    enum { MAX_INSNS_PER_DISPATCH = 1000000 };
    uint32_t pc = addr;
    int insns_executed = 0;
    for (int i = 0; i < MAX_INSNS_PER_DISPATCH; i++) {
        uint32_t next_pc = 0;
        uint32_t insn = fetch_word(pc & 0x1FFFFFFFu);
        uint32_t before_s0 = cpu->gpr[16];
        int transferred = exec_one(cpu, pc, &next_pc);
#ifdef PSX_ENABLE_BLOCK_CYCLES
        psx_advance_cycles(1u);
#endif
        dirty_ram_log_instruction(cpu, pc, insn, before_s0, next_pc,
                                  transferred ? cpu->pc : next_pc,
                                  transferred);
        if (g_unsupported_seen) {
            if (insns_executed == 0) {
                /* Couldn't decode the first instruction.  Most likely
                 * dispatch landed in a dirty page that's not actually
                 * code (stale data, return-target into save area, etc.).
                 * Hand off to psx_unknown_dispatch which has its own
                 * pattern-matching trampoline resolver. */
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
            OV_FPLOG_RET1();
        }
        g_dirty_ram_insns_run++;
        insns_executed++;
        if (transferred) {
            if (g_psx_call_bail) {
                /* A bail unwind began inside a surfaced call: stop the interp
                 * run and hand the wild target (cpu->pc) up to the dispatch
                 * loop's bail handling. */
                g_dirty_ram_blocks_run++;
                if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
                OV_FPLOG_RET1();
            }
            uint32_t target = cpu->pc;
#ifdef PSX_HAS_GAME_DISPATCH
            if (target != 0) {
                /* Tail transfer into compiled code: run with pc cleared so a
                 * plain C return surfaces as a genuine return (pc==0) to the
                 * dispatch loop's contract checks, instead of leaving the
                 * stale target in pc (which would re-dispatch — and re-run —
                 * the same function). */
                cpu->pc = 0;
                if (interp_enter_compiled(cpu, target)) {
                    g_dirty_ram_blocks_run++;
                    if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
                    OV_FPLOG_RET1();
                }
                cpu->pc = target;  /* not compiled: restore surfaced target */
            }
#endif
            uint32_t target_phys = target & 0x1FFFFFFFu;
            if (allow_local_dirty_flow && target != 0 &&
                target != stop_addr &&
                target_phys >= OVERLAY_REGION_FLOOR &&
                dirty_ram_is_dirty(target_phys)) {
                /* Capture freeze gates ONLY the ring write — never flow. */
                if (!g_insn_log_frozen) {
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
                }
                pc = target;
                if ((insns_executed & 0xFFF) == 0) {
                    debug_server_poll();
                    debug_server_wait_if_paused();
                    psx_check_interrupts(cpu);
                }
                continue;
            }
            g_dirty_ram_blocks_run++;
            if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
            g_dirty_interp_chain_target = cpu->pc;
            OV_FPLOG_RET1();
        }
        pc = next_pc;
        /* Straight-line flow reaching the dispatch return contract — exit
         * so the loop returns into the suspended native caller (same
         * hazard as a transfer to stop_addr). */
        if (stop_addr != 0 && pc == stop_addr) {
            cpu->pc = pc;
            g_dirty_ram_blocks_run++;
            if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
            g_dirty_interp_chain_target = pc;
            OV_FPLOG_RET1();
        }
        /* Straight-line code that left the dirty page — hand back to
         * static dispatch by setting cpu->pc and returning. */
        uint32_t next_phys = pc & 0x1FFFFFFFu;
        if (!dirty_ram_is_dirty(next_phys)) {
            cpu->pc = pc;
            g_dirty_ram_blocks_run++;
            if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
            g_dirty_interp_chain_target = pc;
            OV_FPLOG_RET1();
        }
    }
    g_dirty_ram_last_unsupported_pc = pc;
    g_dirty_ram_last_unsupported_insn = fetch_word(pc & 0x1FFFFFFFu);
    g_dirty_ram_last_unsupported_reason = "instruction guard";
    g_dirty_ram_guard_yields++;
    g_dirty_ram_blocks_run++;
    cpu->pc = pc;
    if (pc_entry) pc_entry->insns += (uint64_t)insns_executed;
    g_dirty_interp_chain_target = pc;
    psx_check_interrupts(cpu);
    OV_FPLOG_RET1();
}
#undef OV_FPLOG_RET1
