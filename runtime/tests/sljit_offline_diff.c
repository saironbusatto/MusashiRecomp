/* sljit_offline_diff.c — offline JIT-vs-reference-interpreter differential over a
 * captured overlay image. For each candidate entry: JIT the fragment with
 * overlay_sljit_try_compile, run a reference MIPS interpreter over the SAME bytes
 * with the SAME memory + the SAME call stub, and compare the full register file at
 * return. Any mismatch is a codegen bug in that function (the live $sp-off-by-0x18
 * divergence at 0x12E478 traced to a callee shard corrupting $sp; this finds it
 * without the game).
 *
 * Build:
 *   gcc -O2 -I runtime/include -I lib/sljit/sljit_src \
 *       runtime/tests/sljit_offline_diff.c runtime/src/overlay_sljit.c \
 *       lib/sljit/sljit_src/sljitLir.c -o /tmp/sljit_offline_diff && \
 *   /tmp/sljit_offline_diff <capture.json> [entry_hex ...]
 */
#include "cpu_state.h"
#include "overlay_sljit.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* decoded capture image (defined/loaded below; forward globals so ref_run can
 * read the size). */
static uint8_t* g_img=NULL; static uint32_t g_base=0,g_imgsz=0;

/* ---- fake guest RAM (2 MB) shared by JIT + reference interp --------------- */
static uint8_t g_ram[0x200000];
static uint32_t MASK(uint32_t a){ return a & 0x1FFFFF; }   /* fold KSEG + I/O into 2MB */
static uint32_t rd_word(uint32_t a){ uint32_t v; memcpy(&v,&g_ram[MASK(a)&~3u],4); return v; }
static void     wr_word(uint32_t a,uint32_t v){ memcpy(&g_ram[MASK(a)&~3u],&v,4); }
static uint16_t rd_half(uint32_t a){ uint16_t v; memcpy(&v,&g_ram[MASK(a)&~1u],2); return v; }
static void     wr_half(uint32_t a,uint16_t v){ memcpy(&g_ram[MASK(a)&~1u],&v,2); }
static uint8_t  rd_byte(uint32_t a){ return g_ram[MASK(a)]; }
static void     wr_byte(uint32_t a,uint8_t v){ g_ram[MASK(a)]=v; }
static void cpu_wire(CPUState*c){ c->read_word=rd_word;c->write_word=wr_word;
    c->read_half=rd_half;c->write_half=wr_half;c->read_byte=rd_byte;c->write_byte=wr_byte; }

/* Deterministic, balanced call stub used by BOTH sides: a callee that does not
 * touch $sp, sets $v0 to a fixed value, restores nothing (the caller's frame is
 * intact), returns normally (no bail). Identical effect on JIT and interp, so any
 * divergence isolates the JIT's own codegen — NOT call semantics. */
static int g_calls=0;
int psx_sljit_call(CPUState*cpu,uint32_t target,uint32_t return_pc,int check){
    (void)check; g_calls++; cpu->gpr[2]=0x12340000u ^ target; cpu->gpr[31]=return_pc; cpu->pc=0; return 0; }
void psx_sljit_cop2(CPUState*cpu,uint32_t insn){(void)cpu;(void)insn;}
void psx_sljit_memx(CPUState*cpu,uint32_t insn){(void)cpu;(void)insn;}

/* ---- reference MIPS interpreter (parallels dirty_ram_interp for this subset) */
static uint32_t f_op(uint32_t i){return i>>26;} static uint32_t f_rs(uint32_t i){return(i>>21)&31;}
static uint32_t f_rt(uint32_t i){return(i>>16)&31;} static uint32_t f_rd(uint32_t i){return(i>>11)&31;}
static uint32_t f_sh(uint32_t i){return(i>>6)&31;} static uint32_t f_fn(uint32_t i){return i&63;}
static uint32_t f_imm(uint32_t i){return i&0xFFFF;} static int32_t f_simm(uint32_t i){return(int32_t)(int16_t)(i&0xFFFF);}

