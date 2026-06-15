/* sljit_emit_test.c — standalone parity test for the MIPS->sljit leaf emitter
 * (SLJIT.md §7 step 4, first slice). Builds tiny leaf functions as MIPS words,
 * JITs them via overlay_sljit_try_compile, runs the produced host code against a
 * fake CPUState + RAM, and checks results against hand-computed semantics — plus
 * the decline path (anything outside the slice returns fn==NULL).
 *
 * Self-contained: overlay_sljit.c depends only on sljit + the CPUState struct.
 *   gcc -O2 -I runtime/include -I lib/sljit/sljit_src \
 *       runtime/tests/sljit_emit_test.c runtime/src/overlay_sljit.c \
 *       lib/sljit/sljit_src/sljitLir.c -o /tmp/sljit_emit_test && /tmp/sljit_emit_test
 */
#include "cpu_state.h"
#include "overlay_sljit.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- fake guest RAM + the cpu read/write callbacks --------------------- */
#define RAM_MASK 0x1FFFFu
static uint8_t g_ram[RAM_MASK + 1];

static uint32_t rd_word(uint32_t a){ uint32_t v; memcpy(&v, &g_ram[a & (RAM_MASK & ~3u)], 4); return v; }
static void     wr_word(uint32_t a, uint32_t v){ memcpy(&g_ram[a & (RAM_MASK & ~3u)], &v, 4); }
static uint16_t rd_half(uint32_t a){ uint16_t v; memcpy(&v, &g_ram[a & (RAM_MASK & ~1u)], 2); return v; }
static void     wr_half(uint32_t a, uint16_t v){ memcpy(&g_ram[a & (RAM_MASK & ~1u)], &v, 2); }
static uint8_t  rd_byte(uint32_t a){ return g_ram[a & RAM_MASK]; }
static void     wr_byte(uint32_t a, uint8_t v){ g_ram[a & RAM_MASK] = v; }

static void cpu_init(CPUState *c){
    memset(c, 0, sizeof *c);
    c->read_word = rd_word;  c->write_word = wr_word;
    c->read_half = rd_half;  c->write_half = wr_half;
    c->read_byte = rd_byte;  c->write_byte = wr_byte;
}

/* ---- MIPS encoders ----------------------------------------------------- */
#define ZERO 0
#define V0 2
#define V1 3
#define A0 4
#define A1 5
#define T0 8
#define RA 31
static uint32_t R(uint32_t op,uint32_t rs,uint32_t rt,uint32_t rd,uint32_t sh,uint32_t fn){
    return (op<<26)|(rs<<21)|(rt<<16)|(rd<<11)|(sh<<6)|fn;
}
static uint32_t I(uint32_t op,uint32_t rs,uint32_t rt,uint32_t imm){
    return (op<<26)|(rs<<21)|(rt<<16)|(imm & 0xFFFFu);
}
#define JR_RA   R(0,RA,0,0,0,0x08)
#define NOP     0u

/* ---- harness ----------------------------------------------------------- */
static int g_pass=0, g_fail=0;
#define CHECK(cond, ...) do{ if(cond){g_pass++;} else {g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } }while(0)

static const uint32_t BASE = 0x80010000u;  /* pretend vram of the fragment */

static OverlaySljitFn jit(uint32_t *w, int n){
    OverlaySljitResult r = {0};
    overlay_sljit_try_compile(BASE, (const uint8_t*)w, (uint32_t)(n*4), BASE, &r);
    return r.fn;
}

