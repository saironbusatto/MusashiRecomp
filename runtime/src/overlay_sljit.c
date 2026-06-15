/* overlay_sljit.c — Tier-2 self-contained in-process JIT backend (sljit).
 * See overlay_sljit.h for the tier model and the precision-over-recall SAFETY
 * CONTRACT. This file currently provides: backend-selection policy, sljit
 * availability + a real codegen smoke test, and the try_compile entry point
 * that (until the MIPS->sljit emitter lands) safely declines every fragment to
 * the interpreter. The validated gcc path is untouched. */

#include "overlay_sljit.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "sljitLir.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

/* ---- counters ---------------------------------------------------------- */
static OverlayBackend s_active   = OVERLAY_BACKEND_AUTO;
static int            s_resolved = 0;
static int            s_selftest_ok = -1; /* -1 = not run */
static uint64_t       s_compiles = 0;
static uint64_t       s_declines = 0;
static uint64_t       s_bytes    = 0;
static char           s_last_msg[256];

static void sljit_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_last_msg, sizeof(s_last_msg), fmt, ap);
    va_end(ap);
}

/* ---- backend selection policy ----------------------------------------- */
const char *overlay_backend_name(OverlayBackend b) {
    switch (b) {
        case OVERLAY_BACKEND_GCC:   return "gcc";
        case OVERLAY_BACKEND_SLJIT: return "sljit";
        default:                    return "auto";
    }
}

static OverlayBackend parse_backend(const char *s, OverlayBackend dflt) {
    if (!s || !*s) return dflt;
    if (!strcmp(s, "gcc"))   return OVERLAY_BACKEND_GCC;
    if (!strcmp(s, "sljit")) return OVERLAY_BACKEND_SLJIT;
    if (!strcmp(s, "auto"))  return OVERLAY_BACKEND_AUTO;
    return dflt;
}

OverlayBackend overlay_backend_resolve(const char *cfg, int autocompile_configured) {
    /* Precedence: env PSX_OVERLAY_BACKEND > game.toml [runtime] overlay_backend
     * (cfg) > AUTO. AUTO prefers gcc when a compile command is wired (a dev
     * machine), else sljit (self-contained production / toolchain-less dev). */
    OverlayBackend want = parse_backend(getenv("PSX_OVERLAY_BACKEND"),
                                        parse_backend(cfg, OVERLAY_BACKEND_AUTO));
    OverlayBackend eff = want;
    if (want == OVERLAY_BACKEND_AUTO)
        eff = autocompile_configured ? OVERLAY_BACKEND_GCC : OVERLAY_BACKEND_SLJIT;

    s_active   = eff;
    s_resolved = 1;
    sljit_log("backend resolved: want=%s effective=%s (autocompile=%d)",
              overlay_backend_name(want), overlay_backend_name(eff),
              autocompile_configured);
    return eff;
}

OverlayBackend overlay_backend_active(void) { return s_active; }

int overlay_sljit_available(void) { return 1; }

/* ---- smoke test: JIT a trivial leaf and run it ------------------------- */
/* Produces machine code for `sljit_sw f(sljit_sw a) { return a + 1234; }`,
 * runs it, and checks the result. Proves the codegen + executable allocator
 * work in this build/host. */
typedef sljit_sw (SLJIT_FUNC *SmokeFn)(sljit_sw);

int overlay_sljit_selftest(void) {
    if (s_selftest_ok >= 0) return s_selftest_ok;

    struct sljit_compiler *C = sljit_create_compiler(NULL);
    if (!C) { s_selftest_ok = 0; sljit_log("selftest: create_compiler failed"); return 0; }

    /* one arg (W) -> arrives in saved reg S0; one scratch, one saved, no locals */
    sljit_emit_enter(C, 0, SLJIT_ARGS1(W, W), 1, 1, 0);
    sljit_emit_op2(C, SLJIT_ADD, SLJIT_R0, 0, SLJIT_S0, 0, SLJIT_IMM, 1234);
    sljit_emit_return(C, SLJIT_MOV, SLJIT_R0, 0);

    void *code = sljit_generate_code(C, 0, NULL);
    sljit_uw code_size = sljit_get_generated_code_size(C);
    sljit_free_compiler(C);

    if (!code) { s_selftest_ok = 0; sljit_log("selftest: generate_code failed"); return 0; }

    SmokeFn fn = (SmokeFn)code;
    sljit_sw got = fn(1000);
    sljit_free_code(code, NULL);

    s_selftest_ok = (got == 2234) ? 1 : 0;
    sljit_log("selftest: f(1000)=%ld expected=2234 ok=%d code_size=%lu",
              (long)got, s_selftest_ok, (unsigned long)code_size);
    return s_selftest_ok;
}

/* ======================================================================== *
 *  MIPS -> sljit emitter (SLJIT.md §7 step 4, first slice)
 *
 *  Parallels dirty_ram_interp.c's exec_one — same decode, but emits host code
 *  instead of stepping. PARITY RULES:
 *    - GPRs are MEMORY-BACKED in the CPUState struct (gpr[n] at [S0 + 4n]); each
 *      instruction loads operands, computes, stores back. gpr[0] is never
 *      written (hardwired 0); reads of gpr[0] read the struct's 0. No host
 *      register allocation across instructions (a later pass).
 *    - 32-bit ops everywhere (SLJIT_*32) so wraparound matches the R3000A.
 *    - memory access routes through the cpu read/write callbacks (icall) exactly
 *      like the interpreter, so MMIO / page-watch / dirty-tracking behave alike.
 *  FIRST SLICE shape: single-block LEAF functions only, terminator `jr $ra`.
 *  ANY other control transfer / unsupported opcode aborts the WHOLE fragment
 *  (out->fn = NULL) and the caller runs the interpreter (precision over recall).
 * ======================================================================== */