/* execute one non-control insn on the reference cpu (mirror of emit_one's intent) */
static void ref_one(CPUState*c,uint32_t in){
    uint32_t op=f_op(in),rs=f_rs(in),rt=f_rt(in),rd=f_rd(in),sh=f_sh(in),fn=f_fn(in),imm=f_imm(in);
    int32_t simm=f_simm(in); uint32_t*g=c->gpr;
    #define W(n,v) do{ if(n)g[n]=(uint32_t)(v); }while(0)
    if(op==0){
        switch(fn){
        case 0x00: W(rd,g[rt]<<sh); break; case 0x02: W(rd,g[rt]>>sh); break;
        case 0x03: W(rd,(uint32_t)((int32_t)g[rt]>>sh)); break;
        case 0x04: W(rd,g[rt]<<(g[rs]&31)); break; case 0x06: W(rd,g[rt]>>(g[rs]&31)); break;
        case 0x07: W(rd,(uint32_t)((int32_t)g[rt]>>(g[rs]&31))); break;
        case 0x20: case 0x21: W(rd,g[rs]+g[rt]); break; case 0x22: case 0x23: W(rd,g[rs]-g[rt]); break;
        case 0x24: W(rd,g[rs]&g[rt]); break; case 0x25: W(rd,g[rs]|g[rt]); break;
        case 0x26: W(rd,g[rs]^g[rt]); break; case 0x27: W(rd,~(g[rs]|g[rt])); break;
        case 0x2a: W(rd,((int32_t)g[rs]<(int32_t)g[rt])?1:0); break;
        case 0x2b: W(rd,(g[rs]<g[rt])?1:0); break;
        case 0x10: W(rd,c->hi); break; case 0x12: W(rd,c->lo); break;
        case 0x11: c->hi=g[rs]; break; case 0x13: c->lo=g[rs]; break;
        case 0x18: { int64_t r=(int64_t)(int32_t)g[rs]*(int64_t)(int32_t)g[rt]; c->lo=(uint32_t)r; c->hi=(uint32_t)((uint64_t)r>>32);} break;
        case 0x19: { uint64_t r=(uint64_t)g[rs]*(uint64_t)g[rt]; c->lo=(uint32_t)r; c->hi=(uint32_t)(r>>32);} break;
        case 0x1a: { int32_t a=(int32_t)g[rs],b=(int32_t)g[rt]; if(b==0){c->lo=a<0?1:0xFFFFFFFF;c->hi=(uint32_t)a;} else if((uint32_t)a==0x80000000u&&b==-1){c->lo=0x80000000u;c->hi=0;} else {c->lo=(uint32_t)(a/b);c->hi=(uint32_t)(a%b);} } break;
        case 0x1b: { uint32_t a=g[rs],b=g[rt]; if(b==0){c->lo=0xFFFFFFFF;c->hi=a;} else {c->lo=a/b;c->hi=a%b;} } break;
        case 0x0f: break; /* sync */
        default: printf("  ref: unhandled special fn 0x%02X\n",fn); break;
        }
    } else switch(op){
        case 0x08: case 0x09: W(rt,g[rs]+(uint32_t)simm); break;
        case 0x0a: W(rt,((int32_t)g[rs]<simm)?1:0); break;
        case 0x0b: W(rt,(g[rs]<(uint32_t)simm)?1:0); break;
        case 0x0c: W(rt,g[rs]&imm); break; case 0x0d: W(rt,g[rs]|imm); break; case 0x0e: W(rt,g[rs]^imm); break;
        case 0x0f: W(rt,imm<<16); break;
        case 0x20: W(rt,(uint32_t)(int32_t)(int8_t)c->read_byte(g[rs]+simm)); break;
        case 0x21: W(rt,(uint32_t)(int32_t)(int16_t)c->read_half(g[rs]+simm)); break;
        case 0x23: W(rt,c->read_word(g[rs]+simm)); break;
        case 0x24: W(rt,c->read_byte(g[rs]+simm)); break;
        case 0x25: W(rt,c->read_half(g[rs]+simm)); break;
        case 0x28: c->write_byte(g[rs]+simm,(uint8_t)g[rt]); break;
        case 0x29: c->write_half(g[rs]+simm,(uint16_t)g[rt]); break;
        case 0x2b: c->write_word(g[rs]+simm,g[rt]); break;
        default: printf("  ref: unhandled op 0x%02X\n",op); break;
    }
    g[0]=0;
    #undef W
}
static int ref_is_ctrl(uint32_t in){ uint32_t op=f_op(in),fn=f_fn(in);
    if(op==0) return (fn==0x08||fn==0x09); if(op==1) return 1; return (op>=2&&op<=7); }