int main(void){
    CPUState cpu;

    /* 1. ADDIU v0, a0, 5 */
    { uint32_t w[]={ I(9,A0,V0,5), JR_RA, NOP };
      OverlaySljitFn fn=jit(w,3); CHECK(fn,"addiu: declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[A0]=100; fn(&cpu);
        CHECK(cpu.gpr[V0]==105,"addiu: v0=%u",cpu.gpr[V0]); } }

    /* 2. SLL v0, a0, 3 */
    { uint32_t w[]={ R(0,0,A0,V0,3,0x00), JR_RA, NOP };
      OverlaySljitFn fn=jit(w,3); CHECK(fn,"sll: declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[A0]=0x11; fn(&cpu);
        CHECK(cpu.gpr[V0]==(0x11u<<3),"sll: v0=0x%X",cpu.gpr[V0]); } }

    /* 3. LW v0, 8(a0) */
    { uint32_t w[]={ I(0x23,A0,V0,8), JR_RA, NOP };
      OverlaySljitFn fn=jit(w,3); CHECK(fn,"lw: declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[A0]=0x1000; wr_word(0x1008,0xDEADBEEF); fn(&cpu);
        CHECK(cpu.gpr[V0]==0xDEADBEEF,"lw: v0=0x%X",cpu.gpr[V0]); } }

    /* 4. SW a1, 12(a0) */
    { uint32_t w[]={ I(0x2B,A0,A1,12), JR_RA, NOP };
      OverlaySljitFn fn=jit(w,3); CHECK(fn,"sw: declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[A0]=0x1000; cpu.gpr[A1]=0x12345678; fn(&cpu);
        CHECK(rd_word(0x100C)==0x12345678,"sw: mem=0x%X",rd_word(0x100C)); } }

    /* 5a. SLT v0, a0, a1 (signed: -5 < 3 => 1) */
    { uint32_t w[]={ R(0,A0,A1,V0,0,0x2A), JR_RA, NOP };
      OverlaySljitFn fn=jit(w,3); CHECK(fn,"slt: declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[A0]=(uint32_t)-5; cpu.gpr[A1]=3; fn(&cpu);
        CHECK(cpu.gpr[V0]==1,"slt signed: v0=%u",cpu.gpr[V0]); } }
    /* 5b. SLTU v0, a0, a1 (unsigned: 0xFFFFFFFB > 3 => 0) */
    { uint32_t w[]={ R(0,A0,A1,V0,0,0x2B), JR_RA, NOP };
      OverlaySljitFn fn=jit(w,3); CHECK(fn,"sltu: declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[A0]=(uint32_t)-5; cpu.gpr[A1]=3; fn(&cpu);
        CHECK(cpu.gpr[V0]==0,"sltu: v0=%u",cpu.gpr[V0]); } }

    /* 6a. LB sign-extends 0x80 -> 0xFFFFFF80 */
    { uint32_t w[]={ I(0x20,A0,V0,0), JR_RA, NOP };
      OverlaySljitFn fn=jit(w,3); CHECK(fn,"lb: declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[A0]=0x2000; wr_byte(0x2000,0x80); fn(&cpu);
        CHECK(cpu.gpr[V0]==0xFFFFFF80u,"lb: v0=0x%X",cpu.gpr[V0]); } }
    /* 6b. LBU zero-extends 0x80 -> 0x80 */
    { uint32_t w[]={ I(0x24,A0,V0,0), JR_RA, NOP };
      OverlaySljitFn fn=jit(w,3); CHECK(fn,"lbu: declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[A0]=0x2000; wr_byte(0x2000,0x80); fn(&cpu);
        CHECK(cpu.gpr[V0]==0x80u,"lbu: v0=0x%X",cpu.gpr[V0]); } }

    /* 7. gpr[0] invariant: ADDIU zero,a0,5 must NOT write; OR v0,zero,a0 reads 0 */
    { uint32_t w[]={ I(9,A0,ZERO,5), R(0,ZERO,A0,V0,0,0x25), JR_RA, NOP };
      OverlaySljitFn fn=jit(w,4); CHECK(fn,"zero-inv: declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[A0]=0xABCD; fn(&cpu);
        CHECK(cpu.gpr[ZERO]==0,"zero-inv: gpr0=%u",cpu.gpr[ZERO]);
        CHECK(cpu.gpr[V0]==0xABCD,"zero-inv: v0=0x%X (or zero,a0)",cpu.gpr[V0]); } }

    /* 8. multi-op leaf: ADDU t0,a0,a1 ; SLL v0,t0,2 */
    { uint32_t w[]={ R(0,A0,A1,T0,0,0x21), R(0,0,T0,V0,2,0x00), JR_RA, NOP };
      OverlaySljitFn fn=jit(w,4); CHECK(fn,"multi: declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[A0]=10; cpu.gpr[A1]=7; fn(&cpu);
        CHECK(cpu.gpr[V0]==((10u+7u)<<2),"multi: v0=%u",cpu.gpr[V0]); } }

    /* 9. DECLINE cases — must return fn==NULL (fall to interpreter) */
    { uint32_t w[]={ I(4,A0,A1,2), NOP, JR_RA, NOP };     /* BEQ */
      CHECK(jit(w,4)==NULL,"decline: BEQ accepted"); }
    { uint32_t w[]={ I(3,0,0,0x100), NOP, JR_RA, NOP };   /* JAL */
      CHECK(jit(w,4)==NULL,"decline: JAL accepted"); }
    { uint32_t w[]={ R(0,V0,0,0,0,0x08), NOP };           /* JR v0 (non-ra) */
      CHECK(jit(w,2)==NULL,"decline: JR non-ra accepted"); }
    { uint32_t w[]={ I(4,A0,A1,1), JR_RA, NOP };          /* control in delay slot */
      uint32_t w2[]={ JR_RA, I(4,A0,A1,1) };
      CHECK(jit(w2,2)==NULL,"decline: branch in jr delay slot accepted"); (void)w; }

    /* 10. MULT (signed) + MFHI/MFLO: -3 * 5 = -15 => hi=0xFFFFFFFF lo=0xFFFFFFF1 */
    { uint32_t w[]={ R(0,A0,A1,0,0,0x18), R(0,0,0,V0,0,0x10), R(0,0,0,V1,0,0x12), JR_RA, NOP };
      OverlaySljitFn fn=jit(w,5); CHECK(fn,"mult: declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[A0]=(uint32_t)-3; cpu.gpr[A1]=5; fn(&cpu);
        CHECK(cpu.gpr[V0]==0xFFFFFFFFu,"mult hi: v0=0x%X",cpu.gpr[V0]);
        CHECK(cpu.gpr[V1]==0xFFFFFFF1u,"mult lo: v1=0x%X",cpu.gpr[V1]); } }

    /* 11. MULTU (unsigned): 0xFFFFFFFF * 2 = 0x1FFFFFFFE => hi=1 lo=0xFFFFFFFE */
    { uint32_t w[]={ R(0,A0,A1,0,0,0x19), R(0,0,0,V0,0,0x10), R(0,0,0,V1,0,0x12), JR_RA, NOP };
      OverlaySljitFn fn=jit(w,5); CHECK(fn,"multu: declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[A0]=0xFFFFFFFFu; cpu.gpr[A1]=2; fn(&cpu);
        CHECK(cpu.gpr[V0]==1u,"multu hi: v0=0x%X",cpu.gpr[V0]);
        CHECK(cpu.gpr[V1]==0xFFFFFFFEu,"multu lo: v1=0x%X",cpu.gpr[V1]); } }

    /* 12. Loop (backward branch): v0 = sum(1..a0)
     *   0 addiu v0,zero,0 ; 1 addiu t0,a0,0
     *   2 blez t0,end(+5) ; 3 nop
     *   4 addu v0,v0,t0   ; 5 addiu t0,t0,-1
     *   6 beq zero,zero,loop(-5) ; 7 nop ; 8 jr ra ; 9 nop                  */
    { uint32_t w[]={ I(9,ZERO,V0,0), I(9,A0,T0,0),
                     I(6,T0,0,5), NOP,
                     R(0,V0,T0,V0,0,0x21), I(9,T0,T0,0xFFFF),
                     I(4,ZERO,ZERO,(uint16_t)(-5)), NOP, JR_RA, NOP };
      OverlaySljitFn fn=jit(w,10); CHECK(fn,"loop: declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[A0]=5; fn(&cpu);
        CHECK(cpu.gpr[V0]==15,"loop sum(1..5): v0=%u",cpu.gpr[V0]);
        cpu_init(&cpu); cpu.gpr[A0]=0; fn(&cpu);
        CHECK(cpu.gpr[V0]==0,"loop sum(1..0): v0=%u",cpu.gpr[V0]); } }

    /* 13. Forward conditional: v0 = max(a0,a1) (signed)
     *   0 slt t0,a0,a1 ; 1 bne t0,zero,L1(+4) ; 2 nop
     *   3 addu v0,a0,zero ; 4 beq zero,zero,L2(+2) ; 5 nop
     *   6 L1: addu v0,a1,zero ; 7 L2: jr ra ; 8 nop                          */
    { uint32_t w[]={ R(0,A0,A1,T0,0,0x2A), I(5,T0,ZERO,4), NOP,
                     R(0,A0,ZERO,V0,0,0x21), I(4,ZERO,ZERO,2), NOP,
                     R(0,A1,ZERO,V0,0,0x21), JR_RA, NOP };
      OverlaySljitFn fn=jit(w,9); CHECK(fn,"max: declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[A0]=10; cpu.gpr[A1]=7; fn(&cpu);
        CHECK(cpu.gpr[V0]==10,"max(10,7): v0=%u",cpu.gpr[V0]);
        cpu_init(&cpu); cpu.gpr[A0]=3; cpu.gpr[A1]=8; fn(&cpu);
        CHECK(cpu.gpr[V0]==8,"max(3,8): v0=%u",cpu.gpr[V0]); } }

    /* 14. Still-decline: branch target landing on a delay slot (the jr's) */
    { uint32_t w[]={ I(4,A0,A1,2), NOP, JR_RA, NOP };  /* BEQ target=word3=jr ds */
      CHECK(jit(w,4)==NULL,"decline: branch into delay slot accepted"); }

    /* 15. Still-decline: JR to non-$ra */
    { uint32_t w[]={ R(0,V0,0,0,0,0x08), NOP };
      CHECK(jit(w,2)==NULL,"decline: JR non-ra accepted"); }

    /* 16. DIV (signed): div a0,a1 ; mflo v0 ; mfhi v1 ; jr ra ; nop */
    { uint32_t w[]={ R(0,A0,A1,0,0,0x1A), R(0,0,0,V0,0,0x12), R(0,0,0,V1,0,0x10), JR_RA, NOP };
      OverlaySljitFn fn=jit(w,5); CHECK(fn,"div: declined");
      if(fn){
        cpu_init(&cpu); cpu.gpr[A0]=(uint32_t)-20; cpu.gpr[A1]=3; fn(&cpu);
        CHECK(cpu.gpr[V0]==(uint32_t)-6,"div -20/3 lo: v0=0x%X",cpu.gpr[V0]);
        CHECK(cpu.gpr[V1]==(uint32_t)-2,"div -20%%3 hi: v1=0x%X",cpu.gpr[V1]);
        cpu_init(&cpu); cpu.gpr[A0]=5; cpu.gpr[A1]=0; fn(&cpu);          /* /0, a>0 */
        CHECK(cpu.gpr[V0]==0xFFFFFFFFu,"div 5/0 lo: v0=0x%X",cpu.gpr[V0]);
        CHECK(cpu.gpr[V1]==5,"div 5/0 hi: v1=%u",cpu.gpr[V1]);
        cpu_init(&cpu); cpu.gpr[A0]=(uint32_t)-5; cpu.gpr[A1]=0; fn(&cpu);/* /0, a<0 */
        CHECK(cpu.gpr[V0]==1,"div -5/0 lo: v0=0x%X",cpu.gpr[V0]);
        cpu_init(&cpu); cpu.gpr[A0]=0x80000000u; cpu.gpr[A1]=(uint32_t)-1; fn(&cpu);/* overflow */
        CHECK(cpu.gpr[V0]==0x80000000u,"div INT_MIN/-1 lo: v0=0x%X",cpu.gpr[V0]);
        CHECK(cpu.gpr[V1]==0,"div INT_MIN/-1 hi: v1=%u",cpu.gpr[V1]); } }

    /* 17. DIVU (unsigned): divu a0,a1 ; mflo v0 ; mfhi v1 */
    { uint32_t w[]={ R(0,A0,A1,0,0,0x1B), R(0,0,0,V0,0,0x12), R(0,0,0,V1,0,0x10), JR_RA, NOP };
      OverlaySljitFn fn=jit(w,5); CHECK(fn,"divu: declined");
      if(fn){
        cpu_init(&cpu); cpu.gpr[A0]=20; cpu.gpr[A1]=3; fn(&cpu);
        CHECK(cpu.gpr[V0]==6 && cpu.gpr[V1]==2,"divu 20/3: v0=%u v1=%u",cpu.gpr[V0],cpu.gpr[V1]);
        cpu_init(&cpu); cpu.gpr[A0]=20; cpu.gpr[A1]=0; fn(&cpu);          /* /0 */
        CHECK(cpu.gpr[V0]==0xFFFFFFFFu && cpu.gpr[V1]==20,"divu 20/0: v0=0x%X v1=%u",cpu.gpr[V0],cpu.gpr[V1]); } }

    /* 18. J (forward absolute): skip the v0=99 store
     *   0 addiu v0,zero,1 ; 1 j ->word4 ; 2 nop ; 3 addiu v0,zero,99 (skipped)
     *   4 jr ra ; 5 nop                                                       */
    { uint32_t tphys = (BASE & 0x1FFFFFFFu) + 4u*4u;       /* word 4 */
      uint32_t ji = (2u<<26) | ((tphys>>2) & 0x03FFFFFFu);
      uint32_t w[]={ I(9,ZERO,V0,1), ji, NOP, I(9,ZERO,V0,99), JR_RA, NOP };
      OverlaySljitFn fn=jit(w,6); CHECK(fn,"J: declined");
      if(fn){ cpu_init(&cpu); fn(&cpu);
        CHECK(cpu.gpr[V0]==1,"J skip: v0=%u (want 1)",cpu.gpr[V0]); } }

    printf("\nsljit_emit_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