/* cpu pointer lives in S0 after emit_enter(SLJIT_ARGS1V(P), ...). */
#define R_CPU    SLJIT_S0
#define GPR_OFF(n)  ((sljit_sw)(offsetof(CPUState, gpr) + 4u * (n)))

/* MIPS field decoders (mirror dirty_ram_interp.c). */
static inline uint32_t f_op   (uint32_t i) { return (i >> 26) & 0x3Fu; }
static inline uint32_t f_rs   (uint32_t i) { return (i >> 21) & 0x1Fu; }
static inline uint32_t f_rt   (uint32_t i) { return (i >> 16) & 0x1Fu; }
static inline uint32_t f_rd   (uint32_t i) { return (i >> 11) & 0x1Fu; }
static inline uint32_t f_sh   (uint32_t i) { return (i >>  6) & 0x1Fu; }
static inline uint32_t f_fn   (uint32_t i) { return  i        & 0x3Fu; }
static inline uint32_t f_imm  (uint32_t i) { return  i        & 0xFFFFu; }
static inline uint32_t f_tgt26(uint32_t i) { return  i        & 0x03FFFFFFu; }
static inline sljit_sw f_simm (uint32_t i) { return (sljit_sw)(int32_t)(int16_t)f_imm(i); }

/* gpr[n] -> reg (reads hardwired 0 for n==0 directly from the struct). */
static void ld_gpr(struct sljit_compiler *C, sljit_s32 reg, uint32_t n) {
    sljit_emit_op1(C, SLJIT_MOV32, reg, 0, SLJIT_MEM1(R_CPU), GPR_OFF(n));
}
/* reg -> gpr[n] (skips gpr[0] — hardwired). */
static void st_gpr(struct sljit_compiler *C, uint32_t n, sljit_s32 reg) {
    if (n == 0) return;
    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), GPR_OFF(n), reg, 0);
}

/* rd = rs <op2> rt (register-register ALU). */
static void emit_alu_rr(struct sljit_compiler *C, sljit_s32 op,
                        uint32_t rd, uint32_t rs, uint32_t rt) {
    ld_gpr(C, SLJIT_R0, rs);
    ld_gpr(C, SLJIT_R1, rt);
    sljit_emit_op2(C, op, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
    st_gpr(C, rd, SLJIT_R0);
}

/* rd = rt <shift> sh (immediate shift). */
static void emit_shift_imm(struct sljit_compiler *C, sljit_s32 op,
                           uint32_t rd, uint32_t rt, uint32_t sh) {
    ld_gpr(C, SLJIT_R0, rt);
    sljit_emit_op2(C, op, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)sh);
    st_gpr(C, rd, SLJIT_R0);
}

/* rd = rt <shift> (rs & 31) (variable shift). */
static void emit_shift_var(struct sljit_compiler *C, sljit_s32 op,
                           uint32_t rd, uint32_t rt, uint32_t rs) {
    ld_gpr(C, SLJIT_R1, rs);
    sljit_emit_op2(C, SLJIT_AND32, SLJIT_R1, 0, SLJIT_R1, 0, SLJIT_IMM, 31);
    ld_gpr(C, SLJIT_R0, rt);
    sljit_emit_op2(C, op, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
    st_gpr(C, rd, SLJIT_R0);
}

/* dst = (a <cond> b) ? 1 : 0, via flags. a/b already in R0/R1 or R0/IMM. */
static void emit_setcc(struct sljit_compiler *C, sljit_s32 subop_setflag,
                       sljit_s32 cc, uint32_t dst,
                       sljit_s32 b_reg, sljit_sw b_imm) {
    sljit_emit_op2u(C, subop_setflag, SLJIT_R0, 0,
                    (b_reg == SLJIT_IMM) ? SLJIT_IMM : b_reg, b_imm);
    sljit_emit_op_flags(C, SLJIT_MOV32, SLJIT_R0, 0, cc);
    st_gpr(C, dst, SLJIT_R0);
}

/* Load via cpu->read_* (icall ARGS1(32,32)); addr = gpr[rs] + simm. `ext`
 * is the post-call sign/zero extension op (SLJIT_MOV32 for word). */
static void emit_load(struct sljit_compiler *C, size_t fnoff, sljit_s32 ext,
                      uint32_t rt, uint32_t rs, sljit_sw simm) {
    ld_gpr(C, SLJIT_R0, rs);
    if (simm) sljit_emit_op2(C, SLJIT_ADD32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, simm);
    /* target fn ptr -> R1 (full word); arg addr already in R0. */
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(R_CPU), (sljit_sw)fnoff);
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS1(32, 32), SLJIT_R1, 0);
    if (ext != SLJIT_MOV32)
        sljit_emit_op1(C, ext, SLJIT_R0, 0, SLJIT_R0, 0);
    st_gpr(C, rt, SLJIT_R0);
}

