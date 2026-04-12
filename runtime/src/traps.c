/* traps.c — Phase 2 trap handlers.
 *
 * All traps abort with a diagnostic message.
 * Phase 2 expects MMIO abort before any trap fires.
 */

#include "cpu_state.h"
#include <stdio.h>
#include <stdlib.h>

void psx_syscall(CPUState* cpu, uint32_t code) {
    /*
     * PS1 BIOS SYSCALL convention:
     *   $a0 = 1: EnterCriticalSection — disable interrupts, return old SR
     *   $a0 = 2: ExitCriticalSection  — enable interrupts, return old SR
     *   $a0 = 3: ReturnFromException  — RFE + return to EPC
     *
     * These are the only three SYSCALLs the PS1 BIOS defines. We handle
     * them directly because the BIOS calls EnterCriticalSection during
     * early init before its exception chain infrastructure is ready.
     * Once chains are set up, the recompiled BIOS exception handler
     * could handle these too, but direct handling is correct for all phases.
     */
    uint32_t func = cpu->gpr[4]; /* $a0 = syscall function number */
    uint32_t sr = cpu->cop0[12];

    switch (func) {
        case 1: /* EnterCriticalSection: disable interrupts */
            cpu->cop0[12] = sr & ~0x0404u; /* clear IEc and IEp */
            cpu->gpr[2] = (sr & 0x0404u) ? 1 : 0; /* return whether interrupts were enabled */
            cpu->pc = 0; /* don't tail-call — return to caller */
            return;

        case 2: /* ExitCriticalSection: enable interrupts */
            cpu->cop0[12] = sr | 0x0404u; /* set IEc and IEp */
            cpu->gpr[2] = 0;
            cpu->pc = 0; /* don't tail-call — return to caller */
            return;

        case 3: /* ReturnFromException: RFE + return to EPC */
            /* Pop SR exception stack */
            cpu->cop0[12] = (sr & ~0x0Fu) | ((sr >> 2) & 0x0Fu);
            /* Dispatch to EPC */
            cpu->pc = cpu->cop0[14];
            return;

        default:
            break;
    }

    /* For unknown syscall numbers, fall through to full exception dispatch.
     * This path is taken once the BIOS has set up its exception chains. */

    /* EPC = PC of the syscall instruction (set by generated code before call) */
    cpu->cop0[14] = cpu->pc;  /* EPC */

    /* Cause: preserve existing bits, set ExcCode = 0x08 (syscall) */
    cpu->cop0[13] = (cpu->cop0[13] & ~0x7Cu) | (0x08u << 2);

    /* SR interrupt stack push: bits [5:0] shift left by 2 */
    cpu->cop0[12] = (sr & ~0x3Fu) | ((sr & 0x0Fu) << 2);

    /* Jump to exception vector. */
    uint32_t vector = (sr & 0x00400000u) ? 0xBFC00180u : 0x80000080u;
    psx_dispatch(cpu, vector);
}

static void trap_crash(const char* msg) {
    FILE* cf = fopen("psx_crash.txt", "w");
    if (cf) { fprintf(cf, "%s\n", msg); fclose(cf); }
}

void psx_break(CPUState* cpu, uint32_t code, uint32_t pc) {
    char buf[128];
    snprintf(buf, sizeof(buf), "BREAK @ PC=0x%08X, code=0x%05X", pc, code);
    trap_crash(buf);
    fprintf(stderr, "%s\n", buf); fflush(stderr);
    exit(1);
}

void psx_arith_overflow(CPUState* cpu) {
    char buf[128];
    snprintf(buf, sizeof(buf), "OVERFLOW @ PC=0x%08X", cpu->pc);
    trap_crash(buf);
    fprintf(stderr, "%s\n", buf); fflush(stderr);
    exit(1);
}

void psx_unaligned_access(CPUState* cpu, uint32_t addr, uint32_t pc) {
    char buf[128];
    snprintf(buf, sizeof(buf), "UNALIGNED @ addr=0x%08X, PC=0x%08X", addr, pc);
    trap_crash(buf);
    fprintf(stderr, "%s\n", buf); fflush(stderr);
    exit(1);
}

