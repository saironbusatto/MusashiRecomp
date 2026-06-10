# TCP Debug Server Commands

Protocol: **JSON over newline**, one object per line, responses on same connection.

- Request shape: `{"id": N, "cmd": "<command>", ...params}`
- Success: `{"id": N, "ok": true, ...data}`
- Failure: `{"id": N, "ok": false, "error": "<msg>"}`

There are **two** servers, both implementing this protocol with overlapping command sets:

| Server | Port | Source |
|---|---|---|
| **Native** (our recompiled runtime) | `4370` | `runtime/src/debug_server.c` |
| **DuckStation** (oracle) | `4371` | `duckstation/src/core/psxrecomp_debug_server.cpp` (patched, see `tools/duckstation/psxrecomp_oracle.patch`) |

The `debug_client.py` CLI can target either, or `compare` two at once to diff state live — that's how divergence hunts work.

```bash
python tools/debug_client.py <cmd> [args]           # native (port 4370)
python tools/debug_client.py --port 4380 <cmd>      # psx-beetle
python tools/debug_client.py --ds <cmd> [args]      # duckstation (port 4371)
python tools/debug_client.py compare <cmd>          # run on both, diff results
```

Commands without a bespoke CLI mapping pass through generically: extra
args of the form `key=value` become JSON fields (ints when numeric, else
strings), so every server command is reachable, e.g.
`debug_client.py --port 4370 gpu_frame_dump frame=14528 count=65536`.

---

## Command inventory

Columns: **N** = native, **D** = DuckStation oracle.

| Command | N | D | Params | Description |
|---|---|---|---|---|
| `ping` / `frame` | ✓ | ✓ | — | Heartbeat + current frame number |
| `get_registers` (`regs`) | ✓ | ✓ | — | All 32 GPRs + PC + HI + LO (native also: COP0 SR/Cause/EPC, I_STAT, I_MASK) |
| `read_ram` | ✓ | ✓ | `addr`, `len` | Read bytes from PS1 address space as hex string — up to the full 2 MB in ONE response line. `dump_ram` is an alias (the old chunked multi-line variant is gone: it broke the one-request/one-response protocol and wedged the server) |
| `write_ram` | ✓ | ✓ | `addr`, `hex` | Write bytes to PS1 address space |
| `read_scratch` |   | ✓ | `addr`, `len` | Read PS1 scratchpad (0x1F800000 region) |
| `read_vram` / `vram_peek` | ✓¹ | ✓ | `x`, `y`, `w`, `h` | Read 16-bit VRAM pixels (max 128×128) |
| `gpu_state` | ✓ | ✓ | — | Display area, display depth, draw offset, GPUSTAT, clip rect, xfer state |
| `sio_state` | ✓ | ✓ | — | SIO registers + (native only) pad/memcard protocol + TX/RX history |
| `irq_state` | ✓ | ✓ | — | `I_STAT`, `I_MASK` (both), plus chain state on native |
| `dma_state` | ✓ | ✓ | — | DPCR, DICR, all 7 channel states (madr/bcr/chcr) |
| `event_state` |   | ✓ | — | EvCB table summary (stub on DS — events are BIOS-level) |
| `overlay_state` |   | ✓ | — | Current overlay info |
| `cdrom_sector_dump` | ✓ |   | `offset`, `len` | Dump bytes from the last CD-ROM sector observed by the controller, including LBA/mode metadata |
| `cdrom_sector_history` | ✓ |   | `count`, optional `lba` | Dump newest CD-ROM sector history entries, including raw XA subheader fields, CPU/audio delivery flags, and the first 128 bytes |
| `cdrom_sector_history_clear` | ✓ |   | — | Reset the CD-ROM sector history ring |
| `watch` | ✓ | ✓ | `addr` | Set byte-level memory watchpoint (fires per-frame on change) |
| `unwatch` | ✓ | ✓ | `addr` | Remove memory watchpoint |
| `set_input` | ✓ | ✓ | `buttons`, optional `frames`, optional `lx`, `ly`, `rx`, `ry` | Override pad1 buttons and optional analog axes (PS1 inverted bitmask, 0 = pressed; axes 0-255). Holds until `clear_input` on both backends; pass `frames=N` (beetle) to auto-release after N frames |
| `clear_input` | ✓ | ✓ | — | Remove input and analog axis overrides |
| `turbo` | ✓ |   | `enabled` | Enable/disable TCP-controlled frontend turbo for fast-forward validation |
| `turbo_state` | ✓ |   | — | Query TCP-controlled turbo state |
| `pause` | ✓ | ✓ | — | Pause emulation |
| `continue` (`c`) | ✓ | ✓ | — | Resume emulation |
| `step` | ✓ | ✓ | `[count]` | Step N frames (default 1) |
| `run_to_frame` | ✓ | ✓ | `frame` | Run until frame number, then pause |
| `history` | ✓ | ✓ | — | Ring buffer stats (frames available) |
| `get_frame` | ✓ | ✓ | `frame` | Full frame record from ring buffer |
| `frame_range` | ✓ | ✓ | `start`, `end` | Range query, max 200 frames |
| `frame_timeseries` | ✓ | ✓ | `start`, `end` | Compact timeseries, max 200 frames |
| `set_snapshot` | ✓ | ✓ | `slot`, `addr`, `size` | Configure per-frame RAM snapshot region (slots 0-3) |
| `get_snapshots` | ✓ | ✓ | — | Show snapshot config |
| `screenshot` | ✓ | ✓ | `path` (optional) | Write a 24-bit BMP of the current display to `path` (default `psx_screenshot.bmp` in the runtime cwd); single metadata response `{path,width,height}`. `screenshot_file` is an alias; the old inline-hex-row `screenshot` is gone (it streamed h+1 response lines per request and poisoned the connection) |
| `first_failure` | ✓ |   | — | Find first divergence point between runs (native-side tracking) |
| `read_frame_ram` | ✓ |   | `addr`, `len`, `frame` | Read RAM **as of a specific frame** (from ring buffer) |
| `wtrace_range` | ✓ |   | `lo`, `hi` | Set RAM-write trace range (ring of 1024 writes with RA) |
| `wtrace_dump` | ✓ | beetle | optional `addr_lo`, `addr_hi`, `count`, `newest` | Dump RAM-write trace entries as JSON. The address filter is applied server-side over the FULL ring before the emit cap — always pass it when hunting a specific buffer, otherwise you only see the oldest `count` entries of the whole ring |
| `wtrace_clear` | ✓ |   | — | Reset the trace ring |
| `pc_break` |   | ✓² | `addr` | DS execute breakpoint, state captured on hit (via `pc_hit_last`) |
| `pc_unbreak` |   | ✓² | `addr` | Remove an execute breakpoint |
| `pc_break_list` |   | ✓² | — | List active execute breakpoints |
| `pc_hit_last` |   | ✓² | — | Captured state (PC, $ra, all GPRs, COP0) from most recent PC break hit |
| `pc_hit_clear` |   | ✓² | — | Clear the last-hit record |
| `quit` | ✓ |   | — | Shutdown native runtime |

