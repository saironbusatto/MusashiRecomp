// config_loader.cpp — see config_loader.h for the contract.

#include "config_loader.h"

#include <cctype>
#include <fstream>
#include <stdexcept>
#include <string>

#include "fmt/format.h"

// toml11 is header-only.
#define TOML11_USE_UNRELEASED_TOML_FEATURES
#include "toml.hpp"

namespace PSXRecompV4 {

namespace fs = std::filesystem;

// Parse a hex string ("0x...") to uint32_t. Throws on malformed input.
static uint32_t parse_hex(const std::string& s, const std::string& field) {
    try {
        return static_cast<uint32_t>(std::stoul(s, nullptr, 16));
    } catch (const std::exception& ex) {
        throw std::runtime_error(
            fmt::format("config field '{}': expected hex string, got '{}' ({})",
                        field, s, ex.what()));
    }
}

// Parse the optional [runtime] block. All fields optional; absent fields
// leave has_* = false on the returned struct. Paths are resolved relative
// to `root` (project root).
static RuntimeConfig parse_runtime_block(const toml::value& cfg, const fs::path& root) {
    RuntimeConfig rt;
    if (!cfg.contains("runtime")) return rt;
    const toml::value& runtime = toml::find(cfg, "runtime");

    if (runtime.contains("debug_port")) {
        const auto port = toml::find<int64_t>(runtime, "debug_port");
        if (port < 0 || port > 65535) {
            throw std::runtime_error(
                fmt::format("[runtime] debug_port out of range (0..65535): {}", port));
        }
        rt.debug_port = static_cast<uint16_t>(port);
        rt.has_debug_port = true;
    }
    if (runtime.contains("window_title")) {
        rt.window_title = toml::find<std::string>(runtime, "window_title");
        rt.has_window_title = true;
    }
    if (runtime.contains("memcard_dir")) {
        const auto rel = toml::find<std::string>(runtime, "memcard_dir");
        rt.memcard_dir = fs::absolute(root / rel);
        rt.has_memcard_dir = true;
    }
    if (runtime.contains("disc_speed")) {
        rt.disc_speed     = toml::find<std::string>(runtime, "disc_speed");
        rt.has_disc_speed = true;
    }
    if (runtime.contains("instant_max_per_frame")) {
        const auto n = toml::find<int64_t>(runtime, "instant_max_per_frame");
        if (n < 1 || n > 4096) {
            throw std::runtime_error(fmt::format(
                "[runtime] instant_max_per_frame out of range (1..4096): {}", n));
        }
        rt.instant_max_per_frame     = static_cast<int>(n);
        rt.has_instant_max_per_frame = true;
    }
    if (runtime.contains("fast_boot")) {
        rt.fast_boot = toml::find<bool>(runtime, "fast_boot");
    }
    if (runtime.contains("overlay_cache")) {
        rt.overlay_cache = toml::find<bool>(runtime, "overlay_cache");
    }
    if (runtime.contains("turbo_loads")) {
        rt.turbo_loads = toml::find<bool>(runtime, "turbo_loads");
    }
    return rt;
}

fs::path find_project_root(const fs::path& config_path) {
    fs::path cur = fs::absolute(config_path).parent_path();
    const fs::path fallback = cur;
    for (int i = 0; i < 8; ++i) {
        for (const char* marker : { ".gitignore", ".git", "CMakeLists.txt" }) {
            if (fs::exists(cur / marker)) {
                return cur;
            }
        }
        const fs::path parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return fallback;
}

// Derive the output filename stem from a rom basename. Mirrors the Python
// audit_config.py logic: strip a trailing .BIN/.EXE (case-insensitive) but
// preserve dotted names like "SCUS_942.36" unchanged.
static std::string derive_out_stem(const std::string& rom_basename) {
    auto ends_with_ci = [](const std::string& s, const std::string& suffix) {
        if (s.size() < suffix.size()) return false;
        for (size_t i = 0; i < suffix.size(); ++i) {
            char a = s[s.size() - suffix.size() + i];
            char b = suffix[i];
            if (std::tolower(static_cast<unsigned char>(a)) !=
                std::tolower(static_cast<unsigned char>(b))) return false;
        }
        return true;
    };
    if (ends_with_ci(rom_basename, ".bin") || ends_with_ci(rom_basename, ".exe")) {
        return rom_basename.substr(0, rom_basename.size() - 4);
    }
    return rom_basename;
}

BiosConfig load_bios_config(const fs::path& config_path_in) {
    const fs::path config_path = fs::absolute(config_path_in);
    if (!fs::exists(config_path)) {
        throw std::runtime_error(
            fmt::format("config file not found: {}", config_path.string()));
    }

    const fs::path root = find_project_root(config_path);

    toml::value cfg;
    try {
        cfg = toml::parse(config_path);
    } catch (const toml::syntax_error& ex) {
        throw std::runtime_error(
            fmt::format("TOML syntax error in {}: {}", config_path.string(), ex.what()));
    }

    // [program] — bios.toml uses this; some legacy files use [game]
    const toml::value* prog_ptr = nullptr;
    if (cfg.contains("program")) {
        prog_ptr = &toml::find(cfg, "program");
    } else if (cfg.contains("game")) {
        prog_ptr = &toml::find(cfg, "game");
    } else {
        throw std::runtime_error(
            fmt::format("{}: missing [program] (or [game]) block",
                        config_path.string()));
    }
    const toml::value& prog = *prog_ptr;

    const std::string name = toml::find<std::string>(prog, "name");
    const std::string id   = prog.contains("id")
                                ? toml::find<std::string>(prog, "id")
                                : std::string{};

    // BIOS uses `rom`; game files use `exe`. Either is accepted here, but
    // BIOS callers should pass a bios config that has `rom`.
    std::string rom_field;
    if (prog.contains("rom")) {
        rom_field = toml::find<std::string>(prog, "rom");
    } else if (prog.contains("exe")) {
        rom_field = toml::find<std::string>(prog, "exe");
    } else {
        throw std::runtime_error(
            fmt::format("{}: [program] missing 'rom' or 'exe' field",
                        config_path.string()));
    }
    const fs::path rom_path = fs::absolute(root / rom_field);

    const uint32_t load_address =
        parse_hex(toml::find<std::string>(prog, "load_address"), "program.load_address");
    const uint32_t entry_pc =
        prog.contains("entry_pc")
            ? parse_hex(toml::find<std::string>(prog, "entry_pc"), "program.entry_pc")
            : load_address;
    const uint32_t text_size =
        parse_hex(toml::find<std::string>(prog, "text_size"), "program.text_size");

    // [recompiler]
    if (!cfg.contains("recompiler")) {
        throw std::runtime_error(
            fmt::format("{}: missing [recompiler] block", config_path.string()));
    }
    const toml::value& recomp = toml::find(cfg, "recompiler");

    if (!recomp.contains("seeds")) {
        throw std::runtime_error(
            fmt::format("{}: [recompiler] missing 'seeds' field", config_path.string()));
    }
    const std::string seeds_field = toml::find<std::string>(recomp, "seeds");
    const fs::path seeds_path = fs::absolute(root / seeds_field);

    const std::string out_dir_field =
        recomp.contains("out_dir")
            ? toml::find<std::string>(recomp, "out_dir")
            : std::string{"generated"};
    const fs::path out_dir = fs::absolute(root / out_dir_field);

    const bool strict = recomp.contains("strict")
                            ? toml::find<bool>(recomp, "strict")
                            : true;

    std::string out_stem;
    if (recomp.contains("out_stem")) {
        out_stem = toml::find<std::string>(recomp, "out_stem");
    } else {
        out_stem = derive_out_stem(fs::path(rom_field).filename().string());
    }

    // [[recompiler.bios_vectors]] — optional array of vector dispatch tables
    std::vector<BiosVectorTable> bios_vectors;
    if (recomp.contains("bios_vectors")) {
        const auto& arr = recomp.at("bios_vectors").as_array();
        for (const auto& v : arr) {
            BiosVectorTable bvt;
            bvt.ram_addr = parse_hex(
                toml::find<std::string>(v, "ram_addr"), "bios_vectors.ram_addr");
            bvt.index_reg = static_cast<int>(
                toml::find<int64_t>(v, "index_reg"));
            bvt.table_rom_addr = parse_hex(
                toml::find<std::string>(v, "table_rom_addr"), "bios_vectors.table_rom_addr");
            bvt.table_count = static_cast<uint32_t>(
                toml::find<int64_t>(v, "table_count"));
            bvt.table_ram_addr = v.contains("table_ram_addr")
                ? parse_hex(toml::find<std::string>(v, "table_ram_addr"),
                            "bios_vectors.table_ram_addr")
                : 0u;
            bios_vectors.push_back(bvt);
        }
    }

    // [[recompiler.bios_aliases]] — fixed-target RAM trampolines
    std::vector<BiosAlias> bios_aliases;
    if (recomp.contains("bios_aliases")) {
        const auto& arr = recomp.at("bios_aliases").as_array();
        for (const auto& v : arr) {
            BiosAlias ba;
            ba.ram_addr   = parse_hex(toml::find<std::string>(v, "ram_addr"),
                                      "bios_aliases.ram_addr");
            ba.target_key = parse_hex(toml::find<std::string>(v, "target_key"),
                                      "bios_aliases.target_key");
            bios_aliases.push_back(ba);
        }
    }

    return BiosConfig{
        /*config_path*/  config_path,
        /*project_root*/ root,
        /*name*/         name,
        /*id*/           id,
        /*rom_path*/     rom_path,
        /*load_address*/ load_address,
        /*entry_pc*/     entry_pc,
        /*text_size*/    text_size,
        /*seeds_path*/   seeds_path,
        /*out_dir*/      out_dir,
        /*strict*/       strict,
        /*out_stem*/     out_stem,
        /*bios_vectors*/ std::move(bios_vectors),
        /*bios_aliases*/ std::move(bios_aliases),
        /*runtime*/      parse_runtime_block(cfg, root),
    };
}

GameConfig load_game_config(const fs::path& config_path_in) {
    const fs::path config_path = fs::absolute(config_path_in);
    if (!fs::exists(config_path)) {
        throw std::runtime_error(
            fmt::format("game config not found: {}", config_path.string()));
    }
    const fs::path root = find_project_root(config_path);

    toml::value cfg;
    try {
        cfg = toml::parse(config_path);
    } catch (const toml::syntax_error& ex) {
        throw std::runtime_error(
            fmt::format("TOML syntax error in {}: {}", config_path.string(), ex.what()));
    }

    // [game] (preferred for game configs) or [program]
    const toml::value* prog_ptr = nullptr;
    if (cfg.contains("game")) {
        prog_ptr = &toml::find(cfg, "game");
    } else if (cfg.contains("program")) {
        prog_ptr = &toml::find(cfg, "program");
    } else {
        throw std::runtime_error(
            fmt::format("{}: missing [game] (or [program]) block",
                        config_path.string()));
    }
    const toml::value& game = *prog_ptr;

    const std::string name = toml::find<std::string>(game, "name");
    const std::string id   = game.contains("id")
                                ? toml::find<std::string>(game, "id")
                                : std::string{};

    std::string exe_field;
    if (game.contains("exe")) {
        exe_field = toml::find<std::string>(game, "exe");
    } else if (game.contains("rom")) {
        exe_field = toml::find<std::string>(game, "rom");
    } else {
        throw std::runtime_error(
            fmt::format("{}: [game] missing 'exe' or 'rom' field", config_path.string()));
    }
    const fs::path exe_path = fs::absolute(root / exe_field);

    const uint32_t load_address =
        parse_hex(toml::find<std::string>(game, "load_address"), "game.load_address");
    const uint32_t entry_pc =
        game.contains("entry_pc")
            ? parse_hex(toml::find<std::string>(game, "entry_pc"), "game.entry_pc")
            : load_address;
    const uint32_t text_size =
        parse_hex(toml::find<std::string>(game, "text_size"), "game.text_size");
    const uint32_t stack_base =
        game.contains("stack_base")
            ? parse_hex(toml::find<std::string>(game, "stack_base"), "game.stack_base")
            : 0x801FFFF0u;

    // Disc paths: accept either single `disc` or array `discs`.
    std::vector<fs::path> discs;
    if (game.contains("discs")) {
        const auto& arr = toml::find<std::vector<std::string>>(game, "discs");
        for (const auto& d : arr) discs.push_back(fs::absolute(root / d));
    } else if (game.contains("disc")) {
        const auto& d = toml::find<std::string>(game, "disc");
        discs.push_back(fs::absolute(root / d));
    }

    // [recompiler]
    if (!cfg.contains("recompiler")) {
        throw std::runtime_error(
            fmt::format("{}: missing [recompiler] block", config_path.string()));
    }
    const toml::value& recomp = toml::find(cfg, "recompiler");

    if (!recomp.contains("seeds")) {
        throw std::runtime_error(
            fmt::format("{}: [recompiler] missing 'seeds' field", config_path.string()));
    }
    const fs::path seeds_path =
        fs::absolute(root / toml::find<std::string>(recomp, "seeds"));

    fs::path bios_thunks_path;
    if (recomp.contains("bios_thunks")) {
        bios_thunks_path =
            fs::absolute(root / toml::find<std::string>(recomp, "bios_thunks"));
    }

    const std::string out_dir_field =
        recomp.contains("out_dir")
            ? toml::find<std::string>(recomp, "out_dir")
            : std::string{"generated"};
    const fs::path out_dir = fs::absolute(root / out_dir_field);

    const bool strict = recomp.contains("strict")
                            ? toml::find<bool>(recomp, "strict")
                            : true;

    std::string out_stem;
    if (recomp.contains("out_stem")) {
        out_stem = toml::find<std::string>(recomp, "out_stem");
    } else {
        out_stem = derive_out_stem(fs::path(exe_field).filename().string());
    }

    return GameConfig{
        /*config_path*/      config_path,
        /*project_root*/     root,
        /*name*/             name,
        /*id*/               id,
        /*exe_path*/         exe_path,
        /*load_address*/     load_address,
        /*entry_pc*/         entry_pc,
        /*text_size*/        text_size,
        /*stack_base*/       stack_base,
        /*discs*/            discs,
        /*seeds_path*/       seeds_path,
        /*bios_thunks_path*/ bios_thunks_path,
        /*out_dir*/          out_dir,
        /*strict*/           strict,
        /*out_stem*/         out_stem,
        /*runtime*/          parse_runtime_block(cfg, root),
    };
}

} // namespace PSXRecompV4