/* Store via cpu->write_* (icall ARGS2V(32,32)); addr = gpr[rs]+simm, val=gpr[rt]. */
static void emit_store(struct sljit_compiler *C, size_t fnoff,
                       uint32_t rt, uint32_t rs, sljit_sw simm) {
    ld_gpr(C, SLJIT_R0, rs);
    if (simm) sljit_emit_op2(C, SLJIT_ADD32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, simm);
    ld_gpr(C, SLJIT_R1, rt);
    sljit_emit_op1(C, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(R_CPU), (sljit_sw)fnoff);
    sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS2V(32, 32), SLJIT_R2, 0);
}

enum { EMIT_OK = 0, EMIT_TERM = 1, EMIT_ABORT = 2 };

/* Emit ONE non-control instruction. Returns EMIT_OK, or EMIT_TERM iff `insn`
 * is `jr $ra` (the only terminator this slice supports — caller then emits the
 * delay slot + the shard return), or EMIT_ABORT for anything outside the slice
 * (any other control transfer, or an unsupported opcode). */
static int emit_one(struct sljit_compiler *C, uint32_t insn) {
    uint32_t op = f_op(insn), rs = f_rs(insn), rt = f_rt(insn);
    uint32_t rd = f_rd(insn), sh = f_sh(insn), fn = f_fn(insn);
    uint32_t imm = f_imm(insn);
    sljit_sw simm = f_simm(insn);

    switch (op) {
    case 0x00: /* SPECIAL */
        switch (fn) {
        case 0x00: emit_shift_imm(C, SLJIT_SHL32,  rd, rt, sh); return EMIT_OK; /* SLL (nop when 0) */
        case 0x02: emit_shift_imm(C, SLJIT_LSHR32, rd, rt, sh); return EMIT_OK; /* SRL */
        case 0x03: emit_shift_imm(C, SLJIT_ASHR32, rd, rt, sh); return EMIT_OK; /* SRA */
        case 0x04: emit_shift_var(C, SLJIT_SHL32,  rd, rt, rs); return EMIT_OK; /* SLLV */
        case 0x06: emit_shift_var(C, SLJIT_LSHR32, rd, rt, rs); return EMIT_OK; /* SRLV */
        case 0x07: emit_shift_var(C, SLJIT_ASHR32, rd, rt, rs); return EMIT_OK; /* SRAV */
        case 0x08: /* JR rs — terminator iff rs == $ra; else outside the slice */
            return (rs == 31) ? EMIT_TERM : EMIT_ABORT;
        case 0x0F: return EMIT_OK; /* SYNC = nop */
        case 0x20: case 0x21: emit_alu_rr(C, SLJIT_ADD32, rd, rs, rt); return EMIT_OK; /* ADD/ADDU */
        case 0x22: case 0x23: emit_alu_rr(C, SLJIT_SUB32, rd, rs, rt); return EMIT_OK; /* SUB/SUBU */
        case 0x24: emit_alu_rr(C, SLJIT_AND32, rd, rs, rt); return EMIT_OK; /* AND */
        case 0x25: emit_alu_rr(C, SLJIT_OR32,  rd, rs, rt); return EMIT_OK; /* OR */
        case 0x26: emit_alu_rr(C, SLJIT_XOR32, rd, rs, rt); return EMIT_OK; /* XOR */
        case 0x27: /* NOR: ~(rs|rt) */
            ld_gpr(C, SLJIT_R0, rs); ld_gpr(C, SLJIT_R1, rt);
            sljit_emit_op2(C, SLJIT_OR32,  SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
            sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)-1);
            st_gpr(C, rd, SLJIT_R0);
            return EMIT_OK;
        case 0x2A: /* SLT (signed) */
            ld_gpr(C, SLJIT_R0, rs); ld_gpr(C, SLJIT_R1, rt);
            emit_setcc(C, SLJIT_SUB32 | SLJIT_SET_SIG_LESS, SLJIT_SIG_LESS, rd, SLJIT_R1, 0);
            return EMIT_OK;
        case 0x2B: /* SLTU (unsigned) */
            ld_gpr(C, SLJIT_R0, rs); ld_gpr(C, SLJIT_R1, rt);
            emit_setcc(C, SLJIT_SUB32 | SLJIT_SET_LESS, SLJIT_LESS, rd, SLJIT_R1, 0);
            return EMIT_OK;
        case 0x10: /* MFHI rd = hi */
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_MEM1(R_CPU),
                           (sljit_sw)offsetof(CPUState, hi));
            st_gpr(C, rd, SLJIT_R0);
            return EMIT_OK;
        case 0x12: /* MFLO rd = lo */
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_MEM1(R_CPU),
                           (sljit_sw)offsetof(CPUState, lo));
            st_gpr(C, rd, SLJIT_R0);
            return EMIT_OK;
        case 0x11: /* MTHI hi = rs */
            ld_gpr(C, SLJIT_R0, rs);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU),
                           (sljit_sw)offsetof(CPUState, hi), SLJIT_R0, 0);
            return EMIT_OK;
        case 0x13: /* MTLO lo = rs */
            ld_gpr(C, SLJIT_R0, rs);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU),
                           (sljit_sw)offsetof(CPUState, lo), SLJIT_R0, 0);
            return EMIT_OK;
        case 0x18: /* MULT (signed 32x32 -> 64): hi:lo = sext(rs) * sext(rt) */
        case 0x19: /* MULTU (unsigned) */
            /* Extend both operands to a full host word so a single word MUL gives
             * the true 64-bit product; split into lo (low 32) and hi (>>32, the
             * raw upper 32 — interp does (uint64_t)r >> 32, a logical shift). */
            sljit_emit_op1(C, (fn == 0x18) ? SLJIT_MOV_S32 : SLJIT_MOV_U32,
                           SLJIT_R0, 0, SLJIT_MEM1(R_CPU), GPR_OFF(rs));
            sljit_emit_op1(C, (fn == 0x18) ? SLJIT_MOV_S32 : SLJIT_MOV_U32,
                           SLJIT_R1, 0, SLJIT_MEM1(R_CPU), GPR_OFF(rt));
            sljit_emit_op2(C, SLJIT_MUL, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R1, 0);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU),
                           (sljit_sw)offsetof(CPUState, lo), SLJIT_R0, 0);
            sljit_emit_op2(C, SLJIT_LSHR, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 32);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU),
                           (sljit_sw)offsetof(CPUState, hi), SLJIT_R0, 0);
            return EMIT_OK;
        case 0x1A: { /* DIV (signed). Host idiv traps on /0 and INT_MIN/-1, so
                      * branch-guard both to the interpreter's defined results
                      * BEFORE the DIVMOD ever runs. */
            ld_gpr(C, SLJIT_R0, rs);   /* a */
            ld_gpr(C, SLJIT_R1, rt);   /* b */
            struct sljit_jump *j_b0 =
                sljit_emit_cmp(C, SLJIT_EQUAL | SLJIT_32, SLJIT_R1, 0, SLJIT_IMM, 0);
            struct sljit_jump *j_n1 =
                sljit_emit_cmp(C, SLJIT_NOT_EQUAL | SLJIT_32, SLJIT_R0, 0,
                               SLJIT_IMM, (sljit_sw)0x80000000);
            struct sljit_jump *j_n2 =
                sljit_emit_cmp(C, SLJIT_NOT_EQUAL | SLJIT_32, SLJIT_R1, 0, SLJIT_IMM, -1);
            /* overflow a==INT_MIN, b==-1: lo=0x80000000, hi=0 */
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), (sljit_sw)offsetof(CPUState, lo),
                           SLJIT_IMM, (sljit_sw)0x80000000);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), (sljit_sw)offsetof(CPUState, hi),
                           SLJIT_IMM, 0);
            struct sljit_jump *j_da = sljit_emit_jump(C, SLJIT_JUMP);
            /* normal: lo=a/b, hi=a%b */
            struct sljit_label *Ln = sljit_emit_label(C);
            sljit_set_label(j_n1, Ln); sljit_set_label(j_n2, Ln);
            sljit_emit_op0(C, SLJIT_DIVMOD_S32);   /* R0=R0/R1, R1=R0%R1 */
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), (sljit_sw)offsetof(CPUState, lo), SLJIT_R0, 0);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), (sljit_sw)offsetof(CPUState, hi), SLJIT_R1, 0);
            struct sljit_jump *j_db = sljit_emit_jump(C, SLJIT_JUMP);
            /* b==0: lo=(a<0)?1:-1, hi=a (a still in R0) */
            struct sljit_label *Lb0 = sljit_emit_label(C);
            sljit_set_label(j_b0, Lb0);
            sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_SIG_LESS, SLJIT_R0, 0, SLJIT_IMM, 0);
            sljit_emit_op_flags(C, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_SIG_LESS); /* (a<0)?1:0 */
            sljit_emit_op2(C, SLJIT_SHL32, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, 1);
            sljit_emit_op2(C, SLJIT_SUB32, SLJIT_R2, 0, SLJIT_R2, 0, SLJIT_IMM, 1); /* 1 or -1 */
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), (sljit_sw)offsetof(CPUState, lo), SLJIT_R2, 0);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), (sljit_sw)offsetof(CPUState, hi), SLJIT_R0, 0);
            struct sljit_label *Ld = sljit_emit_label(C);
            sljit_set_label(j_da, Ld); sljit_set_label(j_db, Ld);
            return EMIT_OK;
        }
        case 0x1B: { /* DIVU (unsigned). Guard /0 to interp's defined result. */
            ld_gpr(C, SLJIT_R0, rs);   /* a */
            ld_gpr(C, SLJIT_R1, rt);   /* b */
            struct sljit_jump *j_d0 =
                sljit_emit_cmp(C, SLJIT_EQUAL | SLJIT_32, SLJIT_R1, 0, SLJIT_IMM, 0);
            sljit_emit_op0(C, SLJIT_DIVMOD_U32);   /* R0=R0/R1, R1=R0%R1 */
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), (sljit_sw)offsetof(CPUState, lo), SLJIT_R0, 0);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), (sljit_sw)offsetof(CPUState, hi), SLJIT_R1, 0);
            struct sljit_jump *j_dn = sljit_emit_jump(C, SLJIT_JUMP);
            /* b==0: lo=0xFFFFFFFF, hi=a */
            struct sljit_label *Ld0 = sljit_emit_label(C);
            sljit_set_label(j_d0, Ld0);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), (sljit_sw)offsetof(CPUState, lo),
                           SLJIT_IMM, (sljit_sw)0xFFFFFFFF);
            sljit_emit_op1(C, SLJIT_MOV32, SLJIT_MEM1(R_CPU), (sljit_sw)offsetof(CPUState, hi), SLJIT_R0, 0);
            struct sljit_label *Ld = sljit_emit_label(C);
            sljit_set_label(j_dn, Ld);
            return EMIT_OK;
        }
        default: return EMIT_ABORT;
        }
    case 0x08: case 0x09: /* ADDI/ADDIU rt = rs + simm */
        ld_gpr(C, SLJIT_R0, rs);
        sljit_emit_op2(C, SLJIT_ADD32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, simm);
        st_gpr(C, rt, SLJIT_R0);
        return EMIT_OK;
    case 0x0A: /* SLTI (signed) */
        ld_gpr(C, SLJIT_R0, rs);
        emit_setcc(C, SLJIT_SUB32 | SLJIT_SET_SIG_LESS, SLJIT_SIG_LESS, rt, SLJIT_IMM, simm);
        return EMIT_OK;
    case 0x0B: /* SLTIU (unsigned, simm sign-extended then compared unsigned) */
        ld_gpr(C, SLJIT_R0, rs);
        emit_setcc(C, SLJIT_SUB32 | SLJIT_SET_LESS, SLJIT_LESS, rt, SLJIT_IMM, simm);
        return EMIT_OK;
    case 0x0C: /* ANDI (zero-extended imm) */
        ld_gpr(C, SLJIT_R0, rs);
        sljit_emit_op2(C, SLJIT_AND32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)imm);
        st_gpr(C, rt, SLJIT_R0);
        return EMIT_OK;
    case 0x0D: /* ORI */
        ld_gpr(C, SLJIT_R0, rs);
        sljit_emit_op2(C, SLJIT_OR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)imm);
        st_gpr(C, rt, SLJIT_R0);
        return EMIT_OK;
    case 0x0E: /* XORI */
        ld_gpr(C, SLJIT_R0, rs);
        sljit_emit_op2(C, SLJIT_XOR32, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)imm);
        st_gpr(C, rt, SLJIT_R0);
        return EMIT_OK;
    case 0x0F: /* LUI rt = imm << 16 (rt==0 is a nop — st_gpr skips it) */
        sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0, SLJIT_IMM, (sljit_sw)(imm << 16));
        st_gpr(C, rt, SLJIT_R0);
        return EMIT_OK;
    case 0x20: emit_load(C, offsetof(CPUState, read_byte), SLJIT_MOV_S8,  rt, rs, simm); return EMIT_OK; /* LB */
    case 0x21: emit_load(C, offsetof(CPUState, read_half), SLJIT_MOV_S16, rt, rs, simm); return EMIT_OK; /* LH */
    case 0x23: emit_load(C, offsetof(CPUState, read_word), SLJIT_MOV32,   rt, rs, simm); return EMIT_OK; /* LW */
    case 0x24: emit_load(C, offsetof(CPUState, read_byte), SLJIT_MOV_U8,  rt, rs, simm); return EMIT_OK; /* LBU */
    case 0x25: emit_load(C, offsetof(CPUState, read_half), SLJIT_MOV_U16, rt, rs, simm); return EMIT_OK; /* LHU */
    case 0x28: emit_store(C, offsetof(CPUState, write_byte), rt, rs, simm); return EMIT_OK; /* SB */
    case 0x29: emit_store(C, offsetof(CPUState, write_half), rt, rs, simm); return EMIT_OK; /* SH */
    case 0x2B: emit_store(C, offsetof(CPUState, write_word), rt, rs, simm); return EMIT_OK; /* SW */
    default: return EMIT_ABORT;
    }
}

