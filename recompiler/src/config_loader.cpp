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

// Parse a display aspect "W:H" string (e.g. "4:3", "16:9", "21:9"). Accepts
// only aspects between native 4:3 and 32:9 — narrower than 4:3 has no
// widescreen meaning and ultra-extreme values would squash the GTE X axis
// into unusability. Returns false on malformed input or out-of-range aspect.
static bool parse_aspect_ratio(const std::string& s, int* num, int* den) {
    const size_t colon = s.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size())
        return false;
    int n = 0, d = 0;
    try {
        size_t used = 0;
        n = std::stoi(s.substr(0, colon), &used);
        if (used != colon) return false;
        const std::string rest = s.substr(colon + 1);
        d = std::stoi(rest, &used);
        if (used != rest.size()) return false;
    } catch (const std::exception&) {
        return false;
    }
    if (n <= 0 || d <= 0 || n > 99 || d > 99) return false;
    if (3 * n < 4 * d) return false;   // narrower than 4:3
    if (9 * n > 32 * d) return false;  // wider than 32:9
    *num = n; *den = d;
    return true;
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
    if (runtime.contains("overlay_autocompile_cmd")) {
        rt.overlay_autocompile_cmd =
            toml::find<std::string>(runtime, "overlay_autocompile_cmd");
        rt.has_overlay_autocompile_cmd = !rt.overlay_autocompile_cmd.empty();
    }

    // Optional [video] block — visual enhancement options. Kept on the same
    // RuntimeConfig so main.cpp consumes them alongside the other knobs.
    if (cfg.contains("video")) {
        const toml::value& video = toml::find(cfg, "video");
        if (video.contains("supersampling")) {
            const auto n = toml::find<int64_t>(video, "supersampling");
            if (n < 1 || n > 4) {
                throw std::runtime_error(fmt::format(
                    "[video] supersampling out of range (1..4): {}", n));
            }
            rt.video_supersampling = static_cast<int>(n);
        }
        if (video.contains("antialiasing")) {
            rt.video_antialiasing = toml::find<bool>(video, "antialiasing");
        }
        if (video.contains("texture_filtering")) {
            const auto mode = toml::find<std::string>(video, "texture_filtering");
            if (mode == "nearest")       rt.video_texture_filter = 0;
            else if (mode == "bilinear") rt.video_texture_filter = 1;
            else throw std::runtime_error(fmt::format(
                "[video] texture_filtering must be \"nearest\" or \"bilinear\": {}", mode));
        }
        if (video.contains("renderer")) {
            const auto mode = toml::find<std::string>(video, "renderer");
            if (mode == "software")     rt.video_renderer = 0;
            else if (mode == "opengl")  rt.video_renderer = 1;
            else throw std::runtime_error(fmt::format(
                "[video] renderer must be \"software\" or \"opengl\": {}", mode));
        }
        if (video.contains("crt_filter")) {
            const auto mode = toml::find<std::string>(video, "crt_filter");
            if      (mode == "raw")       rt.video_screen_kind = 0;
            else if (mode == "crt")       rt.video_screen_kind = 1;
            else if (mode == "composite") rt.video_screen_kind = 2;
            else if (mode == "trinitron") rt.video_screen_kind = 3;
            else throw std::runtime_error(fmt::format(
                "[video] crt_filter must be \"raw\"|\"crt\"|\"composite\"|\"trinitron\": {}",
                mode));
        }
        if (video.contains("auto_skip_fmv")) {
            rt.video_auto_skip_fmv = toml::find<bool>(video, "auto_skip_fmv");
        }
        if (video.contains("aspect_ratio")) {
            const auto mode = toml::find<std::string>(video, "aspect_ratio");
            int n = 0, d = 0;
            if (!parse_aspect_ratio(mode, &n, &d))
                throw std::runtime_error(fmt::format(
                    "[video] aspect_ratio must be \"W:H\", no narrower than 4:3 "
                    "and no wider than 32:9 (e.g. \"4:3\", \"16:9\"): {}", mode));
            rt.video_aspect_num = n;
            rt.video_aspect_den = d;
        }
    }

    // Optional [audio] block.
    if (cfg.contains("audio")) {
        const toml::value& audio = toml::find(cfg, "audio");
        if (audio.contains("spu_hq")) {
            rt.audio_spu_hq = toml::find<bool>(audio, "spu_hq");
        }
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

    // Optional expected disc identity (launcher verification badge).
    bool has_disc_crc = false;
    uint32_t disc_crc = 0;
    std::string disc_sha1;
    if (game.contains("disc_crc")) {
        disc_crc = parse_hex(toml::find<std::string>(game, "disc_crc"), "game.disc_crc");
        has_disc_crc = true;
    }
    if (game.contains("disc_sha1")) {
        disc_sha1 = toml::find<std::string>(game, "disc_sha1");
        // normalize to lowercase hex
        for (char& c : disc_sha1) c = (char)std::tolower((unsigned char)c);
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
        /*has_disc_crc*/     has_disc_crc,
        /*disc_crc*/         disc_crc,
        /*disc_sha1*/        disc_sha1,
        /*seeds_path*/       seeds_path,
        /*bios_thunks_path*/ bios_thunks_path,
        /*out_dir*/          out_dir,
        /*strict*/           strict,
        /*out_stem*/         out_stem,
        /*runtime*/          parse_runtime_block(cfg, root),
    };
}

