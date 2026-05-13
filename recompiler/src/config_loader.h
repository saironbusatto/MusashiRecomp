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
