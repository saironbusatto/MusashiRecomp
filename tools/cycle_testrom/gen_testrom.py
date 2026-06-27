#!/usr/bin/env python3
"""
gen_testrom.py — emit a PS-X EXE of cycle-isolation micro-benchmark loops.

This is ruler #2 (FAITHFUL_TIMING_PLAN.md §3c / ACCURACY_BURNDOWN.md axis 2): a
purpose-built, in-tree, version-controlled cycle test program. Unlike organic
BIOS/game code (which mixes cost components in one basic block), each loop here
exercises EXACTLY ONE cost component, so its per-iteration cycle delta isolates
that component. We know the analytic answer by hand, and we run the SAME EXE on
both backends:
  - native: recompiled by psxrecomp-game, invoked + cyc_watch'd (compiled model)
  - Beetle: sideloaded raw PS-EXE (mednafen EXE loader), cyc_watch'd (HW oracle)
and Δ-compare per-iteration. It is a permanent regression harness for the cost
model, good for ANY future PSX title, not just the current vehicle.

ISOLATION METHOD (baseline subtraction): every loop shares the SAME loop overhead
(addiu counter; bne; nop). A `baseline` loop measures that overhead alone; each
component loop adds ONLY its op(s). (component_per_iter − baseline_per_iter) = the
component's cycle cost, with loop overhead AND fetch (both fully cache-resident
after warm-up) cancelled out.

ANCHOR semantics: each loop's TOP is a basic-block leader (it is a branch target),
so cyc_watch single-anchor consecutive-hit Δ = one iteration's cost — the same
measurement on both backends.

The MIPS is hand-encoded (tiny built-in assembler for the subset used) so there is
NO external toolchain dependency; the output is deterministic and reproducible.
"""

import struct
import sys

# --- minimal MIPS32 (R3000, little-endian) encoder for the subset we use -------

REG = {f"${i}": i for i in range(32)}
REG.update({  # ABI names we use
    "$zero": 0, "$at": 1, "$v0": 2, "$v1": 3,
    "$a0": 4, "$a1": 5, "$a2": 6, "$a3": 7,
    "$t0": 8, "$t1": 9, "$t2": 10, "$t3": 11,
    "$t4": 12, "$t5": 13, "$t6": 14, "$t7": 15,
    "$s0": 16, "$s1": 17, "$ra": 31, "$sp": 29, "$gp": 28,
})

def _r(x):
    if isinstance(x, int): return x
    return REG[x]

def R(funct, rs=0, rt=0, rd=0, sa=0):
    return (0 << 26) | (_r(rs) << 21) | (_r(rt) << 16) | (_r(rd) << 11) | (sa << 6) | funct

def I(op, rs=0, rt=0, imm=0):
    return (op << 26) | (_r(rs) << 21) | (_r(rt) << 16) | (imm & 0xFFFF)

# instruction helpers
def nop():               return 0
def addu(rd, rs, rt):    return R(0x21, rs, rt, rd)
def addiu(rt, rs, imm):  return I(0x09, rs, rt, imm)
def ori(rt, rs, imm):    return I(0x0D, rs, rt, imm)
def lui(rt, imm):        return I(0x0F, 0, rt, imm)
def sll(rd, rt, sa):     return R(0x00, 0, rt, rd, sa)
def divu(rs, rt):        return R(0x1B, rs, rt)
def multu(rs, rt):       return R(0x19, rs, rt)
def mflo(rd):            return R(0x12, 0, 0, rd)
def mfhi(rd):            return R(0x10, 0, 0, rd)
def lw(rt, off, base):   return I(0x23, base, rt, off)
def sw(rt, off, base):   return I(0x2B, base, rt, off)
def bne(rs, rt, off):    return I(0x05, rs, rt, off)   # off in words, signed
def beq(rs, rt, off):    return I(0x04, rs, rt, off)
def jr(rs):              return R(0x08, rs)
def j(target_word):      return (0x02 << 26) | (target_word & 0x03FFFFFF)
def jal(target_word):    return (0x03 << 26) | (target_word & 0x03FFFFFF)

def li(rt, val):
    """load-immediate macro → (lui, ori) or a single addiu for small values."""
    val &= 0xFFFFFFFF
    if val < 0x10000:
        return [ori(rt, 0, val)]
    if (val & 0xFFFF) == 0:
        return [lui(rt, val >> 16)]
    return [lui(rt, val >> 16), ori(rt, rt, val & 0xFFFF)]

# --- EXE layout ----------------------------------------------------------------

LOAD_ADDR = 0x80010000
ENTRY     = LOAD_ADDR        # entry == first instruction
STACK_TOP = 0x801FFF00
SCRATCH_RAM = 0x80012000     # a valid main-RAM address we lw/sw against
ITER = 20000                 # inner-loop trip count (large → many anchor hits)

# Each test loop is laid out as a function. We record the loop-top address
# (the anchor) for the harness/README to consume. The outer entry runs the
# tests round-robin forever so anchors keep getting hit.

class Asm:
    def __init__(self, base):
        self.base = base
        self.words = []
        self.labels = {}
    def here(self):
        return self.base + 4 * len(self.words)
    def label(self, name):
        self.labels[name] = self.here()
    def emit(self, *ws):
        for w in ws:
            if isinstance(w, list):
                self.words.extend(w)
            else:
                self.words.append(w)
    def word_addr(self, addr):
        return (addr & 0x0FFFFFFF) >> 2