// ---- UserSettings (settings.toml) — launcher-written override layer ----

UserSettings load_user_settings(const fs::path& path) {
    UserSettings s;
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec)) return s;

    toml::value doc;
    try {
        doc = toml::parse(path.string());
    } catch (const std::exception&) {
        // Malformed file: fall back to all-defaults rather than refuse to boot.
        return s;
    }

    // Each field guarded independently so one bad value can't blank the rest.
    auto try_get = [](auto&& fn) { try { fn(); } catch (const std::exception&) {} };

    if (doc.contains("video")) {
        const toml::value& v = toml::find(doc, "video");
        if (v.contains("renderer")) try_get([&]{
            const auto m = toml::find<std::string>(v, "renderer");
            if (m == "software") { s.renderer = 0; s.has_renderer = true; }
            else if (m == "opengl") { s.renderer = 1; s.has_renderer = true; }
        });
        if (v.contains("supersampling")) try_get([&]{
            const auto n = toml::find<int64_t>(v, "supersampling");
            if (n >= 1 && n <= 4) { s.supersampling = (int)n; s.has_supersampling = true; }
        });
        if (v.contains("window_width")) try_get([&]{
            const auto n = toml::find<int64_t>(v, "window_width");
            if (n >= 640 && n <= 3840) { s.window_width = (int)n; s.has_window_width = true; }
        });
        if (v.contains("antialiasing")) try_get([&]{
            s.antialiasing = toml::find<bool>(v, "antialiasing"); s.has_antialiasing = true;
        });
        if (v.contains("texture_filtering")) try_get([&]{
            const auto m = toml::find<std::string>(v, "texture_filtering");
            if (m == "nearest") { s.texture_filter = 0; s.has_texture_filter = true; }
            else if (m == "bilinear") { s.texture_filter = 1; s.has_texture_filter = true; }
        });
        if (v.contains("crt_filter")) try_get([&]{
            const auto m = toml::find<std::string>(v, "crt_filter");
            if (m == "raw")            { s.screen_kind = 0; s.has_screen_kind = true; }
            else if (m == "crt")       { s.screen_kind = 1; s.has_screen_kind = true; }
            else if (m == "composite") { s.screen_kind = 2; s.has_screen_kind = true; }
            else if (m == "trinitron") { s.screen_kind = 3; s.has_screen_kind = true; }
        });
        if (v.contains("auto_skip_fmv")) try_get([&]{
            s.auto_skip_fmv = toml::find<bool>(v, "auto_skip_fmv"); s.has_auto_skip_fmv = true;
        });
        if (v.contains("aspect_ratio")) try_get([&]{
            const auto m = toml::find<std::string>(v, "aspect_ratio");
            int n = 0, d = 0;
            if (parse_aspect_ratio(m, &n, &d)) {
                s.aspect_num = n; s.aspect_den = d; s.has_aspect_ratio = true;
            }
        });
    }
    if (doc.contains("audio")) {
        const toml::value& a = toml::find(doc, "audio");
        if (a.contains("spu_hq")) try_get([&]{
            s.spu_hq = toml::find<bool>(a, "spu_hq"); s.has_spu_hq = true;
        });
    }
    if (doc.contains("launcher")) {
        const toml::value& l = toml::find(doc, "launcher");
        if (l.contains("skip_launcher")) try_get([&]{
            s.skip_launcher = toml::find<bool>(l, "skip_launcher"); s.has_skip_launcher = true;
        });
    }
    if (doc.contains("bios")) {
        const toml::value& b = toml::find(doc, "bios");
        if (b.contains("path")) try_get([&]{
            const auto p = toml::find<std::string>(b, "path");
            if (!p.empty()) { s.bios_path = fs::path(p); s.has_bios_path = true; }
        });
    }
    if (doc.contains("disc")) {
        const toml::value& d = toml::find(doc, "disc");
        if (d.contains("path")) try_get([&]{
            const auto p = toml::find<std::string>(d, "path");
            if (!p.empty()) { s.disc_path = fs::path(p); s.has_disc_path = true; }
        });
    }
    if (doc.contains("memcard")) {
        const toml::value& m = toml::find(doc, "memcard");
        if (m.contains("dir")) try_get([&]{
            const auto p = toml::find<std::string>(m, "dir");
            if (!p.empty()) { s.memcard_dir = fs::path(p); s.has_memcard_dir = true; }
        });
        if (m.contains("card1")) try_get([&]{
            const auto p = toml::find<std::string>(m, "card1");
            if (!p.empty()) { s.memcard1_path = fs::path(p); s.has_memcard1_path = true; }
        });
        if (m.contains("card2")) try_get([&]{
            const auto p = toml::find<std::string>(m, "card2");
            if (!p.empty()) { s.memcard2_path = fs::path(p); s.has_memcard2_path = true; }
        });
        if (m.contains("enable1")) try_get([&]{
            s.memcard1_enabled = toml::find<bool>(m, "enable1"); s.has_memcard1_enabled = true;
        });
        if (m.contains("enable2")) try_get([&]{
            s.memcard2_enabled = toml::find<bool>(m, "enable2"); s.has_memcard2_enabled = true;
        });
    }
    if (doc.contains("controller")) {
        const toml::value& ct = toml::find(doc, "controller");
        if (ct.contains("p1_device")) try_get([&]{
            const auto v = toml::find<std::string>(ct, "p1_device");
            if (!v.empty()) { s.p1_device = v; s.has_p1_device = true; }
        });
        if (ct.contains("p2_device")) try_get([&]{
            const auto v = toml::find<std::string>(ct, "p2_device");
            if (!v.empty()) { s.p2_device = v; s.has_p2_device = true; }
        });
        if (ct.contains("p1_analog")) try_get([&]{
            s.p1_analog = toml::find<bool>(ct, "p1_analog"); s.has_p1_analog = true;
        });
        if (ct.contains("p2_analog")) try_get([&]{
            s.p2_analog = toml::find<bool>(ct, "p2_analog"); s.has_p2_analog = true;
        });
    }
    return s;
}