/* Run the reference interp from entry until jr $ra returns (matches the shard's
 * fragment semantics). bytes/base define the image. Returns at jr $ra. */
static const uint8_t*g_rimg=NULL; static uint32_t g_rbase=0,g_rsz=0;
static int in_img(uint32_t pc){ uint32_t off=(pc&0x1FFFFFFF)-(g_rbase&0x1FFFFFFF); return off+8<=g_rsz && (pc&0x1FFFFFFF)>=(g_rbase&0x1FFFFFFF); }
static uint32_t rimw(uint32_t pc){ return *(const uint32_t*)(g_rimg+((pc&0x1FFFFFFF)-(g_rbase&0x1FFFFFFF))); }
static void ref_run(CPUState*c,const uint8_t*img,uint32_t base,uint32_t entry){
    g_rimg=img; g_rbase=base; g_rsz=g_imgsz;
    uint32_t pc=entry; int guard=0;
    while(guard++<100000){
        if(!in_img(pc)){ printf("  ref: pc 0x%08X left image — stop\n",pc); return; }
        uint32_t in=rimw(pc);
        uint32_t op=f_op(in),fn=f_fn(in),rs=f_rs(in),rt=f_rt(in),rd=f_rd(in);
        if(op==0&&fn==0x08){ /* jr rs */
            uint32_t tgt=c->gpr[rs];
            uint32_t ds=rimw(pc+4); ref_one(c,ds);
            if(rs==31) return;                 /* return */
            pc=tgt; continue;                  /* jr rX tail */
        }
        if(op==0&&fn==0x09){ /* jalr rd,rs */
            uint32_t tgt=c->gpr[rs]; uint32_t link=rd?rd:31; c->gpr[link]=pc+8;
            uint32_t ds=rimw(pc+4); ref_one(c,ds);
            psx_sljit_call(c,tgt,pc+8,(rd==0||rd==31)); pc+=8; continue;
        }
        if(op==3){ /* jal */
            uint32_t tgt=((pc+4)&0xF0000000)|((in&0x3FFFFFF)<<2); c->gpr[31]=pc+8;
            uint32_t ds=rimw(pc+4); ref_one(c,ds);
            psx_sljit_call(c,tgt,pc+8,1); pc+=8; continue;
        }
        if(op==2){ /* j */
            uint32_t tgt=((pc+4)&0xF0000000)|((in&0x3FFFFFF)<<2);
            uint32_t ds=rimw(pc+4); ref_one(c,ds);
            pc=tgt; continue;
        }
        if(op==1||(op>=4&&op<=7)){ /* branch */
            int take=0; int32_t simm=f_simm(in);
            if(op==4) take=(c->gpr[rs]==c->gpr[rt]);
            else if(op==5) take=(c->gpr[rs]!=c->gpr[rt]);
            else if(op==6) take=((int32_t)c->gpr[rs]<=0);
            else if(op==7) take=((int32_t)c->gpr[rs]>0);
            else { int lz=(rt==0||rt==0x10); if(rt==0x10||rt==0x11) c->gpr[31]=pc+8;
                   take = lz?((int32_t)c->gpr[rs]<0):((int32_t)c->gpr[rs]>=0); }
            uint32_t ds=rimw(pc+4); ref_one(c,ds);
            pc = take ? (pc+4+(simm<<2)) : (pc+8); continue;
        }
        ref_one(c,in); pc+=4;
    }
    printf("  ref: guard tripped (no return)\n");
}