def build():
    a = Asm(LOAD_ADDR)
    anchors = {}

    # ---- entry: set up sp, operands, then round-robin call the tests forever ----
    a.label("entry")
    a.emit(li("$sp", STACK_TOP))
    a.emit(li("$t5", SCRATCH_RAM))     # load/store base
    a.emit(li("$t2", 0x12345678))      # div/mult operand (dividend/multiplicand)
    a.emit(li("$t3", 0x0000007F))      # div/mult operand (divisor/multiplier, !=0)
    # store something at the scratch addr so loads read a known value
    a.emit(sw("$t2", 0, "$t5"))

    a.label("outer")
    # call each test (jal needs absolute word target; patch after layout). We
    # use placeholders and fix up below by re-emitting once addresses are known.
    # Simpler: emit tests INLINE in the outer loop body (no jal), each as its own
    # counted inner loop. The inner-loop tops are the anchors. After all tests,
    # branch back to outer. This keeps it call-free (no $ra juggling).

    def inner_loop(name, body_ops):
        """Emit: li counter; TOP: <body>; addiu counter,-1; bne TOP; nop."""
        a.emit(li("$t0", ITER))
        top = a.here()
        anchors[name] = top
        a.label(name + "_top")
        a.emit(*body_ops)
        a.emit(addiu("$t0", "$t0", -1))
        # bne offset: from delay-slot+4 to top. off_words = (top - (here+4))/4
        br_here = a.here()
        off = (top - (br_here + 4)) >> 2
        a.emit(bne("$t0", "$zero", off))
        a.emit(nop())

    # baseline: loop overhead only
    inner_loop("baseline", [])
    # alu: one addu (pure execute, should be +1 over baseline)
    inner_loop("alu", [addu("$t6", "$t2", "$t3")])
    # load: one lw from main RAM (isolates memory read wait-state)
    inner_loop("load", [lw("$t4", 0, "$t5")])
    # load2: two lw (linearity check: should be ~2x the load delta)
    inner_loop("load2", [lw("$t4", 0, "$t5"), lw("$t7", 0, "$t5")])
    # div: divu + mflo (isolates div latency + stall-on-read, worst case)
    inner_loop("div", [divu("$t2", "$t3"), mflo("$t4")])
    # div_spaced: divu, 2 filler addu, then mflo (stall partly absorbed)
    inner_loop("div_spaced", [divu("$t2", "$t3"),
                              addu("$t6", "$t6", "$t3"),
                              addu("$t6", "$t6", "$t3"),
                              mflo("$t4")])
    # mult: multu + mflo (isolates mult latency)
    inner_loop("mult", [multu("$t2", "$t3"), mflo("$t4")])

    # loop outer forever
    o = a.labels["outer"]
    here = a.here()
    off = (o - (here + 4)) >> 2
    a.emit(beq("$zero", "$zero", off))
    a.emit(nop())

    return a, anchors

def make_psexe(words):
    text = b"".join(struct.pack("<I", w) for w in words)
    text_size = len(text)
    # PS-EXE: pad text size to 0x800 multiple
    if text_size % 0x800:
        text += b"\x00" * (0x800 - (text_size % 0x800))
    text_size_field = len(text)

    hdr = bytearray(0x800)
    hdr[0x00:0x08] = b"PS-X EXE"
    # 0x10 init PC, 0x14 init GP, 0x18 text load addr, 0x1C text size
    struct.pack_into("<I", hdr, 0x10, ENTRY)
    struct.pack_into("<I", hdr, 0x14, 0)            # gp
    struct.pack_into("<I", hdr, 0x18, LOAD_ADDR)
    struct.pack_into("<I", hdr, 0x1C, text_size_field)
    # 0x30 stack base, 0x34 stack size (BIOS sets sp; we also set it in code)
    struct.pack_into("<I", hdr, 0x30, STACK_TOP)
    struct.pack_into("<I", hdr, 0x34, 0)
    # marker region region text (0x4C..) often holds an ASCII license string; leave blank
    return bytes(hdr) + text

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "cycle_testrom.exe"
    a, anchors = build()
    blob = make_psexe(a.words)
    with open(out, "wb") as f:
        f.write(blob)
    print(f"wrote {out}  ({len(blob)} bytes, {len(a.words)} instrs, "
          f"text@0x{LOAD_ADDR:08X}, entry@0x{ENTRY:08X})")
    print("anchors (loop-top block leaders) — arm cyc_watch single-anchor on these:")
    for name, addr in anchors.items():
        print(f"  {name:12s} 0x{addr:08X}")
    # emit an anchors json for the harness
    import json
    meta = {
        "load_address": f"0x{LOAD_ADDR:08X}",
        "entry_pc": f"0x{ENTRY:08X}",
        "text_size": f"0x{(len(blob)-0x800):08X}",
        "iter": ITER,
        "scratch_ram": f"0x{SCRATCH_RAM:08X}",
        "anchors": {k: f"0x{v:08X}" for k, v in anchors.items()},
    }
    with open(out + ".anchors.json", "w") as f:
        json.dump(meta, f, indent=2)
    print(f"wrote {out}.anchors.json")

if __name__ == "__main__":
    main()