/* ---- control-flow classification (no emission) ------------------------- */
enum { CTRL_NONE = 0, CTRL_RETURN, CTRL_BRANCH, CTRL_JUMP, CTRL_CALL, CTRL_ABORT };

/* Classify a possible control instruction. For CTRL_BRANCH (conditional) and
 * CTRL_JUMP (unconditional J), *out_tbyte is the target as a SIGNED byte offset
 * relative to the fragment entry. `off` is the instruction's fragment-relative
 * byte offset; `entry_phys` is the fragment's phys entry (for J's absolute
 * target). This slice handles `jr $ra` (return), PC-relative conditional
 * branches, and the absolute J (when it stays inside the fragment). JAL/JALR/
 * jr-non-ra/link-branches are CTRL_ABORT (decline the whole fragment). */
static int classify_control(uint32_t insn, uint32_t off, uint32_t entry_phys,
                            int32_t *out_tbyte) {
    uint32_t op = f_op(insn), fn = f_fn(insn), rs = f_rs(insn), rt = f_rt(insn);
    if (op == 0x00) {
        if (fn == 0x08) return (rs == 31) ? CTRL_RETURN : CTRL_ABORT; /* JR */
        if (fn == 0x09) return CTRL_CALL;                             /* JALR */
        return CTRL_NONE;
    }
    if (op == 0x03) return CTRL_CALL;                                 /* JAL */
    if (op == 0x02) {                            /* J target (absolute) */
        /* In KSEG the region's high bits cancel under the phys mask, so the
         * fragment-relative byte offset is just target_phys - entry_phys. */
        uint32_t target_phys = (f_tgt26(insn) << 2) & 0x1FFFFFFFu;
        *out_tbyte = (int32_t)target_phys - (int32_t)entry_phys;
        return CTRL_JUMP;
    }
    if (op >= 0x04 && op <= 0x07) {              /* BEQ/BNE/BLEZ/BGTZ */
        *out_tbyte = (int32_t)off + 4 + (int32_t)(f_simm(insn) << 2);
        return CTRL_BRANCH;
    }
    if (op == 0x01) {                            /* REGIMM */
        if (rt == 0x00 || rt == 0x01) {          /* BLTZ / BGEZ (no link) */
            *out_tbyte = (int32_t)off + 4 + (int32_t)(f_simm(insn) << 2);
            return CTRL_BRANCH;
        }
        return CTRL_ABORT;                        /* BLTZAL/BGEZAL/other */
    }
    return CTRL_NONE;
}