/* ---- capture loader (minimal JSON: find base64 image covering an addr) ----- */
static int b64dec(const char*s,uint8_t*out,int outcap){
    static int8_t T[256]; static int init=0; if(!init){memset(T,-1,256);
        const char*A="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for(int i=0;i<64;i++)T[(unsigned char)A[i]]=i; init=1;}
    int n=0,val=0,bits=0;
    for(;*s&&*s!='"';s++){ if(*s=='='||T[(unsigned char)*s]<0)continue; val=(val<<6)|T[(unsigned char)*s]; bits+=6;
        if(bits>=8){bits-=8; if(n<outcap)out[n++]=(uint8_t)(val>>bits);} }
    return n;
}
/* crude: pull load_addr/size/bytes_b64 of the FIRST capture covering target */
static int load_capture(const char*path,uint32_t target){
    FILE*f=fopen(path,"rb"); if(!f){printf("cannot open %s\n",path);return 0;}
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    char*buf=malloc(sz+1); fread(buf,1,sz,f); buf[sz]=0; fclose(f);
    char*p=buf;
    while((p=strstr(p,"\"load_addr\""))){
        char*la=strchr(p,':'); unsigned long load=strtoul(strchr(la,'"')+1,NULL,16);
        char*szp=strstr(p,"\"size\""); unsigned long size=szp?strtoul(strchr(szp,':')+1,NULL,10):0;
        char*bp=strstr(p,"\"bytes_b64\""); if(!bp)break;
        char*q=strchr(strchr(bp,':')+1,'"')+1;
        if(((load&0x1FFFFFFF)<=(target&0x1FFFFFFF))&&((target&0x1FFFFFFF)<(load&0x1FFFFFFF)+size)){
            g_img=malloc(size+16); g_imgsz=b64dec(q,g_img,size+16); g_base=(uint32_t)load;
            free(buf); printf("capture: base=0x%08X size=0x%lX decoded=0x%X\n",g_base,size,g_imgsz); return 1;
        }
        p=q;
    }
    free(buf); printf("no capture covers 0x%08X\n",target); return 0;
}
static uint32_t imgw(uint32_t addr){ return *(const uint32_t*)(g_img+((addr&0x1FFFFFFF)-(g_base&0x1FFFFFFF))); }

/* Test one entry: JIT it, run JIT + reference from identical state, compare. */
static int g_fail=0,g_ok=0,g_decl=0;
static void test_entry(uint32_t entry){
    OverlaySljitResult r={0};
    overlay_sljit_try_compile(entry,g_img,g_imgsz,g_base,&r);
    if(!r.fn){ g_decl++; return; }
    /* identical start state for both: a frame at 0x801FE000, sp below it, plus
     * pseudo-random but fixed gpr values so divergences surface. */
    CPUState cj,ci; memset(&cj,0,sizeof cj); cpu_wire(&cj);
    uint32_t sp=0x801FE358;
    for(int i=1;i<32;i++) cj.gpr[i]=0x01010101u*i + 0x100;
    cj.gpr[29]=sp; cj.gpr[31]=0x80012000u; cj.gpr[16]=0x801FC000u; /* s0 = object ptr */
    /* a plausible saved frame so epilogue loads are sane */
    wr_word(sp+0x14,0x80012000u); wr_word(sp+0x10,0x801FC000u);
    ci=cj; cpu_wire(&ci);
    int calls0=g_calls; r.fn(&cj); int jcalls=g_calls-calls0;
    calls0=g_calls; ref_run(&ci,g_img,g_base,entry); int icalls=g_calls-calls0;
    int diff=0; for(int i=1;i<32;i++) if(cj.gpr[i]!=ci.gpr[i]) diff=1;
    if(cj.hi!=ci.hi||cj.lo!=ci.lo) diff=1;
    if(diff){
        g_fail++;
        printf("DIVERGE 0x%08X (%u insns, jit_calls=%d ref_calls=%d):\n",entry,r.insns,jcalls,icalls);
        const char*rn[32]={"zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3","t4","t5","t6","t7",
            "s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1","gp","sp","fp","ra"};
        for(int i=1;i<32;i++) if(cj.gpr[i]!=ci.gpr[i])
            printf("   %-3s jit=0x%08X ref=0x%08X (delta %d)\n",rn[i],cj.gpr[i],ci.gpr[i],(int)(cj.gpr[i]-ci.gpr[i]));
        if(cj.hi!=ci.hi) printf("   hi  jit=0x%08X ref=0x%08X\n",cj.hi,ci.hi);
        if(cj.lo!=ci.lo) printf("   lo  jit=0x%08X ref=0x%08X\n",cj.lo,ci.lo);
    } else g_ok++;
}

/* DUMP mode: JIT a fragment with whichever emitter is linked, run it from a
 * fixed deterministic state + the shared balanced call stub, and print the final
 * register file on one line. Build this TU once with the new overlay_sljit.c and
 * once with the old (HEAD) one, then `diff` the outputs: any line that differs is
 * a function my register allocation changed — a definitive, reference-free bisect
 * (the old emitter is the validated baseline). */
static void dump_entry(uint32_t entry){
    OverlaySljitResult r={0};
    overlay_sljit_try_compile(entry,g_img,g_imgsz,g_base,&r);
    if(!r.fn){ printf("0x%08X DECLINED\n",entry); return; }
    fprintf(stderr,"RUN 0x%08X insns=%u\n",entry,r.insns); fflush(stderr);
    CPUState c; memset(&c,0,sizeof c); cpu_wire(&c);
    for(int i=1;i<32;i++) c.gpr[i]=0x01010101u*i + 0x100;
    c.gpr[29]=0x801FE358; c.gpr[31]=0x80012000u; c.gpr[16]=0x801FC000u;
    wr_word(0x801FE358+0x14,0x80012000u); wr_word(0x801FE358+0x10,0x801FC000u);
    int calls0=g_calls; r.fn(&c); int calls=g_calls-calls0;
    printf("0x%08X insns=%u calls=%d sp=0x%08X",entry,r.insns,calls,c.gpr[29]);
    for(int i=1;i<32;i++) printf(" %08X",c.gpr[i]);
    printf(" hi=%08X lo=%08X\n",c.hi,c.lo);
}

int main(int argc,char**argv){
    if(argc<2){ printf("usage: %s <capture.json> [--dump] [entry_hex ...]\n",argv[0]); return 2; }
    int dump=0, ai=2;
    if(argc>2 && !strcmp(argv[2],"--dump")){ dump=1; ai=3; }
    uint32_t deflt[]={0x8012E478,0x8012BC24,0x8012B424,0x8012B0A4,0x8012D2D0,0x8012C7E4,0x8012BD40,0x8012BEB0};
    int n; static uint32_t list[8192];
    if(argc>ai){ n=0; for(int i=ai;i<argc&&n<8192;i++) list[n++]=(uint32_t)strtoul(argv[i],NULL,16);
                 if(!load_capture(argv[1],list[0])) return 1; }
    else if(dump){
        /* auto-enumerate: every jal target + every jalr-reachable entry in the
         * image (broad coverage for the old-vs-new diff). */
        if(!load_capture(argv[1],deflt[0])) return 1;
        n=0;
        for(uint32_t off=0; off+4<=g_imgsz; off+=4){
            uint32_t w=*(const uint32_t*)(g_img+off);
            if((w>>26)==3){ /* jal */
                uint32_t tgt=((g_base&0xF0000000)|((w&0x3FFFFFF)<<2));
                if(((tgt&0x1FFFFFFF)>=(g_base&0x1FFFFFFF))&&((tgt&0x1FFFFFFF)<(g_base&0x1FFFFFFF)+g_imgsz)){
                    int dup=0; for(int k=0;k<n;k++) if(list[k]==tgt){dup=1;break;}
                    if(!dup&&n<8192) list[n++]=tgt;
                }
            }
        }
        /* sort ascending for stable diff */
        for(int i=0;i<n;i++)for(int j=i+1;j<n;j++) if(list[j]<list[i]){uint32_t t=list[i];list[i]=list[j];list[j]=t;}
    }
    else { n=sizeof(deflt)/4; memcpy(list,deflt,sizeof deflt); if(!load_capture(argv[1],list[0])) return 1; }

    for(int i=0;i<n;i++){
        if(((list[i]&0x1FFFFFFF) < (g_base&0x1FFFFFFF)) || ((list[i]&0x1FFFFFFF) >= (g_base&0x1FFFFFFF)+g_imgsz)) continue;
        if(dump) dump_entry(list[i]); else test_entry(list[i]);
    }
    if(!dump) printf("\noffline_diff: %d ok, %d DIVERGE, %d declined\n",g_ok,g_fail,g_decl);
    return g_fail?1:0;
}
