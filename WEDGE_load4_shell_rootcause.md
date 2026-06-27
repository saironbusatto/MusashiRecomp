# Root-cause: load=4 wedges Tomba 2 BIOS-shell boot (branch wt/tomba2-load-accuracy)

Companion to the memory.c load wait-state note. When PSX_RAM_READ_WAIT_CYCLES=4
(the Beetle-oracle-accurate value, vs the old DuckStation 6), Tomba 2's boot
deterministically wedges in the BIOS shell. This documents the root cause as far
as traced, and the next step.

## Symptom (reproducible, deterministic)

- pc=0 / halt; `total_checks` frozen at exactly 134756056 across restarts.
- `current_func` = 0x1FC2CE28 (BIOS shell fn, ROM 0xBFC2CE28, runs at RAM
  0x80044E28 via the shell relocation 0xBFC18000+ → RAM 0x30000+).
- `last_store_pc` = 0xBFC2CE64. i_stat 0x80 (VBLANK pending), i_mask 0x0D.
- Runs via the dirty-RAM interpreter (relocated shell). Reached via a B0 call.

## Mechanism (CONFIRMED by register state at the freeze)

The shell fn at 0xBFC2CE28 indexes a struct array by a handle `s1` (= input v0):

```
bfc2ce28: bgez v0, 0xbfc2ce38     ; LOWER-bound guard only (v0<0 → error -1)
bfc2ce2c:   move s1, v0           ; s1 = the handle
; index = s1 * 1672  (s1*4-s1=*3, *4=*12,+s1=*13, *16=*208,+s1=*209, *8 = *1672)
bfc2ce50: lui t7,0x8011           ; table base 0x80110EC8
bfc2ce60: addu v0, t6, t7         ; v0 = 0x80110EC8 + s1*1672
bfc2ce64: sw s0, 0x28(sp)         ; (last store before wedge)
...                               ; struct copy into [v0], + 3 intra-shell jals
```

At the freeze: **s1 = 0x68 = 104**, t6 = 0x2A740 = 104×1672, v0 = 0x80110EC8 +
0x2A740 = **0x8013B608** — a WILD pointer far past the table. There is NO
upper-bound check, so the struct copy corrupts memory at 0x8013B608+, leading to
the downstream pc=0 halt.

With PSX_RAM_READ_WAIT_CYCLES=6 the same fn runs with a small valid s1 (the table
has only a handful of 1672-byte slots — consistent with BIOS-shell memcard
save-file display structs: icon frames + metadata). So **s1 is timing-dependent**.

## Why timing-dependent → the real bug

s1 = v0 is returned by a prior call (the shell's memcard save-file enumeration —
the freeze state shows heavy SIO/memcard activity: mc_read_done 72, sio_irq_total
16220, tx_writes 17341). The faster-but-accurate CPU (load=4) outruns the
memcard/SIO RESPONSE TIMING, so the enumeration's file count/index comes out wrong
(104 instead of a small value). The underlying inaccuracy is **axis-5 peripheral
(memcard/SIO) response timing is not faithful** — it is implicitly tuned to the
old (too-slow) CPU load cost. This is exactly the faithful-core cascade: an
accurate CPU cost exposes an inaccurate device timing.

## Next step

1. CONFIRM the source of v0=104: trace the caller (ra=0x80044F10 → caller jal at
   RAM 0x80044F08 / ROM 0xBFC2CF08) back to the memcard-enumeration call that
   returns the handle; or run a native(load=4)↔Beetle first-divergence on the
   shell to find the first forked value. (find_divergence.py is stale — use the
   cyc_watch/ring methodology or a fresh comparator.)
2. FIX the device timing faithfully (memcard/SIO response paced on the guest
   cycle clock, not implicitly on the old load cost) so enumeration is
   CPU-speed-independent. This is an axis-5 task (ACCURACY_BURNDOWN.md), the
   suspected-weakest axis. Then load=4 should boot.
3. A defensive upper-bound check in the shell fn would mask the symptom but NOT
   fix the wrong count — do not do that (CLAUDE.md Rule -1: fix the cause).

Until then: branch wt/tomba2-load-accuracy carries the accurate load=4 + this
wedge; wt/tomba2-cycle-audit keeps the booting load=6 for ongoing FMV/ruler
validation.