bool save_user_settings(const fs::path& path, const UserSettings& s) {
    std::error_code ec;
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path(), ec);  // best-effort

    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;

    // TOML strings use forward slashes so backslash escaping is never an issue.
    auto fwd = [](const fs::path& p) {
        std::string str = p.generic_string();
        return str;
    };

    f << "# psxrecomp user settings - written by the launcher. Safe to hand-edit.\n";
    f << "# Overrides the bundled game.toml; the command line overrides this file.\n\n";

    f << "[video]\n";
    if (s.has_renderer)
        f << "renderer          = \"" << (s.renderer ? "opengl" : "software") << "\"\n";
    if (s.has_supersampling)
        f << "supersampling     = " << s.supersampling << "\n";
    if (s.has_window_width)
        f << "window_width      = " << s.window_width << "\n";
    if (s.has_antialiasing)
        f << "antialiasing      = " << (s.antialiasing ? "true" : "false") << "\n";
    if (s.has_texture_filter)
        f << "texture_filtering = \"" << (s.texture_filter ? "bilinear" : "nearest") << "\"\n";
    if (s.has_screen_kind) {
        const char* k = s.screen_kind == 1 ? "crt"
                      : s.screen_kind == 2 ? "composite"
                      : s.screen_kind == 3 ? "trinitron" : "raw";
        f << "crt_filter        = \"" << k << "\"\n";
    }
    if (s.has_auto_skip_fmv)
        f << "auto_skip_fmv     = " << (s.auto_skip_fmv ? "true" : "false") << "\n";
    if (s.has_aspect_ratio)
        f << "aspect_ratio      = \"" << s.aspect_num << ":" << s.aspect_den << "\"\n";
    f << "\n[audio]\n";
    if (s.has_spu_hq)
        f << "spu_hq = " << (s.spu_hq ? "true" : "false") << "\n";
    if (s.has_skip_launcher)
        f << "\n[launcher]\nskip_launcher = " << (s.skip_launcher ? "true" : "false") << "\n";
    if (s.has_bios_path)
        f << "\n[bios]\npath = \"" << fwd(s.bios_path) << "\"\n";
    if (s.has_disc_path)
        f << "\n[disc]\npath = \"" << fwd(s.disc_path) << "\"\n";
    if (s.has_memcard_dir || s.has_memcard1_path || s.has_memcard2_path ||
        s.has_memcard1_enabled || s.has_memcard2_enabled) {
        f << "\n[memcard]\n";
        if (s.has_memcard_dir)
            f << "dir     = \"" << fwd(s.memcard_dir) << "\"\n";
        if (s.has_memcard1_path)
            f << "card1   = \"" << fwd(s.memcard1_path) << "\"\n";
        if (s.has_memcard2_path)
            f << "card2   = \"" << fwd(s.memcard2_path) << "\"\n";
        if (s.has_memcard1_enabled)
            f << "enable1 = " << (s.memcard1_enabled ? "true" : "false") << "\n";
        if (s.has_memcard2_enabled)
            f << "enable2 = " << (s.memcard2_enabled ? "true" : "false") << "\n";
    }

    if (s.has_p1_device || s.has_p2_device || s.has_p1_analog || s.has_p2_analog) {
        f << "\n[controller]\n";
        if (s.has_p1_device)
            f << "p1_device = \"" << s.p1_device << "\"\n";
        if (s.has_p1_analog)
            f << "p1_analog = " << (s.p1_analog ? "true" : "false") << "\n";
        if (s.has_p2_device)
            f << "p2_device = \"" << s.p2_device << "\"\n";
        if (s.has_p2_analog)
            f << "p2_analog = " << (s.p2_analog ? "true" : "false") << "\n";
    }

    return f.good();
}

} // namespace PSXRecompV4
