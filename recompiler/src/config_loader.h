// config_loader.h — shared TOML config loader for psxrecomp-{bios,game}.
//
// Mirrors the schema accepted by tools/audit_config.py (Python side). See
// docs/config_schema.md for the field reference.
//
// Two entry points:
//   load_bios_config(path)  reads bios/SCPH1001.toml — describes the BIOS
//   load_game_config(path)  reads <game>/game.toml   — describes a game EXE
//
// A runtime/process that needs both calls both; the BIOS one is the
// always-loaded base, the game one is layered on top (merge semantics are
// the caller's responsibility for now — recompiler tools consume one or
// the other independently).
//
// Paths inside the TOML are resolved relative to the detected project
// root (the nearest ancestor of the config file that has .gitignore,
// .git, or CMakeLists.txt).

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace PSXRecompV4 {

// [runtime] block — consumed by runtime/src/main.cpp. All fields optional;
// callers that need them check has_* flags or use the supplied defaults.
struct RuntimeConfig {
    bool                  has_debug_port = false;
    uint16_t              debug_port     = 0;

    bool                  has_window_title = false;
    std::string           window_title;

    bool                  has_memcard_dir = false;
    std::filesystem::path memcard_dir;     // absolute path (resolved against project root)

    // disc_speed: "1x" (default) | "2x" | "4x" | "instant"
    // Controls how quickly CD-ROM timing delays fire. "instant" collapses all
    // seek/read delays to 1 cycle — correct INT sequence, no artificial wait.
    bool                  has_disc_speed = false;
    std::string           disc_speed;      // raw string; main.cpp converts to divisor

    // instant_max_per_frame: per-frame sector-IRQ budget while disc_speed =
    // "instant" (cdrom.c floors the per-sector period to VBLANK/N). Absent =
    // cdrom.c built-in default. Runtime-tunable via the cdrom_instant_rate
    // TCP command; the turbo-through-loads predicate drives the same knob.
    bool                  has_instant_max_per_frame = false;
    int                   instant_max_per_frame = 0;

    // fast_boot: snapshot BIOS state at first game handoff and restore it on
    // subsequent launches, skipping BIOS execution entirely. Default off;
    // enable per-game in [runtime]. Snapshot is keyed on BIOS SHA256 + entry_pc.
    bool                  fast_boot = false;

    // overlay_cache: enable the overlay DLL cache + capture (Layer A). Off by
    // default. When true the runtime scans cache/<game_id>/ for precompiled
    // overlay DLLs (loaded ahead of the dirty-RAM interpreter) and records
    // overlay bytes to overlay_captures.json for offline compilation.
    bool                  overlay_cache = false;

    // turbo_loads: OPT-IN per game. While the game is loading (CD data
    // stream active, XA/FMV excluded, post-BIOS-handoff only) the frontend
    // skips wall-clock pacing so the guest runs at host speed — compressing
    // load wall-time. Streaming titles (e.g. Crash) must leave this off.
    bool                  turbo_loads = false;

    // overlay_autocompile_cmd: variant-capture automation (step 2.8). A
    // shell command (run via cmd.exe /C, cwd = project root) that compiles
    // overlay_captures.json into the cache — normally the project's
    // compile_overlays.py invocation. When set (and overlay_cache is on),
    // the runtime auto-captures on sustained capture-window interp pressure
    // and spawns this command in the background; on success the loader
    // rescans the cache and the new variant goes native in-session.
    bool                  has_overlay_autocompile_cmd = false;
    std::string           overlay_autocompile_cmd;

    // overlay_backend: Tier-2 codegen backend selection (SLJIT.md §1).
    // "auto" (default, empty == auto) | "gcc" | "sljit". auto resolves to gcc
    // when overlay_autocompile_cmd is configured (a dev machine with a
    // toolchain), else sljit (self-contained / toolchain-less). The env var
    // PSX_OVERLAY_BACKEND overrides this at runtime. gcc stays the default
    // until the sljit emitter passes the same-state differential gate.
    std::string           overlay_backend;

    // ---- [video] block — visual enhancement options ----
    // supersampling: internal-resolution SSAA factor (per axis). 1 = native
    // (default, behaves exactly as before). 2..4 render geometry/shading into
    // an N*-scaled mirror of VRAM and downsample on present — true ordered-grid
    // supersampling + edge anti-aliasing. Cost scales ~N^2 in fill rate.
    int                   video_supersampling = 1;

    // antialiasing: when true the present path uses linear filtering when
    // scaling the framebuffer to the window (smooths the supersample
    // downscale and any window resize). false = nearest (sharp pixels).
    // Defaults to true.
    bool                  video_antialiasing = true;

    // texture_filtering: "nearest" (default, native PSX look) | "bilinear"
    // (smooths textures and 2D backgrounds). Stored as 0/1.
    int                   video_texture_filter = 0;

    // renderer: "software" (default) | "opengl". Selects the rasterizer/present
    // backend. The OpenGL backend is a hardware-accelerated alternative; the
    // software rasterizer remains the fallback. Stored as 0=software, 1=opengl.
    int                   video_renderer = 0;

    // crt_filter: present-time screen-colour model (verified-enhancement LUT).
    // "raw" (default, byte-identical 5->8 passthrough) | "crt" | "composite" |
    // "trinitron". Stored 0..3 to match ScreenKind in runtime/color_lut.h. The
    // PSX_SCREEN env var overrides this at runtime (debug path).
    int                   video_screen_kind = 0;

    // auto_skip_fmv: when true, full-motion videos (streaming XA audio + MDEC
    // video) are fast-forwarded invisibly to their end — the disc stream is
    // delivered instantly, presentation + pacing are suppressed, and audio is
    // muted for the duration, so an FMV plays out in a fraction of a second with
    // nothing shown. The game's own player loop still runs to end-of-stream, so
    // all side effects (scene transition, save flags) happen exactly as normal.
    bool                  video_auto_skip_fmv = false;

    // aspect_ratio: display aspect "W:H" (default "4:3" = native). A wider
    // aspect (e.g. "16:9") enables the widescreen hack: the GTE squashes
    // screen-X by (4*H)/(3*W) around the game's projection centre and the
    // present path stretches the 4:3 frame to W:H — net effect is a wider
    // field of view for GTE-projected geometry. Screen-space 2D (HUD, FMV,
    // sprite widths) stretches; world geometry keeps correct proportions.
    int                   video_aspect_num = 4;
    int                   video_aspect_den = 3;

    // ---- [audio] block ----
    // spu_hq: enable the SPU float-shadow re-render (Catmull-Rom resample, float
    // headroom). Verified-enhancement, default OFF — spu_render output is
    // byte-identical to the canon hardware mix when off. The PSX_AUDIO_SHADOW
    // env var overrides this at runtime (debug path).
    bool                  audio_spu_hq = false;

    // ---- [controller] block — game-declared input defaults ----
    // default_analog: present a DualShock/analog pad (id 0x73) by default for
    // this game, so a stick-capable title (e.g. Tomba, which gives variable run
    // speed through the analog stick) feels right out of the box without the
    // user toggling DualShock in the launcher. Per-install settings.toml
    // [controller] p1_analog/p2_analog still override. Default off (digital 0x41).
    // `default_analog = true` sets both ports; `p1_analog`/`p2_analog` set one.
    bool                  has_default_analog = false;
    bool                  default_p1_analog  = false;
    bool                  default_p2_analog  = false;

    // deadzone: default analog-stick deadzone in raw SDL axis units (0..32767).
    // Applied both to the stick->d-pad press threshold and the analog-axis centre
    // dead-band. Absent => runtime default (12000). Overridden per-install by
    // settings.toml [controller] deadzone and by input.ini.
    bool                  has_deadzone = false;
    int                   deadzone     = 0;
};

// One entry from [[recompiler.bios_vectors]].
// Describes a BIOS vector dispatch stub (A0/B0/C0) that the BIOS installs
// into low RAM at boot. The recompiler reads the function pointer table from
// the ROM binary at build time and emits a static C switch handler so these
// addresses are resolved as binary-search hits at runtime rather than falling
// through to dirty_ram_interp.
struct BiosVectorTable {
    uint32_t ram_addr;       // RAM address of the installed stub (e.g. 0xA0)
    int      index_reg;      // CPU register that holds the function index ($t1 = 9)
    uint32_t table_rom_addr; // ROM virtual address of the function pointer table
    uint32_t table_count;    // number of entries to read from the table
    // Runtime RAM address of the live function table (used as fallback for
    // Shell-patched entries not present in ROM). 0 = no runtime fallback.
    uint32_t table_ram_addr;
};

// One entry from [[recompiler.bios_aliases]].
// A RAM address the BIOS installs a simple fixed-target trampoline at
// (e.g. the SIO handler at 0x0CF0 which just jalrs to 0x641C). Emitted as
// a one-liner wrapper in the dispatch table — no table lookup, no switch.
struct BiosAlias {
    uint32_t ram_addr;    // the installed stub address (e.g. 0x0CF0)
    uint32_t target_key;  // normalized dispatch key of the target (e.g. 0x641C)
};

struct BiosConfig {
    std::filesystem::path config_path;   // the toml file itself
    std::filesystem::path project_root;  // resolved via .gitignore/.git/CMakeLists.txt walk

    // [program] block
    std::string           name;          // display name, e.g. "SCPH1001 BIOS"
    std::string           id;            // canonical id, e.g. "SCPH-1001"
    std::filesystem::path rom_path;      // absolute path to BIOS ROM
    uint32_t              load_address;
    uint32_t              entry_pc;
    uint32_t              text_size;

    // [recompiler] block
    std::filesystem::path seeds_path;    // absolute path to seeds JSON
    std::filesystem::path out_dir;       // absolute path to output dir
    bool                  strict;        // currently always true
    std::string           out_stem;      // derived if not explicit
    std::vector<BiosVectorTable> bios_vectors; // optional vector dispatch tables
    std::vector<BiosAlias>       bios_aliases; // optional fixed-target trampolines

    // [runtime] block (optional)
    RuntimeConfig         runtime;
};

struct GameConfig {
    std::filesystem::path config_path;
    std::filesystem::path project_root;

    // [game] block
    std::string           name;          // e.g. "Tomba!"
    std::string           id;            // e.g. "SCUS-94236"
    std::filesystem::path exe_path;      // absolute path to PS-X EXE
    uint32_t              load_address;
    uint32_t              entry_pc;
    uint32_t              text_size;
    uint32_t              stack_base;    // initial $sp
    // disc paths (Phase D will properly support multi-disc; for now we
    // accept either a single `disc = "..."` or `discs = [...]` and store
    // the union here).
    std::vector<std::filesystem::path> discs;

    // Optional expected disc identity, for the launcher's "Disc verified" badge.
    // disc_crc: full-file CRC32 (IEEE) of the data track. disc_sha1: lowercase
    // hex SHA-1. Either may be absent (has_disc_crc / disc_sha1.empty()).
    bool                  has_disc_crc = false;
    uint32_t              disc_crc = 0;
    std::string           disc_sha1;

    // [recompiler] block
    std::filesystem::path seeds_path;     // absolute path to seeds (text or json)
    std::filesystem::path bios_thunks_path; // optional; empty if not set
    std::filesystem::path out_dir;
    bool                  strict;
    std::string           out_stem;       // derived if not explicit

    // [runtime] block (optional)
    RuntimeConfig         runtime;

    // [widescreen] block (optional) — per-game knobs for the widescreen hack
    // ([video] aspect_ratio != 4:3). All default to inert; a game with no
    // [widescreen] block gets the plain GTE squash + stretched present only.
    //
    // sprite_tag_funcs: guest addresses of functions called once per
    //   character/billboard prim with the prim pointer in $a0 (the recompiler
    //   emits a psx_ws_sprite_tag(cpu) callback at their entry). Tagged prims
    //   get their X coords re-squashed around the prim's projected anchor at
    //   GP0 submission, undoing the present stretch so sprites keep correct
    //   proportions.
    // sprite_anchor_addr: scratchpad address holding the prim's projected
    //   anchor SXY (written by the game's RTPS preamble) at tag time.
    // hud_sprt_squash: center-squash every UNtagged textured-rect (SPRT)
    //   prim — pure screen-space 2D (HUD, menus) — so it presents at native
    //   proportions. Untextured TILEs (fades) are never touched.
    std::vector<uint32_t> ws_sprite_tag_funcs;
    uint32_t              ws_sprite_anchor_addr = 0;
    bool                  ws_hud_sprt_squash = false;

    // Cull-margin widening. The game's per-object draw classifier compares
    // (objX - camX + BIAS) against a RANGE derived from the 4:3 screen width;
    // the GTE squash shows ~33% more world, so the fixed margin collapses and
    // objects pop in/out near the wide-screen edges. We widen the window by
    // emitting a runtime margin term psx_ws_x_margin() (0 at 4:3/boot/menu/FMV,
    // ~the half-extra-width when stretching) into the relevant immediates:
    //   cull_bias_sites:  an addiu rT,rS,imm → rT = rS + (imm + margin)
    //   cull_range_sites: an sltiu rT,rS,imm → rT = rS <u (imm + 2*margin)
    //   cull_a1_sites:    a nop (load/branch-delay) → a1 += margin (for the
    //                     caller-supplied-margin classifier variants)
    // All Ghidra-evidenced; empty by default. Changing these requires a regen.
    std::vector<uint32_t> ws_cull_bias_sites;
    std::vector<uint32_t> ws_cull_range_sites;
    std::vector<uint32_t> ws_cull_a1_sites;

    // Backdrop screen-X squash ([widescreen.backdrop] x_sites). The parallax
    // 2D backdrop layer (ocean/cloud/mountain/grass — overlay actor handlers)
    // computes screenX = (worldX - camX) >> parallax in pure integer math and
    // stores it to the object's screen-X field WITHOUT the GTE, so the GTE
    // X-squash that gives 3D the wider 16:9 FOV never reaches it; far pieces
    // sit past the 320px edge and are clipped (the edge "void"/pop-in). Each
    // x_site is a `sh rt,off(base)` storing the FINAL screenX; we emit it as
    // `write_half(base+off, psx_ws_backdrop_x(rt))` so the value is squashed
    // around screen centre (identity at 4:3). These addresses live in OVERLAY
    // code, so the overlay compile must see this config (--ws-config). A regen
    // is required; empty by default.
    std::vector<uint32_t> ws_backdrop_x_sites;

    // [widescreen.backdrop] unsquash_funcs — far-backdrop driver functions whose
    // body is bracketed with gte_ws_set_suppress(1)/(0) so the GTE X-squash is
    // OFF for their (far, parallax) draws: the backdrop fills the stretched 16:9
    // frame instead of leaving edge void (8C). Main-EXE addresses; regen-class.
    std::vector<uint32_t> ws_backdrop_unsquash_funcs;

    // [widescreen.cull] auto_screen_x — automatic horizontal-FOV cull widening.
    // GTE-projected render funnels reject a primitive when ALL its vertices fall
    // off the 4:3 frame: a per-vertex `sltiu vN, SX, 0x140` (right edge) paired
    // with `sltiu vN, SY, 0xE0` (bottom edge) in the same function. When this is
    // true the recompiler auto-detects that signature and emits every width
    // compare (0x140 / inclusive 0x141) with + 2*psx_ws_x_margin(), so the
    // wider 16:9 field of view is submitted instead of culled at 320 — no
    // per-site address list needed. 0 at 4:3 ⇒ byte-identical. Off by default;
    // a regen is required. (Vertical 0xE0 bound is left untouched.)
    bool ws_auto_screen_x_cull = false;
};

// UserSettings — the launcher-written, user-editable override layer.
//
// Lives in a `settings.toml` next to the runtime exe (NOT in the repo). It is
// layered on top of the bundled game.toml at startup: any field present here
// overrides the corresponding game.toml value, and the command line overrides
// both. Absent fields fall through to game.toml. The launcher seeds this file
// from game.toml defaults the first time it writes.
//
// Unlike game.toml, paths here are stored verbatim (the user picked them); they
// are NOT resolved against a project root.
struct UserSettings {
    // [video]
    bool has_renderer       = false; int  renderer       = 0; // 0=software,1=opengl
    bool has_supersampling  = false; int  supersampling  = 1; // 1..4
    // Window size: width in px; height is always width*3/4 (PSX 4:3). Applies to
    // both the launcher and the emulator window so they boot at the same size.
    bool has_window_width   = false; int  window_width   = 1280; // -> 1280x960
    bool has_antialiasing   = false; bool antialiasing   = true;
    bool has_texture_filter = false; int  texture_filter = 0; // 0=nearest,1=bilinear
    bool has_screen_kind    = false; int  screen_kind    = 0; // 0..3 (ScreenKind)
    bool has_auto_skip_fmv  = false; bool auto_skip_fmv  = false; // skip FMVs
    // [launcher] — when true, boot straight into the game and skip the GUI
    // launcher window (mirrors snesrecomp's SkipLauncher). Overridable per-run:
    // `--launcher` forces the GUI back on; `PSX_NO_LAUNCHER=1` forces it off.
    bool has_skip_launcher  = false; bool skip_launcher  = false;
    bool has_aspect_ratio   = false; int  aspect_num     = 4; // display aspect W:H
                                     int  aspect_den     = 3; // (4:3 = native)
    // [audio]
    bool has_spu_hq         = false; bool spu_hq         = false;
    // [bios] / [disc] / [memcard]
    bool has_bios_path      = false; std::filesystem::path bios_path;
    bool has_disc_path      = false; std::filesystem::path disc_path;
    bool has_memcard_dir    = false; std::filesystem::path memcard_dir;
    // Per-slot memory-card overrides. An explicit card path overrides the
    // <dir>/card<N>.mcd default; the enable flag inserts/removes the card.
    bool has_memcard1_path    = false; std::filesystem::path memcard1_path;
    bool has_memcard2_path    = false; std::filesystem::path memcard2_path;
    bool has_memcard1_enabled = false; bool memcard1_enabled = true;
    bool has_memcard2_enabled = false; bool memcard2_enabled = true;

    // [controller] — per-player input device + pad type. device is one of:
    //   "none"     — no pad in this port (port not connected)
    //   "keyboard" — driven by the keyboard map (input.ini)
    //   "<GUID>"   — an SDL game-controller GUID (SDL_JoystickGetGUIDString)
    // analog selects the emulated pad type: false = digital (0x41), true =
    // DualShock/analog (0x73). Defaults: P1 keyboard/digital, P2 none/digital.
    bool has_p1_device = false; std::string p1_device = "keyboard";
    bool has_p2_device = false; std::string p2_device = "none";
    bool has_p1_analog = false; bool p1_analog = false;
    bool has_p2_analog = false; bool p2_analog = false;
    // Analog-stick deadzone, raw SDL axis units (0..32767). The launcher edits
    // this as 0-100% (raw = pct*32767/100), mirroring snesrecomp's GamepadDeadzone.
    bool has_deadzone  = false; int  deadzone  = 12000;
};

// GameOptions — the game's OWN native OPTION-screen settings, declared in a
// dedicated, self-contained `game_options.toml` next to game.toml. Kept entirely
// separate from GameConfig (recompiler/aftermarket config) and UserSettings
// (launcher overrides): these describe the GAME's settings, persisted across
// launches because some titles keep them only in a per-boot RAM global with no
// memory-card config block (issue #5). See game_options.{h,c} for the runtime.
struct GameOption {
    std::string name;          // key written to the saved-values state file
    uint32_t    addr = 0;      // guest RAM address of the canonical config global
    int         size = 1;      // 1 or 2 bytes
    uint32_t    init_store_pc = 0; // PC of the boot-init sb/sh that writes this
                                   // global's default; the recompiler rewrites it
                                   // to apply the persisted value (restore-at-init).
};
struct GameOptions {
    std::vector<GameOption> options;     // empty => feature off for this game
};

// Load game_options.toml ([[option]] array of {name, addr, size}). Returns an
// empty GameOptions if the file is missing/unreadable; throws on a malformed
// declared entry (bad addr / size) so a typo is surfaced, not silently dropped.
GameOptions load_game_options(const std::filesystem::path& path);

// Load settings.toml. Returns an all-defaults (all has_*=false) struct if the
// file is missing or unreadable. Malformed values are skipped (best-effort:
// the launcher must still be able to start so the user can fix them), so this
// never throws.
UserSettings load_user_settings(const std::filesystem::path& path);

// Write settings.toml deterministically. Returns false on I/O failure.
bool save_user_settings(const std::filesystem::path& path, const UserSettings& s);

// Locate the project root by walking upward from `config_path` until a
// directory containing `.gitignore`, `.git`, or `CMakeLists.txt` is found.
// Throws std::runtime_error if not found within 8 levels.
std::filesystem::path find_project_root(const std::filesystem::path& config_path);

// Load a BIOS config TOML. Throws std::runtime_error on schema violations.
BiosConfig load_bios_config(const std::filesystem::path& config_path);

// Load a game config TOML. Throws std::runtime_error on schema violations.
GameConfig load_game_config(const std::filesystem::path& config_path);

} // namespace PSXRecompV4