¹ Native `vram_peek` is the legacy name; DS calls it `read_vram`. Same semantics.  
² The `pc_*` family is specific to the DS oracle: DuckStation's CPU core honours `CPU::AddBreakpointWithCallback`, while our native runtime dispatches whole recompiled functions (no mid-function PC breaks).

---

## Divergence-hunt workflow

When a recompiled-BIOS bug is suspected, the two servers let you find the **first** divergence instead of chasing symptoms. Standard procedure (inherited from v3's `DEBUG.md`):

1. **Sync state via PC + registers, not frame number.** Frames drift after even a single timing glitch. Pause both servers; compare `get_registers` until they match.
2. **Dump both sides fully.** Compare `get_frame`, `gpu_state`, `irq_state`, `dma_state` (DS), `dump_ram` over the same regions.
3. **Byte-level comparison.** Tiny mismatches usually point at one subsystem. Use `debug_client.py compare <cmd>` for automatic diff.
4. **Find the earliest mismatch**, not a later symptom. Ring-buffer queries (`frame_range`, `read_frame_ram`) help locate which frame went wrong.
5. **Trace the write.** Use `watch` to catch the divergent store, or DS's `pc_break` on the suspect function entry. Look at `$ra` in `pc_hit_last` to identify the caller chain.
6. **Classify.** codegen (recompiler generates wrong instruction), runtime (MMIO or kernel simulation wrong), timing (IRQ cadence), or BIOS (real-hardware quirk we didn't model).
7. **Minimal fix** in the correct subsystem. Never hand-deliver state to hide the symptom (see CLAUDE.md §0).

---

## Rule when the server can't answer your question

If an inspection need isn't covered by the existing commands, **do not fall back to printf or log files**. Instead:

1. Add a handler in `runtime/src/debug_server.c` (native)
2. Add the matching handler in `duckstation/src/core/psxrecomp_debug_server.cpp` (DS), regenerate the patch via the instructions in `tools/duckstation/README.md`
3. Keep field names parallel between the two
4. Update this file

The TCP server is the canonical instrumentation surface. Rule 3 in `CLAUDE.md` is absolute: **no `fprintf(stderr, …)` in source code, ever, for any reason**.
