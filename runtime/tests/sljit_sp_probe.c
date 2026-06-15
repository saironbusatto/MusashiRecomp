/* sljit_sp_probe.c — reproduce the live $sp-off-by-0x18 divergence at 0x12E478.
 * Stresses the register allocator with patterns that the 66-case harness doesn't:
 * register pressure forcing eviction of $sp, $sp dirtied across a call (reset),
 * and stack adjust split by branches. Standalone like sljit_emit_test.c. */
#include "cpu_state.h"
#include "overlay_sljit.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RAM_MASK 0x1FFFFFu
static uint8_t g_ram[RAM_MASK + 1];
static uint32_t rd_word(uint32_t a){ uint32_t v; memcpy(&v,&g_ram[a&(RAM_MASK&~3u)],4); return v; }
static void     wr_word(uint32_t a,uint32_t v){ memcpy(&g_ram[a&(RAM_MASK&~3u)],&v,4); }
static uint16_t rd_half(uint32_t a){ uint16_t v; memcpy(&v,&g_ram[a&(RAM_MASK&~1u)],2); return v; }
static void     wr_half(uint32_t a,uint16_t v){ memcpy(&g_ram[a&(RAM_MASK&~1u)],&v,2); }
static uint8_t  rd_byte(uint32_t a){ return g_ram[a&RAM_MASK]; }
static void     wr_byte(uint32_t a,uint8_t v){ g_ram[a&RAM_MASK]=v; }
static void cpu_init(CPUState*c){ memset(c,0,sizeof*c);
    c->read_word=rd_word;c->write_word=wr_word;c->read_half=rd_half;c->write_half=wr_half;
    c->read_byte=rd_byte;c->write_byte=wr_byte; }

/* call stub: a balanced callee (does not touch sp), returns normally. */
int psx_sljit_call(CPUState*cpu,uint32_t target,uint32_t return_pc,int check){
    (void)target;(void)return_pc;(void)check; cpu->pc=0; return 0; }
void psx_sljit_cop2(CPUState*cpu,uint32_t insn){(void)cpu;(void)insn;}
void psx_sljit_memx(CPUState*cpu,uint32_t insn){(void)cpu;(void)insn;}

#define V0 2
#define A0 4
#define T0 8
#define T1 9
#define T2 10
#define T3 11
#define T4 12
#define T5 13
#define T6 14
#define T7 15
#define S0 16
#define SP 29
#define RA 31
static uint32_t R(uint32_t op,uint32_t rs,uint32_t rt,uint32_t rd,uint32_t sh,uint32_t fn){
    return (op<<26)|(rs<<21)|(rt<<16)|(rd<<11)|(sh<<6)|fn; }
static uint32_t I(uint32_t op,uint32_t rs,uint32_t rt,uint32_t imm){
    return (op<<26)|(rs<<21)|(rt<<16)|(imm&0xFFFFu); }
static uint32_t Jal(uint32_t t){ return (3u<<26)|((t>>2)&0x03FFFFFFu); }
#define JR_RA R(0,RA,0,0,0,0x08)
#define NOP 0u
static const uint32_t BASE=0x80010000u;
static OverlaySljitFn jit(uint32_t*w,int n){ OverlaySljitResult r={0};
    overlay_sljit_try_compile(BASE,(const uint8_t*)w,(uint32_t)(n*4),BASE,&r); return r.fn; }
static int g_pass=0,g_fail=0;
#define CHECK(c,...) do{ if(c)g_pass++; else{g_fail++;printf("FAIL: ");printf(__VA_ARGS__);printf("\n");} }while(0)

