# Building psx-beetle on Linux

The psx-beetle oracle builds and runs on Linux with the same wire protocol
as on Windows. Verified 2026-07-02 (Kula World boots to demo gameplay;
ping/screenshot served over the TCP debug protocol).

## beetle-psx checkout

`runtime/src/beetle_libretro.cpp` targets the C++ beetle-psx tree. Upstream
master has since been converted to plain C (`libretro.cpp` -> `libretro.c`,
`PS_CPU` class dropped), which no longer compiles against our integration.
Pin the checkout to the last compatible commit — the same base
`docs/beetle_wtrace_hook.patch` was generated from:

```bash
git clone https://github.com/libretro/beetle-psx-libretro.git beetle-psx
cd beetle-psx
git checkout 5759277b          # "audit pass" — last C++-tree base we target
patch -p1 < ../docs/beetle_wtrace_hook.patch
patch -p1 < ../docs/beetle_sio_trace_hook.patch
patch -p1 < ../docs/beetle_cdcmd_trace_hook.patch
patch -p1 < ../docs/beetle_oracle_buildfix.patch   # REQUIRED — see below
```

**The first three patches do not build on their own.** `beetle_cdcmd_trace_hook`
was generated against a tree without `beetle_wtrace_hook` applied, so applying
both duplicates `g_psxrecomp_wtrace_cb` / `g_psxrecomp_fntrace_cb` and
`libretro.cpp` fails with "redefinition of ...". Separately,
`runtime/src/beetle_libretro.cpp` requires `g_psxrecomp_rtrace_cb` and
`g_psxrecomp_irq_cb`, which no committed patch provides.
`beetle_oracle_buildfix.patch` resolves both; apply it last.

With all four applied the libretro core (`libmednafen_psx.a`) builds clean.
The `psx-beetle` target does NOT link yet: it additionally wants
`beetle_core_get_guest_cycles`, `beetle_cyc_watch_{arm,clear,get,get_state}`,
`psxrecomp_cdc_irq_type`, `psxrecomp_dma_int_status` and
`retro_psxref_exc_ring_dump` — instrumentation that only ever existed on the
original dev machine and was never captured as a patch. Reconstructing it
means inventing semantics, so it is left open rather than guessed.

`beetle_cdcmd_trace_hook.patch` adds a CD-command trace callback (fires per
command dispatch in cdc.cpp) exposed as the `cdrom_cmd_dump` / `cdrom_cmd_reset`
debug commands — used to oracle-diff the Kula World CD read sequence.

`beetle_sio_trace_hook.patch` adds `FrontIO::SetSIOTraceCallback` (fires per
completed SIO byte exchange) — an integration hook beetle_libretro.cpp needs
that predated the committed wtrace patch.

## Build

```bash
# Static lib. On unix the artifact is named mednafen_psx_libretro.so but is
# an ar archive (STATIC_LINKING=1); stage it under the name cmake expects.
make platform=unix STATIC_LINKING=1 HAVE_LIGHTREC=0 -j"$(nproc)"
cp mednafen_psx_libretro.so libmednafen_psx.a

cd ../runtime
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPSX_LAUNCHER=OFF -DPSX_DEBUG_TOOLS=ON
ninja -C build psx-beetle

# Headless run (picks the next free port from 4380 if taken; watch stderr)
xvfb-run -a ./runtime/build/psx-beetle ../bios/SCPH1001.BIN --disc <game.cue>
```