/* Compute a branch's taken/not-taken predicate (1/0) into S1, reading the
 * source registers at the BRANCH instruction (before its delay slot runs). */
static void emit_cond_to_S1(struct sljit_compiler *C, uint32_t insn) {
    uint32_t op = f_op(insn), rs = f_rs(insn), rt = f_rt(insn);
    switch (op) {
    case 0x04: /* BEQ rs==rt */
        ld_gpr(C, SLJIT_R0, rs); ld_gpr(C, SLJIT_R1, rt);
        sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_Z, SLJIT_R0, 0, SLJIT_R1, 0);
        sljit_emit_op_flags(C, SLJIT_MOV32, SLJIT_S1, 0, SLJIT_EQUAL);
        break;
    case 0x05: /* BNE rs!=rt */
        ld_gpr(C, SLJIT_R0, rs); ld_gpr(C, SLJIT_R1, rt);
        sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_Z, SLJIT_R0, 0, SLJIT_R1, 0);
        sljit_emit_op_flags(C, SLJIT_MOV32, SLJIT_S1, 0, SLJIT_NOT_EQUAL);
        break;
    case 0x06: /* BLEZ rs<=0 */
        ld_gpr(C, SLJIT_R0, rs);
        sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_SIG_LESS_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
        sljit_emit_op_flags(C, SLJIT_MOV32, SLJIT_S1, 0, SLJIT_SIG_LESS_EQUAL);
        break;
    case 0x07: /* BGTZ rs>0 */
        ld_gpr(C, SLJIT_R0, rs);
        sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_SIG_GREATER, SLJIT_R0, 0, SLJIT_IMM, 0);
        sljit_emit_op_flags(C, SLJIT_MOV32, SLJIT_S1, 0, SLJIT_SIG_GREATER);
        break;
    default: /* REGIMM: BLTZ (rt==0) / BGEZ (rt==1) */
        ld_gpr(C, SLJIT_R0, rs);
        if (rt == 0x00) {
            sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_SIG_LESS, SLJIT_R0, 0, SLJIT_IMM, 0);
            sljit_emit_op_flags(C, SLJIT_MOV32, SLJIT_S1, 0, SLJIT_SIG_LESS);
        } else {
            sljit_emit_op2u(C, SLJIT_SUB32 | SLJIT_SET_SIG_GREATER_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
            sljit_emit_op_flags(C, SLJIT_MOV32, SLJIT_S1, 0, SLJIT_SIG_GREATER_EQUAL);
        }
        break;
    }
}