int main(void){
    CPUState cpu;

    /* P1: register-pressure eviction of $sp, then $sp restored.
     *   addiu sp,sp,-0x18 ; load 8 temps (forces sp eviction) ; addiu sp,sp,0x18 ; jr ra
     * Expect: sp == entry sp (balanced). */
    { uint32_t w[]={
        I(9,SP,SP,(uint16_t)-0x18),
        I(9,0,T0,1),I(9,0,T1,2),I(9,0,T2,3),I(9,0,T3,4),
        I(9,0,T4,5),I(9,0,T5,6),I(9,0,T6,7),I(9,0,T7,8),
        I(9,SP,SP,0x18),
        JR_RA,NOP };
      OverlaySljitFn fn=jit(w,sizeof(w)/4); CHECK(fn,"P1 declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[SP]=0x801FE358; fn(&cpu);
        CHECK(cpu.gpr[SP]==0x801FE358,"P1 sp=0x%08X want 0x801FE358 (delta %d)",cpu.gpr[SP],(int)(cpu.gpr[SP]-0x801FE358)); } }

    /* P2: $sp dirtied, then a jal (flush+reset), then $sp adjusted post-call.
     *   addiu sp,sp,-0x18 ; sw ra,0x14(sp) ; jal foo ; nop ; lw ra,0x14(sp) ; addiu sp,sp,0x18 ; jr ra ; nop */
    { uint32_t w[]={
        I(9,SP,SP,(uint16_t)-0x18),
        I(0x2B,SP,RA,0x14),
        Jal(0x80012340u), NOP,
        I(0x23,SP,RA,0x14),
        I(9,SP,SP,0x18),
        JR_RA,NOP };
      OverlaySljitFn fn=jit(w,sizeof(w)/4); CHECK(fn,"P2 declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[SP]=0x801FE358; cpu.gpr[RA]=0xDEAD; fn(&cpu);
        CHECK(cpu.gpr[SP]==0x801FE358,"P2 sp=0x%08X want 0x801FE358 (delta %d)",cpu.gpr[SP],(int)(cpu.gpr[SP]-0x801FE358)); } }

    /* P3: $sp adjust on one side of a conditional branch (fall-through keeps cache).
     *   addiu sp,sp,-0x18 ; beq a0,zero,L ; nop ; (not taken) addiu t0,zero,1 ; L: addiu sp,sp,0x18 ; jr ra ; nop */
    { uint32_t w[]={
        I(9,SP,SP,(uint16_t)-0x18),
        I(4,A0,0,2), NOP,                 /* beq a0,zero,+2 -> L (word index 4) */
        I(9,0,T0,1),
        I(9,SP,SP,0x18),                  /* L */
        JR_RA,NOP };
      OverlaySljitFn fn=jit(w,sizeof(w)/4); CHECK(fn,"P3 declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[SP]=0x801FE358; cpu.gpr[A0]=0; fn(&cpu);  /* taken */
        CHECK(cpu.gpr[SP]==0x801FE358,"P3 taken sp=0x%08X delta %d",cpu.gpr[SP],(int)(cpu.gpr[SP]-0x801FE358));
        cpu_init(&cpu); cpu.gpr[SP]=0x801FE358; cpu.gpr[A0]=1; fn(&cpu);  /* not taken */
        CHECK(cpu.gpr[SP]==0x801FE358,"P3 nottaken sp=0x%08X delta %d",cpu.gpr[SP],(int)(cpu.gpr[SP]-0x801FE358)); } }

    /* P4: heavy pressure + call + sp, many regs live across the call. */
    { uint32_t w[]={
        I(9,SP,SP,(uint16_t)-0x18),
        I(9,0,S0,0x11),I(9,0,T0,0x22),I(9,0,T1,0x33),I(9,0,T2,0x44),
        I(9,0,T3,0x55),I(9,0,T4,0x66),I(9,0,T5,0x77),
        Jal(0x80012340u), NOP,
        R(0,S0,T0,V0,0,0x21),             /* addu v0,s0,t0 (uses regs live across call) */
        I(9,SP,SP,0x18),
        JR_RA,NOP };
      OverlaySljitFn fn=jit(w,sizeof(w)/4); CHECK(fn,"P4 declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[SP]=0x801FE358; fn(&cpu);
        CHECK(cpu.gpr[SP]==0x801FE358,"P4 sp=0x%08X delta %d",cpu.gpr[SP],(int)(cpu.gpr[SP]-0x801FE358)); } }

    /* P5: epilogue IN the jr $ra delay slot (the most common idiom).
     *   addiu sp,sp,-0x18 ; <work> ; jr ra ; addiu sp,sp,0x18 (delay) */
    { uint32_t w[]={
        I(9,SP,SP,(uint16_t)-0x18),
        I(9,0,T0,1),I(9,0,T1,2),
        JR_RA, I(9,SP,SP,0x18) };
      OverlaySljitFn fn=jit(w,sizeof(w)/4); CHECK(fn,"P5 declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[SP]=0x801FE358; fn(&cpu);
        CHECK(cpu.gpr[SP]==0x801FE358,"P5 sp=0x%08X delta %d",cpu.gpr[SP],(int)(cpu.gpr[SP]-0x801FE358)); } }

    /* P6: epilogue in delay slot + register pressure (force sp eviction first). */
    { uint32_t w[]={
        I(9,SP,SP,(uint16_t)-0x18),
        I(9,0,T0,1),I(9,0,T1,2),I(9,0,T2,3),I(9,0,T3,4),
        I(9,0,T4,5),I(9,0,T5,6),I(9,0,T6,7),I(9,0,T7,8),
        JR_RA, I(9,SP,SP,0x18) };
      OverlaySljitFn fn=jit(w,sizeof(w)/4); CHECK(fn,"P6 declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[SP]=0x801FE358; fn(&cpu);
        CHECK(cpu.gpr[SP]==0x801FE358,"P6 sp=0x%08X delta %d",cpu.gpr[SP],(int)(cpu.gpr[SP]-0x801FE358)); } }

    /* P7: branch DELAY SLOT adjusts sp (executes regardless of branch outcome).
     *   addiu sp,sp,-0x18 ; beq a0,zero,L ; addiu sp,sp,0x18 (delay!) ; nop ; L: jr ra ; nop */
    { uint32_t w[]={
        I(9,SP,SP,(uint16_t)-0x18),
        I(4,A0,0,2),                      /* beq a0,zero,->word4 (L) */
        I(9,SP,SP,0x18),                  /* delay slot: sp += 0x18 */
        NOP,
        JR_RA,NOP };                      /* L (word4) */
      OverlaySljitFn fn=jit(w,sizeof(w)/4); CHECK(fn,"P7 declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[SP]=0x801FE358; cpu.gpr[A0]=0; fn(&cpu);   /* taken */
        CHECK(cpu.gpr[SP]==0x801FE358,"P7 taken sp=0x%08X delta %d",cpu.gpr[SP],(int)(cpu.gpr[SP]-0x801FE358));
        cpu_init(&cpu); cpu.gpr[SP]=0x801FE358; cpu.gpr[A0]=1; fn(&cpu);   /* not taken */
        CHECK(cpu.gpr[SP]==0x801FE358,"P7 nottaken sp=0x%08X delta %d",cpu.gpr[SP],(int)(cpu.gpr[SP]-0x801FE358)); } }

    /* P8: register-register sp adjust:  addu sp, sp, t0  (t0 = -0x18 then +0x18). */
    { uint32_t w[]={
        I(9,0,T0,(uint16_t)-0x18),
        R(0,SP,T0,SP,0,0x21),             /* addu sp,sp,t0  (sp -= 0x18) */
        I(9,0,T0,0x18),
        R(0,SP,T0,SP,0,0x21),             /* addu sp,sp,t0  (sp += 0x18) */
        JR_RA,NOP };
      OverlaySljitFn fn=jit(w,sizeof(w)/4); CHECK(fn,"P8 declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[SP]=0x801FE358; fn(&cpu);
        CHECK(cpu.gpr[SP]==0x801FE358,"P8 sp=0x%08X delta %d",cpu.gpr[SP],(int)(cpu.gpr[SP]-0x801FE358)); } }

    /* P9: two calls with sp live across both + epilogue in delay slot. */
    { uint32_t w[]={
        I(9,SP,SP,(uint16_t)-0x18),
        I(0x2B,SP,RA,0x14),
        Jal(0x80012340u),NOP,
        Jal(0x80012380u),NOP,
        I(0x23,SP,RA,0x14),
        JR_RA, I(9,SP,SP,0x18) };
      OverlaySljitFn fn=jit(w,sizeof(w)/4); CHECK(fn,"P9 declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[SP]=0x801FE358; cpu.gpr[RA]=0xBEEF; fn(&cpu);
        CHECK(cpu.gpr[SP]==0x801FE358,"P9 sp=0x%08X delta %d",cpu.gpr[SP],(int)(cpu.gpr[SP]-0x801FE358)); } }

    /* P10: loop that touches sp-relative loads each iteration (backward branch),
     * epilogue after. addiu sp,sp,-0x18 ; li t0,3 ; L: lw t1,0(sp) ; addiu t0,t0,-1 ;
     * bne t0,zero,L ; nop ; addiu sp,sp,0x18 ; jr ra ; nop */
    { uint32_t w[]={
        I(9,SP,SP,(uint16_t)-0x18),
        I(9,0,T0,3),
        I(0x23,SP,T1,0),                  /* L (word2): lw t1,0(sp) */
        I(9,T0,T0,(uint16_t)-1),
        I(5,T0,0,(uint16_t)-3),           /* bne t0,zero,-3 -> word2 */
        NOP,
        I(9,SP,SP,0x18),
        JR_RA,NOP };
      OverlaySljitFn fn=jit(w,sizeof(w)/4); CHECK(fn,"P10 declined");
      if(fn){ cpu_init(&cpu); cpu.gpr[SP]=0x801FE358; fn(&cpu);
        CHECK(cpu.gpr[SP]==0x801FE358,"P10 sp=0x%08X delta %d",cpu.gpr[SP],(int)(cpu.gpr[SP]-0x801FE358)); } }

    printf("\nsljit_sp_probe: %d passed, %d failed\n",g_pass,g_fail);
    return g_fail?1:0;
}