void psx_unknown_dispatch(CPUState* cpu, uint32_t addr, uint32_t phys) {
    /*
     * The BIOS writes small jump trampolines into RAM at runtime
     * (e.g., at 0xA0, 0xB0, 0xC0 for the A0/B0/C0 vectors).
     * Pattern: lui rN, hi / addiu rN, rN, lo / jr rN / nop
     * Since we can't execute RAM instructions, resolve the pattern
     * and re-dispatch to the computed target.
     */
    {
        uint32_t w0 = cpu->read_word(addr);
        uint32_t w1 = cpu->read_word(addr + 4);
        uint32_t w2 = cpu->read_word(addr + 8);
        uint32_t op0 = (w0 >> 26) & 0x3F;
        uint32_t op1 = (w1 >> 26) & 0x3F;

        /* Trampoline resolution: set cpu->pc so the dispatch loop re-dispatches.
         * This avoids growing the native stack for RAM trampoline chains. */

        /* Pattern 1: J target */
        if (op0 == 2) {
            uint32_t target = (addr & 0xF0000000u) | ((w0 & 0x03FFFFFFu) << 2);
            cpu->pc = target;
            return;
        }

        /* Pattern 2: JR rs (single instruction) */
        if (op0 == 0 && (w0 & 0x3F) == 0x08) {
            uint32_t rs = (w0 >> 21) & 0x1F;
            cpu->pc = cpu->gpr[rs];
            return;
        }

        /* Pattern 3: addiu rN, $zero, imm / jr rN (small address trampoline) */
        if (op0 == 0x09) { /* ADDIU */
            uint32_t rs0 = (w0 >> 21) & 0x1F;
            uint32_t rt0 = (w0 >> 16) & 0x1F;
            if (rs0 == 0) { /* addiu rN, $zero, imm = li rN, imm */
                int16_t imm = (int16_t)(w0 & 0xFFFF);
                uint32_t target = (uint32_t)(int32_t)imm;
                /* w1 should be jr rN */
                if ((w1 & 0xFC1FFFFF) == 0x00000008) { /* jr rs */
                    uint32_t jr_rs = (w1 >> 21) & 0x1F;
                    if (jr_rs == rt0) {
                        /* w2 is delay slot — execute it as load of $t1 */
                        uint32_t ds_op = (w2 >> 26) & 0x3F;
                        if (ds_op == 0x09) { /* ADDIU in delay slot */
                            uint32_t ds_rs = (w2 >> 21) & 0x1F;
                            uint32_t ds_rt = (w2 >> 16) & 0x1F;
                            int16_t ds_imm = (int16_t)(w2 & 0xFFFF);
                            if (ds_rs == 0) {
                                cpu->gpr[ds_rt] = (uint32_t)(int32_t)ds_imm;
                            }
                        }
                        cpu->pc = target;
                        return;
                    }
                }
            }
        }

        /* Pattern 4: lui rN, hi / addiu rN, rN, lo / jr rN */
        if (op0 == 0x0F) { /* LUI */
            uint32_t rt0 = (w0 >> 16) & 0x1F;
            uint32_t hi_val = (w0 & 0xFFFF) << 16;

            if (op1 == 0x09) { /* ADDIU */
                uint32_t rs1 = (w1 >> 21) & 0x1F;
                uint32_t rt1 = (w1 >> 16) & 0x1F;
                if (rs1 == rt0 && rt1 == rt0) {
                    int16_t lo_val = (int16_t)(w1 & 0xFFFF);
                    uint32_t target = hi_val + (uint32_t)(int32_t)lo_val;
                    cpu->pc = target;
                    return;
                }
            }
            if (op1 == 0x0D) { /* ORI */
                uint32_t rs1 = (w1 >> 21) & 0x1F;
                uint32_t rt1 = (w1 >> 16) & 0x1F;
                if (rs1 == rt0 && rt1 == rt0) {
                    uint32_t target = hi_val | (w1 & 0xFFFF);
                    cpu->pc = target;
                    return;
                }
            }
        }
    }

    {
        /* Log dispatch miss to file and continue (collect all misses). */
        static FILE* miss_log = NULL;
        static int miss_count = 0;
        if (!miss_log) miss_log = fopen("psx_dispatch_misses.txt", "w");
        if (miss_log) {
            fprintf(miss_log, "0x%08X phys=0x%08X\n", addr, phys);
            fflush(miss_log);
        }
        miss_count++;
        if (miss_count > 200) {
            fprintf(stderr, "DISPATCH MISS limit reached (%d misses). See psx_dispatch_misses.txt\n", miss_count);
            fflush(stderr);
            if (miss_log) fclose(miss_log);
            exit(1);
        }
        /* Return without executing — function is a no-op. */
        cpu->pc = 0;
    }
}
