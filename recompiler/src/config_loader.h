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

    // [recompiler] block
    std::filesystem::path seeds_path;     // absolute path to seeds (text or json)
    std::filesystem::path bios_thunks_path; // optional; empty if not set
    std::filesystem::path out_dir;
    bool                  strict;
    std::string           out_stem;       // derived if not explicit

    // [runtime] block (optional)
    RuntimeConfig         runtime;
};

// Locate the project root by walking upward from `config_path` until a
// directory containing `.gitignore`, `.git`, or `CMakeLists.txt` is found.
// Throws std::runtime_error if not found within 8 levels.
std::filesystem::path find_project_root(const std::filesystem::path& config_path);

// Load a BIOS config TOML. Throws std::runtime_error on schema violations.
BiosConfig load_bios_config(const std::filesystem::path& config_path);

// Load a game config TOML. Throws std::runtime_error on schema violations.
GameConfig load_game_config(const std::filesystem::path& config_path);

} // namespace PSXRecompV4