/* Read a little-endian word from the decode image at phys offset `off`. */
static inline uint32_t img_word(const uint8_t *b, uint32_t off) {
    return  (uint32_t)b[off]
         | ((uint32_t)b[off + 1] <<  8)
         | ((uint32_t)b[off + 2] << 16)
         | ((uint32_t)b[off + 3] << 24);
}

#define SLJIT_MAX_FRAG_INSNS 2048u
#define SLJIT_MAX_FRAG_CTRL  512u   /* branches/jumps per fragment cap */

void overlay_sljit_try_compile(uint32_t entry,
                               const uint8_t *bytes, uint32_t size,
                               uint32_t image_base_vram,
                               OverlaySljitResult *out) {
    out->fn = NULL; out->code_lo = 0; out->code_len = 0; out->insns = 0;
    if (!bytes) { s_declines++; return; }

    uint32_t entry_phys = entry & 0x1FFFFFFFu;
    uint32_t base_phys  = image_base_vram & 0x1FFFFFFFu;
    if (entry_phys < base_phys) { s_declines++; return; }
    uint32_t off0 = entry_phys - base_phys;

    /* ---- PASS 1: find the terminator, collect branch targets + delay-slot
     * offsets. Linear scan in memory order (branches don't break the layout);
     * the fragment is [entry, jr $ra + its delay slot). ----------------------*/
    static uint8_t is_target[SLJIT_MAX_FRAG_INSNS]; /* index by word            */
    static uint8_t is_ds[SLJIT_MAX_FRAG_INSNS];     /* word is a delay slot      */
    /* (static: avoids ~20 KB of stack; the emitter is single-threaded — JIT is
     * synchronous on the emu thread.) */
    struct { uint32_t bw; int32_t tbyte; } brs[SLJIT_MAX_FRAG_CTRL];
    int nbr = 0;
    int ncalls = 0;
    uint32_t frag_words = 0;
    int found_term = 0;

    memset(is_target, 0, sizeof is_target);
    memset(is_ds, 0, sizeof is_ds);

    for (uint32_t i = 0; i < SLJIT_MAX_FRAG_INSNS; i++) {
        uint32_t off = off0 + i * 4u;
        if (off + 4u > size) break;                 /* off image w/o terminator */
        uint32_t insn = img_word(bytes, off);
        int32_t tbyte = 0;
        int ctrl = classify_control(insn, i * 4u, entry_phys, &tbyte);
        if (ctrl == CTRL_ABORT) { s_declines++; return; }
        if (ctrl == CTRL_RETURN) {
            if (i + 1u < SLJIT_MAX_FRAG_INSNS) is_ds[i + 1u] = 1;
            frag_words  = i + 2u;                    /* jr + delay slot */
            found_term  = 1;
            break;
        }
        if (ctrl == CTRL_BRANCH || ctrl == CTRL_JUMP) {
            if (i + 1u < SLJIT_MAX_FRAG_INSNS) is_ds[i + 1u] = 1;
            if (nbr >= (int)SLJIT_MAX_FRAG_CTRL) { s_declines++; return; }
            brs[nbr].bw = i; brs[nbr].tbyte = tbyte; nbr++;
        }
        if (ctrl == CTRL_CALL) {           /* jal/jalr: delay slot, no fragment target */
            if (i + 1u < SLJIT_MAX_FRAG_INSNS) is_ds[i + 1u] = 1;
            ncalls++;
        }
        /* CTRL_NONE: straight-line, continue. */
    }
    if (!found_term || frag_words == 0 || frag_words > SLJIT_MAX_FRAG_INSNS) {
        s_declines++; return;
    }
    if (off0 + frag_words * 4u > size) { s_declines++; return; }  /* delay slot off image */

    /* Validate branch targets: in-range, aligned, not landing on a delay slot. */
    for (int b = 0; b < nbr; b++) {
        int32_t tb = brs[b].tbyte;
        if (tb < 0 || (tb & 3) != 0) { s_declines++; return; }
        uint32_t tw = (uint32_t)tb / 4u;
        if (tw >= frag_words || is_ds[tw]) { s_declines++; return; }
        is_target[tw] = 1;
    }

    /* ---- PASS 2: emit ----------------------------------------------------- */
    struct sljit_compiler *C = sljit_create_compiler(NULL);
    if (!C) { s_declines++; sljit_log("compile: create_compiler failed"); return; }
    /* void shard(CPUState* cpu): S0=cpu, S1=branch predicate / jalr target,
     * S2=psx_sljit_call address (only when the fragment has calls). Scratches
     * R0..R4 (operands, fn-ptr, addr/value, call args + icall headroom). */
    int saveds = (ncalls > 0) ? 3 : 2;
    sljit_emit_enter(C, 0, SLJIT_ARGS1V(P), 5, saveds, 0);
    if (ncalls > 0)
        sljit_emit_op1(C, SLJIT_MOV, SLJIT_S2, 0,
                       SLJIT_IMM, (sljit_sw)(uintptr_t)psx_sljit_call);

    static struct sljit_label *labels[SLJIT_MAX_FRAG_INSNS];
    struct { struct sljit_jump *j; uint32_t tw; } jmps[SLJIT_MAX_FRAG_CTRL];
    int njmp = 0;
    for (uint32_t i = 0; i < frag_words; i++) labels[i] = NULL;

    int aborted = 0;
    enum { PEND_NONE = 0, PEND_RET, PEND_BR, PEND_CALL } pending = PEND_NONE;
    uint32_t pend_tw = 0;
    int pend_cond = 0;          /* PEND_BR: 1 = conditional (S1), 0 = unconditional */
    uint32_t pend_call_target = 0;  /* PEND_CALL: jal absolute target           */
    uint32_t pend_call_return = 0;  /* return address (continuation)            */
    int      pend_call_dynamic = 0; /* 1 = jalr (target in S1), 0 = jal (const) */
    int      pend_call_check = 0;   /* apply the (ra,sp) contract after the call*/

    for (uint32_t i = 0; i < frag_words; i++) {
        if (is_target[i]) labels[i] = sljit_emit_label(C);
        uint32_t insn = img_word(bytes, off0 + i * 4u);

        if (pending == PEND_NONE) {
            int32_t tbyte = 0;
            int ctrl = classify_control(insn, i * 4u, entry_phys, &tbyte);
            if (ctrl == CTRL_ABORT) { aborted = 1; break; }
            if (ctrl == CTRL_RETURN) { pending = PEND_RET; continue; }
            if (ctrl == CTRL_BRANCH || ctrl == CTRL_JUMP) {
                if (ctrl == CTRL_BRANCH)
                    emit_cond_to_S1(C, insn);  /* predicate read BEFORE the delay slot */
                pending   = PEND_BR;
                pend_cond = (ctrl == CTRL_BRANCH);
                pend_tw   = (uint32_t)((int32_t)tbyte / 4);
                continue;
            }
            if (ctrl == CTRL_CALL) {
                /* Link write + (jalr) target capture happen BEFORE the delay slot
                 * (the delay slot may clobber rs); the dispatch happens AFTER it. */
                uint32_t cop = f_op(insn), crs = f_rs(insn), crd = f_rd(insn);
                uint32_t pc_virt = entry + i * 4u;
                pend_call_return = pc_virt + 8u;
                if (cop == 0x03) {                 /* JAL: link $ra, absolute target */
                    pend_call_dynamic = 0;
                    pend_call_target  = ((pc_virt + 4u) & 0xF0000000u) | (f_tgt26(insn) << 2);
                    pend_call_check   = 1;
                    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0,
                                   SLJIT_IMM, (sljit_sw)pend_call_return);
                    st_gpr(C, 31, SLJIT_R0);
                } else {                           /* JALR rd, rs: dynamic target */
                    uint32_t link = crd ? crd : 31u;
                    pend_call_dynamic = 1;
                    pend_call_check   = (crd == 0 || crd == 31);
                    ld_gpr(C, SLJIT_S1, crs);      /* capture target before delay slot */
                    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R0, 0,
                                   SLJIT_IMM, (sljit_sw)pend_call_return);
                    st_gpr(C, link, SLJIT_R0);
                }
                pending = PEND_CALL;
                continue;
            }
            if (emit_one(C, insn) != EMIT_OK) { aborted = 1; break; }
        } else {
            /* This instruction is the delay slot of the pending control insn;
             * it must be a plain, supported, non-control op (the constraint the
             * interpreter's exec_delay_slot enforces). It executes regardless of
             * branch outcome, so emit it, THEN apply the pending transfer. */
            int32_t dummy = 0;
            if (classify_control(insn, i * 4u, entry_phys, &dummy) != CTRL_NONE) { aborted = 1; break; }
            if (emit_one(C, insn) != EMIT_OK) { aborted = 1; break; }
            if (pending == PEND_RET) {
                sljit_emit_return_void(C);
            } else if (pending == PEND_BR) {
                struct sljit_jump *j = pend_cond
                    ? sljit_emit_cmp(C, SLJIT_NOT_EQUAL, SLJIT_S1, 0, SLJIT_IMM, 0)
                    : sljit_emit_jump(C, SLJIT_JUMP);
                if (njmp >= (int)SLJIT_MAX_FRAG_CTRL) { aborted = 1; break; }
                jmps[njmp].j = j; jmps[njmp].tw = pend_tw; njmp++;
            } else { /* PEND_CALL: psx_sljit_call(cpu, target, return_pc, check) */
                sljit_emit_op1(C, SLJIT_MOV, SLJIT_R0, 0, R_CPU, 0);           /* cpu */
                if (pend_call_dynamic)
                    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_S1, 0);  /* jalr target */
                else
                    sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)pend_call_target);
                sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R2, 0, SLJIT_IMM, (sljit_sw)pend_call_return);
                sljit_emit_op1(C, SLJIT_MOV32, SLJIT_R3, 0, SLJIT_IMM, (sljit_sw)pend_call_check);
                sljit_emit_icall(C, SLJIT_CALL, SLJIT_ARGS4(32, P, 32, 32, 32), SLJIT_S2, 0);
                /* helper returned nonzero ⇒ transfer/bail in progress: return now,
                 * propagating cpu->pc / g_psx_call_bail to the dispatch loop. */
                struct sljit_jump *cont =
                    sljit_emit_cmp(C, SLJIT_EQUAL, SLJIT_R0, 0, SLJIT_IMM, 0);
                sljit_emit_return_void(C);
                sljit_set_label(cont, sljit_emit_label(C));
            }
            pending = PEND_NONE;
        }
    }
    if (!aborted && pending != PEND_NONE) aborted = 1;  /* control w/o delay slot */
    if (aborted) { sljit_free_compiler(C); s_declines++; return; }

    /* Bind each branch jump to its target label. */
    for (int k = 0; k < njmp; k++) {
        if (jmps[k].tw >= frag_words || !labels[jmps[k].tw]) {
            sljit_free_compiler(C); s_declines++; return;   /* defensive */
        }
        sljit_set_label(jmps[k].j, labels[jmps[k].tw]);
    }

    void *code = sljit_generate_code(C, 0, NULL);
    sljit_uw csz = sljit_get_generated_code_size(C);
    sljit_free_compiler(C);
    if (!code) { s_declines++; sljit_log("compile: generate_code failed @0x%08X", entry); return; }

    out->fn       = (OverlaySljitFn)code;
    out->code_lo  = entry_phys;
    out->code_len = frag_words * 4u;
    out->insns    = frag_words;
    s_compiles++;
    s_bytes += (uint64_t)csz;
    sljit_log("compile ok @0x%08X: %u insns (%d br, %d call) -> %lu bytes host",
              entry, frag_words, nbr, ncalls, (unsigned long)csz);
}

void overlay_sljit_get_status(int *available, int *selftest_ok,
                              uint64_t *compiles, uint64_t *declines,
                              uint64_t *bytes_emitted) {
    if (available)     *available     = overlay_sljit_available();
    if (selftest_ok)   *selftest_ok   = s_selftest_ok;
    if (compiles)      *compiles      = s_compiles;
    if (declines)      *declines      = s_declines;
    if (bytes_emitted) *bytes_emitted = s_bytes;
}
