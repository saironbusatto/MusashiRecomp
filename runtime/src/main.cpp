/* main.cpp — Phase 3 runtime entry point.
 *
 * Loads BIOS ROM, initializes CPU state + SDL display, calls into
 * the recompiled reset vector. BIOS drives execution; SDL presents
 * VRAM at each vblank via callback from gpu_vblank_tick().
 */

#include "cpu_state.h"
#include "psx_interpreter.h"
#include "cdrom.h"
#include "fntrace.h"
#include "boot_state.h"
#include "overlay_capture.h"
#include "overlay_loader.h"
#include "autocompile.h"
#include "gpu.h"
#include "gpu_sw_renderer.h"
#include "gpu_render.h"
#include "gpu_gl_renderer.h"
#include "frame_pacing.h"
#include "sio.h"
#include "spu.h"
#include "memcard.h"
#include "debug_server.h"
#include "crash_trace.h"
#include "freeze_heartbeat.h"
#include "config_loader.h"
#include "crc32.h"
#include <SDL.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#endif

#ifndef PSX_DEFAULT_BIOS_PATH
#define PSX_DEFAULT_BIOS_PATH "bios/SCPH1001.BIN"
#endif
#ifndef PSX_DEFAULT_GAME_CONFIG_PATH
#define PSX_DEFAULT_GAME_CONFIG_PATH ""
#endif
#ifndef PSX_WINDOW_TITLE
#define PSX_WINDOW_TITLE "psxrecomp"
#endif
#ifndef DEFAULT_DEBUG_PORT
#error DEFAULT_DEBUG_PORT must be defined by the runtime target.
#endif

extern "C" uint64_t gte_get_exec_count(void);

/* memory.c */
extern "C" void     memory_init(const char* bios_path);
extern "C" void     memory_set_sr_ptr(const uint32_t *p);
extern "C" uint32_t memory_get_bios_checksum(void);

/* dma.c */
extern "C" void dma_init(void);

/* mdec.c */
extern "C" void mdec_init(void);

/* timers.c */
extern "C" void timers_init(void);

/* interrupts.c */
extern "C" void interrupts_init(void);
extern "C" uint32_t psx_read_word(uint32_t addr);
extern "C" void     psx_write_word(uint32_t addr, uint32_t val);
extern "C" uint16_t psx_read_half(uint32_t addr);
extern "C" void     psx_write_half(uint32_t addr, uint16_t val);
extern "C" uint8_t  psx_read_byte(uint32_t addr);
extern "C" void     psx_write_byte(uint32_t addr, uint8_t val);

/* ---- SDL state ---- */
static SDL_Window*   sdl_window;
static SDL_Renderer* sdl_renderer;
static SDL_Texture*  sdl_texture;
static SDL_GameController* sdl_controller;
static SDL_JoystickID      sdl_controller_instance = -1;
/* ARGB8888 staging buffer. Sized for the active internal resolution:
 * 640*scale x 512*scale. Allocated once the supersampling scale is known
 * (sized for the native 640x512 when supersampling is off). */
static uint32_t*     sdl_pixel_buf = nullptr;
static int           s_fast_boot_active = 0;  /* cleared when game entry PC fires */

/* [video] options, resolved from the game config (defaults: native + AA). */
static int           g_video_scale = 1;     /* internal-resolution SSAA factor */
static bool          g_video_aa    = true;  /* linear present filtering */
static int           g_video_texfilter = 0; /* 0=nearest, 1=bilinear */
static int           g_video_renderer = 0;  /* 0=software, 1=opengl (requested) */
static bool          g_gl_active = false;    /* GL context live -> GL present path */

/* Vsync self-heal state (see SDL_RenderPresent wrapper in
 * sdl_vblank_present). C linkage: freeze_heartbeat.c includes both in
 * the freeze dump so a slow-frames wedge can be attributed to driver
 * present backpressure from the dump alone. */
extern "C" {
uint32_t g_present_slow_count = 0;     /* presents that blocked >250ms */
int      g_present_vsync_disabled = 0; /* 1 once self-heal tripped */
}

/* Turbo-through-loads (step 4). C linkage: debug_server.c reads/toggles the
 * enable and reports the frame counter. Enabled by game.toml [runtime]
 * turbo_loads (opt-in per game) or the turbo_loads TCP command. */
extern "C" {
int      g_turbo_loads_enabled = 0;
uint64_t g_turbo_loads_frames  = 0;   /* vblanks run unpaced (observability) */
}
static SDL_AudioDeviceID sdl_audio_device;
static int16_t       sdl_audio_buf[2048 * 2];

static std::filesystem::path find_upward(std::filesystem::path start,
                                         const std::filesystem::path& marker) {
    std::error_code ec;
    start = std::filesystem::absolute(start, ec);
    if (ec) start = std::filesystem::current_path();
    if (!std::filesystem::is_directory(start, ec)) start = start.parent_path();

    for (;;) {
        if (std::filesystem::exists(start / marker, ec)) return start;
        if (!start.has_parent_path() || start.parent_path() == start) break;
        start = start.parent_path();
    }
    return {};
}

static std::filesystem::path exe_dir_from_argv(const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path exe_dir;
    if (argv0 && argv0[0]) {
        exe_dir = fs::absolute(argv0, ec).parent_path();
        if (ec) exe_dir.clear();
    }
    if (exe_dir.empty()) exe_dir = fs::current_path();
    return exe_dir;
}

static std::filesystem::path resolve_existing_runtime_path(const char* requested,
                                                           const char* argv0) {
    namespace fs = std::filesystem;
    if (!requested || !requested[0]) return {};

    std::error_code ec;
    fs::path p(requested);
    if (fs::exists(p, ec)) return fs::absolute(p, ec);
    if (p.is_absolute()) return {};

    std::vector<fs::path> roots;
    roots.push_back(fs::current_path());
    roots.push_back(exe_dir_from_argv(argv0));
    for (const fs::path& root : roots) {
        fs::path direct = root / p;
        if (fs::exists(direct, ec)) return fs::absolute(direct, ec);

        fs::path found = find_upward(root, p);
        if (!found.empty()) return fs::absolute(found / p, ec);
    }
    return {};
}

static std::filesystem::path sidecar_cfg_path(const char* argv0, const char* filename) {
    return exe_dir_from_argv(argv0) / filename;
}

static std::filesystem::path read_cached_path(const char* argv0, const char* filename) {
    std::ifstream f(sidecar_cfg_path(argv0, filename));
    if (!f.is_open()) return {};
    std::string line;
    std::getline(f, line);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    return line.empty() ? std::filesystem::path{} : std::filesystem::path(line);
}

static void write_cached_path(const char* argv0, const char* filename,
                              const std::filesystem::path& path) {
    std::ofstream f(sidecar_cfg_path(argv0, filename), std::ios::trunc);
    if (f.is_open()) f << path.string() << "\n";
}

static void launcher_warning(const char* title, const std::string& msg) {
    std::fprintf(stderr, "%s: %s\n", title, msg.c_str());
#ifdef _WIN32
    MessageBoxA(NULL, msg.c_str(), title, MB_OK | MB_ICONWARNING);
#endif
}

static void launcher_info(const char* title, const std::string& msg) {
    std::fprintf(stderr, "%s: %s\n", title, msg.c_str());
#ifdef _WIN32
    MessageBoxA(NULL, msg.c_str(), title, MB_OK | MB_ICONINFORMATION);
#endif
}

/* Game display name for picker dialogs ("Tomba!"); set after the game
 * config loads, before any interactive file resolution. */
static std::string s_picker_game_name = "PSXRecomp";

static bool pick_runtime_file(const char* title, const char* filter,
                              std::filesystem::path& out) {
#ifdef _WIN32
    char path_buf[4096];
    std::memset(path_buf, 0, sizeof(path_buf));

    OPENFILENAMEA ofn;
    std::memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path_buf;
    ofn.nMaxFile = (DWORD)sizeof(path_buf);
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (!GetOpenFileNameA(&ofn)) return false;
    out = path_buf;
    return true;
#else
    (void)filter;
    (void)out;
    std::fprintf(stderr, "psxrecomp: %s requires a command-line path on this platform.\n", title);
    return false;
#endif
}

static std::string uppercase_ascii(std::string s) {
    for (char& c : s) {
        c = (char)std::toupper((unsigned char)c);
    }
    return s;
}

static bool ends_with_ci(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); i++) {
        char a = (char)std::toupper((unsigned char)s[s.size() - suffix.size() + i]);
        char b = (char)std::toupper((unsigned char)suffix[i]);
        if (a != b) return false;
    }
    return true;
}

static std::filesystem::path cue_data_path(const std::filesystem::path& path) {
    if (!ends_with_ci(path.string(), ".cue")) return path;

    std::ifstream cue(path);
    if (!cue.is_open()) return path;

    std::string line;
    while (std::getline(cue, line)) {
        const std::string upper = uppercase_ascii(line);
        const size_t file_pos = upper.find("FILE");
        const size_t binary_pos = upper.find("BINARY");
        if (file_pos == std::string::npos || binary_pos == std::string::npos) continue;

        size_t q1 = line.find('"', file_pos);
        size_t q2 = (q1 == std::string::npos) ? std::string::npos : line.find('"', q1 + 1);
        std::string bin_name;
        if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1 + 1) {
            bin_name = line.substr(q1 + 1, q2 - q1 - 1);
        } else {
            std::istringstream iss(line.substr(file_pos + 4));
            iss >> bin_name;
        }
        if (bin_name.empty()) break;

        std::filesystem::path bin_path(bin_name);
        if (bin_path.is_relative()) return path.parent_path() / bin_path;
        return bin_path;
    }
    return path;
}

static bool read_at(std::ifstream& f, uint64_t offset, uint8_t* out, size_t len) {
    f.clear();
    f.seekg((std::streamoff)offset, std::ios::beg);
    if (!f.good()) return false;
    f.read(reinterpret_cast<char*>(out), (std::streamsize)len);
    return f.gcount() == (std::streamsize)len;
}

static bool is_iso_pvd(const uint8_t* sector) {
    return sector[0] == 0x01 && std::memcmp(&sector[1], "CD001", 5) == 0 && sector[6] == 0x01;
}

static std::string psx_exe_id_from_game_id(const std::string& game_id) {
    if (game_id.size() == 10 && game_id[4] == '-') {
        return game_id.substr(0, 4) + "_" + game_id.substr(5, 3) + "." + game_id.substr(8, 2);
    }
    return game_id;
}

struct DiscValidation {
    bool opened = false;
    bool has_header = false;
    bool id_matches = false;
    std::string detail;
};

static DiscValidation validate_disc_image(const std::filesystem::path& selected_path,
                                          const std::string& game_id) {
    DiscValidation v;
    const std::filesystem::path data_path = cue_data_path(selected_path);
    std::ifstream f(data_path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        v.detail = "Could not open the selected disc image or its CUE-referenced BIN file.";
        return v;
    }
    v.opened = true;

    const uint64_t size = (uint64_t)f.tellg();
    uint8_t sector[2048];
    if ((size >= (16ull * 2352ull + 24ull + sizeof(sector))) &&
        read_at(f, 16ull * 2352ull + 24ull, sector, sizeof(sector)) &&
        is_iso_pvd(sector)) {
        v.has_header = true;
    } else if ((size >= (16ull * 2048ull + sizeof(sector))) &&
               read_at(f, 16ull * 2048ull, sector, sizeof(sector)) &&
               is_iso_pvd(sector)) {
        v.has_header = true;
    }

    if (!v.has_header) {
        v.detail = "No ISO9660 CD001 header was found at the standard PS1 disc locations.";
        return v;
    }

    if (game_id.empty()) {
        v.id_matches = true;
        return v;
    }

    const std::string exe_id = uppercase_ascii(psx_exe_id_from_game_id(game_id));
    const std::string serial_id = uppercase_ascii(game_id);
    const uint64_t scan_len_u64 = std::min<uint64_t>(size, 16ull * 1024ull * 1024ull);
    const size_t scan_len = (size_t)scan_len_u64;
    std::vector<uint8_t> scan(scan_len);
    if (!read_at(f, 0, scan.data(), scan.size())) {
        v.detail = "The ISO header is present, but the image could not be scanned for the game ID.";
        return v;
    }

    auto contains_ascii = [&](const std::string& needle) {
        if (needle.empty() || needle.size() > scan.size()) return false;
        return std::search(scan.begin(), scan.end(), needle.begin(), needle.end()) != scan.end();
    };

    if (contains_ascii(exe_id) || contains_ascii(serial_id)) {
        v.id_matches = true;
    } else {
        v.detail = "The disc header is readable, but it does not contain the expected game ID " +
                   exe_id + " / " + serial_id + " in the early disc metadata.";
    }
    return v;
}

static bool validate_disc_for_launch(const std::filesystem::path& path,
                                     const std::string& game_id) {
    const DiscValidation v = validate_disc_image(path, game_id);
    if (!v.opened) {
        launcher_warning("Disc Image Not Found", v.detail + "\n\nSelected path:\n" + path.string());
        return false;
    }
    if (!v.has_header || !v.id_matches) {
        launcher_warning("Disc Image Warning",
            v.detail + "\n\nThis may be the wrong game or a corrupt image. The runtime will try to run it anyway.");
    }
    return true;
}

static bool validate_bios_for_launch(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    const std::streamoff size = f.tellg();
    if (size != 512 * 1024) {
        launcher_warning("BIOS Warning",
            "The selected BIOS is not 512 KiB. Please select SCPH1001.BIN.");
        return false;
    }
    std::vector<uint8_t> data((size_t)size);
    if (!read_at(f, 0, data.data(), data.size())) return false;
    const uint32_t crc = crc32_compute(data.data(), data.size());
    if (crc != 0x37157331u) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "The selected BIOS CRC32 is %08X, but this build was validated with SCPH1001.BIN CRC32 37157331.\n\n"
            "The runtime will try it anyway, but boot may fail.", crc);
        launcher_warning("BIOS Warning", buf);
    }
    return true;
}

static std::filesystem::path resolve_bios_path(const char* requested, const char* argv0);

static std::filesystem::path resolve_bios_for_runtime(const char* requested,
                                                      const char* argv0) {
    std::filesystem::path resolved = resolve_bios_path(requested, argv0);
    if (!resolved.empty() && std::filesystem::exists(resolved) &&
        validate_bios_for_launch(resolved)) {
        return resolved;
    }

    std::filesystem::path cached = read_cached_path(argv0, "bios.cfg");
    if (!cached.empty() && std::filesystem::exists(cached) &&
        validate_bios_for_launch(cached)) {
        return cached;
    }

    /* Interactive pick. Be explicit about WHICH of the two user-supplied
     * files is being requested — first-time users see two pickers in a
     * row and the difference between "BIOS" and "disc image" is not
     * obvious to non-technical players. */
    launcher_info((s_picker_game_name + " — PlayStation BIOS needed").c_str(),
        s_picker_game_name + " does not include any Sony or game files.\n\n"
        "Step 1 of 2 — PlayStation BIOS\n\n"
        "In the next window, select your PlayStation BIOS dump. The file is "
        "usually named SCPH1001.BIN and is exactly 512 KB. You must dump it "
        "from your own console or otherwise legally obtain it.\n\n"
        "(This is NOT the game disc — that is asked for next.)");
    std::string bios_title =
        s_picker_game_name + " — Step 1 of 2: select PlayStation BIOS (SCPH1001.BIN)";
    for (;;) {
        std::filesystem::path picked;
        if (!pick_runtime_file(
                bios_title.c_str(),
                "PlayStation BIOS (*.bin)\0*.bin\0All Files (*.*)\0*.*\0",
                picked)) {
            return {};
        }
        if (validate_bios_for_launch(picked)) {
            write_cached_path(argv0, "bios.cfg", picked);
            return picked;
        }
    }
}

static std::filesystem::path resolve_disc_for_runtime(const std::filesystem::path& config_disc,
                                                      const char* disc_override,
                                                      const std::string& game_id,
                                                      const char* argv0) {
    if (disc_override && disc_override[0]) {
        std::filesystem::path p = std::filesystem::absolute(disc_override);
        return validate_disc_for_launch(p, game_id) ? p : std::filesystem::path{};
    }

    if (!config_disc.empty() && std::filesystem::exists(config_disc) &&
        validate_disc_for_launch(config_disc, game_id)) {
        return config_disc;
    }

    std::filesystem::path cached = read_cached_path(argv0, "disc.cfg");
    if (!cached.empty() && std::filesystem::exists(cached) &&
        validate_disc_for_launch(cached, game_id)) {
        return cached;
    }

    launcher_info((s_picker_game_name + " — game disc image needed").c_str(),
        "Step 2 of 2 — game disc image\n\n"
        "In the next window, select your " + s_picker_game_name +
        (game_id.empty() ? std::string() : " (" + game_id + ")") +
        " disc image ripped from your own disc.\n\n"
        "Accepted formats: .cue (preferred, with its .bin next to it), "
        ".bin, or .iso.\n\n"
        "(This is NOT the BIOS — the BIOS was already chosen.)");
    std::string disc_title =
        s_picker_game_name + " — Step 2 of 2: select " + s_picker_game_name +
        " disc image (.cue / .bin / .iso)";
    for (;;) {
        std::filesystem::path picked;
        if (!pick_runtime_file(
                disc_title.c_str(),
                "PS1 Disc Images (*.cue;*.bin;*.iso)\0*.cue;*.bin;*.iso\0All Files (*.*)\0*.*\0",
                picked)) {
            return {};
        }
        if (validate_disc_for_launch(picked, game_id)) {
            write_cached_path(argv0, "disc.cfg", picked);
            return picked;
        }
    }
}

static std::filesystem::path resolve_bios_path(const char* requested, const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(requested);
    if (fs::exists(p, ec)) {
        fs::path abs = fs::absolute(p, ec);
        return ec ? p : abs;
    }
    if (p.is_absolute()) return p;

    std::vector<fs::path> roots;
    roots.push_back(fs::current_path());
    if (argv0 && argv0[0]) roots.push_back(fs::absolute(argv0, ec).parent_path());

    for (const fs::path& root : roots) {
        fs::path found = find_upward(root, p);
        if (!found.empty()) return found / p;
    }
    return p;
}

// Fallback memcard directory used when no game config (or its [runtime]
// block) specifies one. Prefer the executable's directory so saves live
// next to the binary; fall back to the current working directory.
static std::filesystem::path default_memcard_dir(const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path exe_dir;
    if (argv0 && argv0[0]) {
        exe_dir = fs::absolute(argv0, ec).parent_path();
        if (ec) exe_dir.clear();
    }
    if (exe_dir.empty()) exe_dir = fs::current_path();
    return exe_dir;
}

static void close_controller(void);

static void shutdown_runtime(void) {
    memcard_flush_all();
    overlay_capture_write_json();
    if (sdl_audio_device) {
        SDL_ClearQueuedAudio(sdl_audio_device);
        SDL_CloseAudioDevice(sdl_audio_device);
        sdl_audio_device = 0;
    }
    close_controller();
    debug_server_shutdown();
}

static void sdl_audio_pump(void) {
    if (!sdl_audio_device) return;

    const uint32_t bytes_per_frame = sizeof(int16_t) * 2u;
    const uint32_t max_queue_bytes = 44100u * bytes_per_frame / 5u;
    if (SDL_GetQueuedAudioSize(sdl_audio_device) > max_queue_bytes) return;

    static double sample_accum = 0.0;
    sample_accum += 44100.0 / 60.0;
    int frames = (int)sample_accum;
    sample_accum -= (double)frames;
    if (frames <= 0) return;
    if (frames > 2048) frames = 2048;

    spu_render(sdl_audio_buf, frames);
    SDL_QueueAudio(sdl_audio_device, sdl_audio_buf,
                   (uint32_t)frames * bytes_per_frame);
}

/* PS1 digital pad button bits (active-low: 0=pressed, 1=released).
 * Bit 0 = SELECT, Bit 3 = START, Bit 4 = UP, Bit 5 = RIGHT,
 * Bit 6 = DOWN, Bit 7 = LEFT, Bit 8 = L2, Bit 9 = R2,
 * Bit 10 = L1, Bit 11 = R1, Bit 12 = TRIANGLE, Bit 13 = CIRCLE,
 * Bit 14 = CROSS, Bit 15 = SQUARE */
#define PAD_SELECT   (1 << 0)
#define PAD_START    (1 << 3)
#define PAD_UP       (1 << 4)
#define PAD_RIGHT    (1 << 5)
#define PAD_DOWN     (1 << 6)
#define PAD_LEFT     (1 << 7)
#define PAD_L2       (1 << 8)
#define PAD_R2       (1 << 9)
#define PAD_L1       (1 << 10)
#define PAD_R1       (1 << 11)
#define PAD_TRIANGLE (1 << 12)
#define PAD_CIRCLE   (1 << 13)
#define PAD_CROSS    (1 << 14)
#define PAD_SQUARE   (1 << 15)

struct ControllerSource {
    enum class Kind {
        None,
        Button,
        AxisPositive,
        AxisNegative,
    };

    Kind kind = Kind::None;
    int id = -1;
};

struct PsxButtonMap {
    uint16_t bit;
    const char* ini_name;
    std::vector<ControllerSource> sources;
};

static int controller_device_index = 0;
static int controller_deadzone = 12000;
static std::array<PsxButtonMap, 14> controller_map = {{
    { PAD_UP,       "up",       {} },
    { PAD_DOWN,     "down",     {} },
    { PAD_LEFT,     "left",     {} },
    { PAD_RIGHT,    "right",    {} },
    { PAD_CROSS,    "cross",    {} },
    { PAD_CIRCLE,   "circle",   {} },
    { PAD_SQUARE,   "square",   {} },
    { PAD_TRIANGLE, "triangle", {} },
    { PAD_L1,       "l1",       {} },
    { PAD_R1,       "r1",       {} },
    { PAD_L2,       "l2",       {} },
    { PAD_R2,       "r2",       {} },
    { PAD_START,    "start",    {} },
    { PAD_SELECT,   "select",   {} },
}};

static std::string trim_copy(const std::string& s) {
    size_t first = 0;
    while (first < s.size() && std::isspace((unsigned char)s[first])) first++;
    size_t last = s.size();
    while (last > first && std::isspace((unsigned char)s[last - 1])) last--;
    return s.substr(first, last - first);
}

static std::string lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static bool parse_bool_value(const std::string& value, bool fallback) {
    std::string v = lower_copy(trim_copy(value));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return fallback;
}

static ControllerSource parse_controller_source(const std::string& raw) {
    std::string s = lower_copy(trim_copy(raw));
    ControllerSource out;
    if (s.empty() || s == "none" || s == "disabled") return out;

    if (s == "a")              { out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_A; return out; }
    if (s == "b")              { out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_B; return out; }
    if (s == "x")              { out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_X; return out; }
    if (s == "y")              { out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_Y; return out; }
    if (s == "back" || s == "view" || s == "select") {
        out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_BACK; return out;
    }
    if (s == "start" || s == "menu") {
        out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_START; return out;
    }
    if (s == "guide")          { out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_GUIDE; return out; }
    if (s == "leftstick")      { out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_LEFTSTICK; return out; }
    if (s == "rightstick")     { out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_RIGHTSTICK; return out; }
    if (s == "leftshoulder" || s == "lb" || s == "l1") {
        out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_LEFTSHOULDER; return out;
    }
    if (s == "rightshoulder" || s == "rb" || s == "r1") {
        out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_RIGHTSHOULDER; return out;
    }
    if (s == "dpup" || s == "dpadup") {
        out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_DPAD_UP; return out;
    }
    if (s == "dpdown" || s == "dpaddown") {
        out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_DPAD_DOWN; return out;
    }
    if (s == "dpleft" || s == "dpadleft") {
        out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_DPAD_LEFT; return out;
    }
    if (s == "dpright" || s == "dpadright") {
        out.kind = ControllerSource::Kind::Button; out.id = SDL_CONTROLLER_BUTTON_DPAD_RIGHT; return out;
    }
    if (s == "lefttrigger" || s == "lt" || s == "l2") {
        out.kind = ControllerSource::Kind::AxisPositive; out.id = SDL_CONTROLLER_AXIS_TRIGGERLEFT; return out;
    }
    if (s == "righttrigger" || s == "rt" || s == "r2") {
        out.kind = ControllerSource::Kind::AxisPositive; out.id = SDL_CONTROLLER_AXIS_TRIGGERRIGHT; return out;
    }
    if (s == "leftx+" || s == "lsright") {
        out.kind = ControllerSource::Kind::AxisPositive; out.id = SDL_CONTROLLER_AXIS_LEFTX; return out;
    }
    if (s == "leftx-" || s == "lsleft") {
        out.kind = ControllerSource::Kind::AxisNegative; out.id = SDL_CONTROLLER_AXIS_LEFTX; return out;
    }
    if (s == "lefty+" || s == "lsdown") {
        out.kind = ControllerSource::Kind::AxisPositive; out.id = SDL_CONTROLLER_AXIS_LEFTY; return out;
    }
    if (s == "lefty-" || s == "lsup") {
        out.kind = ControllerSource::Kind::AxisNegative; out.id = SDL_CONTROLLER_AXIS_LEFTY; return out;
    }
    if (s == "rightx+" || s == "rsright") {
        out.kind = ControllerSource::Kind::AxisPositive; out.id = SDL_CONTROLLER_AXIS_RIGHTX; return out;
    }
    if (s == "rightx-" || s == "rsleft") {
        out.kind = ControllerSource::Kind::AxisNegative; out.id = SDL_CONTROLLER_AXIS_RIGHTX; return out;
    }
    if (s == "righty+" || s == "rsdown") {
        out.kind = ControllerSource::Kind::AxisPositive; out.id = SDL_CONTROLLER_AXIS_RIGHTY; return out;
    }
    if (s == "righty-" || s == "rsup") {
        out.kind = ControllerSource::Kind::AxisNegative; out.id = SDL_CONTROLLER_AXIS_RIGHTY; return out;
    }

    SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(s.c_str());
    if (button != SDL_CONTROLLER_BUTTON_INVALID) {
        out.kind = ControllerSource::Kind::Button;
        out.id = button;
        return out;
    }
    SDL_GameControllerAxis axis = SDL_GameControllerGetAxisFromString(s.c_str());
    if (axis != SDL_CONTROLLER_AXIS_INVALID) {
        out.kind = ControllerSource::Kind::AxisPositive;
        out.id = axis;
        return out;
    }
    return out;
}

static std::vector<ControllerSource> parse_source_list(const std::string& value) {
    std::vector<ControllerSource> sources;
    std::stringstream ss(value);
    std::string item;
    while (std::getline(ss, item, ',')) {
        ControllerSource source = parse_controller_source(item);
        if (source.kind != ControllerSource::Kind::None) {
            sources.push_back(source);
        }
    }
    return sources;
}

static void set_default_controller_mapping(void) {
    for (auto& entry : controller_map) entry.sources.clear();

    auto set_sources = [](const char* name, const char* sources) {
        for (auto& entry : controller_map) {
            if (std::strcmp(entry.ini_name, name) == 0) {
                entry.sources = parse_source_list(sources);
                return;
            }
        }
    };

    set_sources("up",       "dpup,lefty-");
    set_sources("down",     "dpdown,lefty+");
    set_sources("left",     "dpleft,leftx-");
    set_sources("right",    "dpright,leftx+");
    set_sources("cross",    "a");
    set_sources("circle",   "b");
    set_sources("square",   "x");
    set_sources("triangle", "y");
    set_sources("l1",       "leftshoulder");
    set_sources("r1",       "rightshoulder");
    set_sources("l2",       "lefttrigger");
    set_sources("r2",       "righttrigger");
    set_sources("start",    "start");
    set_sources("select",   "back");
}

static std::string default_input_ini_text(void) {
    return
        "; PSXRecomp input mapping. PSX buttons are active when any listed source is pressed.\n"
        "; Sources use SDL/Xbox names: a,b,x,y,back,start,leftshoulder,rightshoulder,\n"
        "; lefttrigger,righttrigger,dpup,dpdown,dpleft,dpright,leftx-/leftx+/lefty-/lefty+.\n"
        "\n"
        "[controller]\n"
        "enabled = true\n"
        "device = 0\n"
        "deadzone = 12000\n"
        "\n"
        "[mapping]\n"
        "up = dpup,lefty-\n"
        "down = dpdown,lefty+\n"
        "left = dpleft,leftx-\n"
        "right = dpright,leftx+\n"
        "cross = a\n"
        "circle = b\n"
        "square = x\n"
        "triangle = y\n"
        "l1 = leftshoulder\n"
        "r1 = rightshoulder\n"
        "l2 = lefttrigger\n"
        "r2 = righttrigger\n"
        "start = start\n"
        "select = back\n";
}

static void load_input_config(const char* argv0) {
    set_default_controller_mapping();

    namespace fs = std::filesystem;
    fs::path config_path = exe_dir_from_argv(argv0) / "input.ini";
    std::error_code ec;
    if (!fs::exists(config_path, ec)) {
        std::ofstream out(config_path, std::ios::binary);
        if (out) out << default_input_ini_text();
        return;
    }

    std::ifstream in(config_path);
    if (!in) return;

    bool controller_enabled = true;
    std::string section;
    std::string line;
    while (std::getline(in, line)) {
        size_t comment = line.find_first_of(";#");
        if (comment != std::string::npos) line.resize(comment);
        line = trim_copy(line);
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower_copy(trim_copy(line.substr(1, line.size() - 2)));
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = lower_copy(trim_copy(line.substr(0, eq)));
        std::string value = trim_copy(line.substr(eq + 1));
        if (section == "controller") {
            if (key == "enabled") {
                controller_enabled = parse_bool_value(value, controller_enabled);
            } else if (key == "device") {
                controller_device_index = std::max(0, std::atoi(value.c_str()));
            } else if (key == "deadzone") {
                controller_deadzone = std::max(0, std::min(32767, std::atoi(value.c_str())));
            }
        } else if (section == "mapping") {
            for (auto& entry : controller_map) {
                if (key == entry.ini_name) {
                    entry.sources = parse_source_list(value);
                    break;
                }
            }
        }
    }

    if (!controller_enabled) {
        for (auto& entry : controller_map) entry.sources.clear();
    }
}

static void close_controller(void) {
    if (sdl_controller) {
        SDL_GameControllerClose(sdl_controller);
        sdl_controller = nullptr;
        sdl_controller_instance = -1;
    }
}

static void open_configured_controller(void) {
    if (sdl_controller) return;

    /* First pass: open the configured device index. Second pass: open the
     * first available controller — covers Steam Remote Play where the virtual
     * Xbox controller appears at an unpredictable index, and cases where the
     * configured index is simply out of range. */
    for (int pass = 0; pass < 2 && !sdl_controller; pass++) {
        int seen = 0;
        int joysticks = SDL_NumJoysticks();
        for (int i = 0; i < joysticks; i++) {
            if (!SDL_IsGameController(i)) continue;
            int match = (pass == 0) ? (seen == controller_device_index) : 1;
            seen++;
            if (!match) continue;

            sdl_controller = SDL_GameControllerOpen(i);
            if (sdl_controller) {
                SDL_Joystick* joy = SDL_GameControllerGetJoystick(sdl_controller);
                sdl_controller_instance = joy ? SDL_JoystickInstanceID(joy) : -1;
                const char* name = SDL_GameControllerName(sdl_controller);
                std::fprintf(stdout, "psxrecomp runtime: controller %d (pass %d): %s\n",
                             seen - 1, pass, name ? name : "(unnamed)");
            }
            break;
        }
    }
}

static bool controller_source_pressed(const ControllerSource& source) {
    if (!sdl_controller) return false;

    switch (source.kind) {
    case ControllerSource::Kind::Button:
        return SDL_GameControllerGetButton(
            sdl_controller, (SDL_GameControllerButton)source.id) != 0;
    case ControllerSource::Kind::AxisPositive:
        return SDL_GameControllerGetAxis(
            sdl_controller, (SDL_GameControllerAxis)source.id) > controller_deadzone;
    case ControllerSource::Kind::AxisNegative:
        return SDL_GameControllerGetAxis(
            sdl_controller, (SDL_GameControllerAxis)source.id) < -controller_deadzone;
    case ControllerSource::Kind::None:
    default:
        return false;
    }
}

static uint16_t pad_from_keyboard(void) {
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    uint16_t buttons = 0xFFFF; /* all released */

    if (keys[SDL_SCANCODE_UP])      buttons &= ~PAD_UP;
    if (keys[SDL_SCANCODE_DOWN])    buttons &= ~PAD_DOWN;
    if (keys[SDL_SCANCODE_LEFT])    buttons &= ~PAD_LEFT;
    if (keys[SDL_SCANCODE_RIGHT])   buttons &= ~PAD_RIGHT;
    if (keys[SDL_SCANCODE_RETURN])  buttons &= ~PAD_START;
    if (keys[SDL_SCANCODE_RSHIFT])  buttons &= ~PAD_SELECT;
    if (keys[SDL_SCANCODE_X])       buttons &= ~PAD_CROSS;
    if (keys[SDL_SCANCODE_Z])       buttons &= ~PAD_SQUARE;
    if (keys[SDL_SCANCODE_S])       buttons &= ~PAD_CIRCLE;
    if (keys[SDL_SCANCODE_A])       buttons &= ~PAD_TRIANGLE;
    if (keys[SDL_SCANCODE_Q])       buttons &= ~PAD_L1;
    if (keys[SDL_SCANCODE_W])       buttons &= ~PAD_R1;
    if (keys[SDL_SCANCODE_E])       buttons &= ~PAD_L2;
    if (keys[SDL_SCANCODE_R])       buttons &= ~PAD_R2;

    return buttons;
}

static uint16_t merge_controller_pad(uint16_t buttons) {
    if (!sdl_controller) return buttons;

    for (const auto& entry : controller_map) {
        for (const auto& source : entry.sources) {
            if (controller_source_pressed(source)) {
                buttons &= (uint16_t)~entry.bit;
                break;
            }
        }
    }
    return buttons;
}

/* PSX native vblank cadence: NTSC ≈ 59.94 Hz. Wall-clock target keeps
 * audio sample generation (735 samples/vblank * 60 = 44100/sec) matched
 * to the SDL audio device drain rate, eliminating queue overflow drops
 * and underruns. Uncapped, the host runs the simulation at whatever
 * speed it can — typically several × realtime — and audio glitches. */
static constexpr double PSX_FRAME_PERIOD_MS = 1000.0 / 59.94;

/* Called from gpu_vblank_tick() at each simulated vblank. */
static void sdl_vblank_present(void) {
#ifndef PSX_NO_DEBUG_TOOLS
    /* Debug server: pause gate, poll commands, record frame, check watchpoints. */
    debug_server_wait_if_paused();
    debug_server_poll();
    debug_server_record_frame();
    debug_server_check_watchpoints();

    /* Check debug server input override. */
    int override = debug_server_get_input_override();
#else
    /* Production: skip debug server. Still need to advance frame counter
     * locally so anything else that reads it continues to work. */
    extern uint64_t s_frame_count;
    s_frame_count++;
    int override = -1;
#endif

    /* Step 2.8 automation ticks — BEFORE the turbo/fast-boot early returns
     * below, so capture detection and compile pickup keep running while the
     * frontend is skipping presents. Both are cheap and emu-thread-only. */
    overlay_autocapture_tick();
    autocompile_poll_main();

    /* Pump SDL events to prevent window freeze. */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            psx_crash_trace_set_exit_origin("sdl_window_close");
            shutdown_runtime();
            std::exit(0);
        } else if (ev.type == SDL_CONTROLLERDEVICEADDED) {
            open_configured_controller();
        } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
            if (ev.cdevice.which == sdl_controller_instance) {
                close_controller();
                open_configured_controller();
            }
        } else if (ev.type == SDL_KEYDOWN) {
            /* Fullscreen toggle: F11, Alt+Enter, or Cmd/Ctrl+F.
             * FULLSCREEN_DESKTOP keeps the desktop resolution; the
             * renderer's logical size letterboxes the 640x480 image. */
            const Uint16 mod = ev.key.keysym.mod;
            if (ev.key.keysym.sym == SDLK_F11 ||
                (ev.key.keysym.sym == SDLK_RETURN && (mod & KMOD_ALT)) ||
                (ev.key.keysym.sym == SDLK_f && (mod & (KMOD_GUI | KMOD_CTRL)))) {
                Uint32 is_fs = SDL_GetWindowFlags(sdl_window) &
                               SDL_WINDOW_FULLSCREEN_DESKTOP;
                SDL_SetWindowFullscreen(sdl_window,
                    is_fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
        }
    }

    /* Sample keyboard state and feed into SIO controller.
     * Debug server input override takes priority if active. */
    uint16_t pad_buttons_this_frame;
    if (override >= 0) {
        pad_buttons_this_frame = (uint16_t)override;
    } else {
        pad_buttons_this_frame = merge_controller_pad(pad_from_keyboard());
    }
    sio_set_pad_state(pad_buttons_this_frame);

    /* Turbo-active test, shared by the audio mute here and the pacing/
     * present gate below. During turbo the guest runs at host speed, so
     * rendered SPU audio is time-compressed garble — queueing it is what
     * players hear as load-time stutter. Muting = stop queueing: the SDL
     * device drains the last real-time samples and underruns to silence;
     * SPU voice positions freeze with the pump, so music resumes from
     * where it left off when pacing returns. */
    int turbo_loads_active = 0;
    if (g_turbo_loads_enabled) {
        extern int fntrace_is_game_started(void);
        if (fntrace_is_game_started() && cdrom_load_in_progress())
            turbo_loads_active = 1;
    }
#ifndef PSX_SDL_NO_AUDIO
    if (!turbo_loads_active) sdl_audio_pump();
#endif

    /* TCP turbo is for automated validation and trace capture. It keeps the
     * simulation advancing and the debug server polling, but removes frontend
     * presentation and wall-clock pacing. */
#ifndef PSX_NO_DEBUG_TOOLS
    if (debug_server_turbo_enabled()) return;
#endif

    /* Turbo mode: while TAB is held, skip both VRAM->ARGB conversion and
     * SDL_RenderPresent. The recompiled BIOS still advances simulated
     * cycles every vblank, so the BIOS proceeds at whatever rate the host
     * CPU sustains without graphics-driver vsync overhead. Present once
     * every TURBO_PRESENT_EVERY frames so the user sees visual progress. */
    {
        const Uint8* keys = SDL_GetKeyboardState(NULL);
        static int turbo_skip = 0;
        const int TURBO_PRESENT_EVERY = 30;
        if (keys[SDL_SCANCODE_TAB]) {
            turbo_skip = (turbo_skip + 1) % TURBO_PRESENT_EVERY;
            if (turbo_skip != 0) return;  /* skip render this frame */
        } else {
            turbo_skip = 0;
        }
    }

    /* Fast boot: run at full speed with no display until game entry PC fires.
     * Kernel init and disc auth still execute; only logo rendering is skipped. */
    if (s_fast_boot_active) {
        extern int fntrace_is_game_started(void);
        if (!fntrace_is_game_started()) return;
        s_fast_boot_active = 0;
    }

    /* Turbo-through-loads (step 4, OPT-IN via game.toml [runtime]
     * turbo_loads): while the game is loading — CD data stream active,
     * XA/FMV excluded, post-BIOS-handoff only — skip wall-clock pacing and
     * most presents so the guest runs at host speed. Step-3 measurement
     * showed loads are paced by the game's own per-sector processing at
     * real time (2.2-4.8 sectors/frame against a 32-256 IRQ budget), so
     * host-speed execution is the lever that compresses load wall-time.
     * Presents 1-in-30 so visual progress stays visible. */
    if (turbo_loads_active) {
        g_turbo_loads_frames++;
        static int tl_skip = 0;
        const int TL_PRESENT_EVERY = 30;
        tl_skip = (tl_skip + 1) % TL_PRESENT_EVERY;
        if (tl_skip != 0) return;
    }

    /* Wall-clock pacing: always runs once fast_boot has ended, even when the
     * display is still disabled (e.g. game crt0 setup). Skipped only by the
     * turbo and fast_boot early-returns above. frame_pacer_wait is the
     * race-free replacement for the old open-coded loop whose double
     * counter read could underflow into a ~24.7-day SDL_Delay (Bug B
     * hard freeze). */
    {
        static FramePacer pacer = { 0 };
        frame_pacer_wait(&pacer, PSX_FRAME_PERIOD_MS);
    }

    /* ---- Display from our VRAM ---- */
    uint32_t w = 0, h = 0;
    int active_scale = 1;   /* hi-res mirror used only for 15-bit display */
    {
        static bool disabled_frame_presented = false;
        GpuDisplayInfo di;
        gpu_get_display_info(&di);
        if (di.disabled || di.width == 0 || di.height == 0) {
#ifndef PSX_SDL_NO_RENDER
            if (!disabled_frame_presented) {
                disabled_frame_presented = true;
                if (g_gl_active) {
                    gl_renderer_present_blank();
                } else {
                    SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);
                    SDL_RenderClear(sdl_renderer);
                    SDL_RenderPresent(sdl_renderer);
                }
            }
#endif
            return;
        }
        disabled_frame_presented = false;
        w = di.width; h = di.height;

        /* The hi-res mirror is a 15-bit copy of VRAM; 24-bit display (FMV)
         * reads packed bytes the mirror can't represent, so fall back to the
         * native path for those frames (the present filter still upscales). */
        active_scale = (g_video_scale > 1 && !di.depth24) ? g_video_scale : 1;

        if (active_scale > 1) {
            int sw = (int)w * active_scale;
            gr_render_display_hires(sdl_pixel_buf, (int)(sw * sizeof(uint32_t)),
                                    (int)di.display_x, (int)di.display_y,
                                    (int)w, (int)h);
        } else {
            for (uint32_t y = 0; y < h; y++) {
                for (uint32_t x = 0; x < w; x++) {
                    sdl_pixel_buf[y * w + x] = gpu_display_pixel_argb(&di, x, y);
                }
            }
        }
    }

    /* Update only the active display rectangle. The backing texture is sized
     * 640x512 (times the supersampling factor), while games can switch to
     * smaller modes such as 320x224 for FMV; presenting the full texture would
     * leave the active image stuck in the upper-left portion of the window. */
#ifndef PSX_SDL_NO_RENDER
    int src_w = (int)w * active_scale;
    int src_h = (int)h * active_scale;
    if (g_gl_active) {
        /* OpenGL present: upload the active display rect and draw a full-screen
         * quad. SDL_GL_SwapWindow handles vsync; the wall-clock pacer above
         * still owns timing. */
        gl_renderer_present(sdl_pixel_buf, src_w, src_h, g_video_aa ? 1 : 0);
    } else {
    SDL_Rect src = { 0, 0, src_w, src_h };
    SDL_UpdateTexture(sdl_texture, &src, sdl_pixel_buf,
                      (int)(src_w * sizeof(uint32_t)));

    SDL_Rect dst = { 0, 0, 640 * g_video_scale, 480 * g_video_scale };
    SDL_RenderClear(sdl_renderer);
    SDL_RenderCopy(sdl_renderer, sdl_texture, &src, &dst);

    /* Vsync self-heal. The renderer is created with PRESENTVSYNC for
     * tear-free output, but the wall-clock pacer above already holds
     * 59.94 Hz, so driver vsync is redundant for timing. Under some
     * driver states (observed: NVIDIA GL with the swap queue wedged)
     * SwapBuffers blocks ~1.5 s per present, dragging the whole
     * emulation to ~0.7 fps for minutes (freeze dump 1781045865:
     * 8/8 main-thread samples inside wglSwapBuffers). If presents
     * block pathologically several times in a row, drop driver vsync
     * for the rest of the session; our own pacing keeps the rate. */
    {
        const Uint64 t0 = SDL_GetPerformanceCounter();
        SDL_RenderPresent(sdl_renderer);
        const Uint64 t1 = SDL_GetPerformanceCounter();
        const Uint64 freq = SDL_GetPerformanceFrequency();
        const Uint64 present_ms = (t1 >= t0 && freq) ? ((t1 - t0) * 1000u) / freq : 0;
        if (!g_present_vsync_disabled && present_ms > 250) {
            g_present_slow_count++;
            if (g_present_slow_count >= 3 &&
                SDL_RenderSetVSync(sdl_renderer, 0) == 0) {
                g_present_vsync_disabled = 1;
            }
        }
    }
    }
#endif
}

int main(int argc, char** argv) {
    /* Force line-buffered output so messages appear even if killed. */
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    std::fprintf(stderr, "psxrecomp: main() entered\n");
    std::fflush(stderr);

    /* Install crash handlers early so they catch issues during init too.
     * Writes psx_last_run_report.json on signal/SEH/atexit/fail-fast. */
    psx_crash_trace_install_handlers();

    const char* bios_path = PSX_DEFAULT_BIOS_PATH;
    const char* game_config_path = nullptr;
    const char* disc_override_path = nullptr;
    /* Parse args.
     *   --bios <path>       override the compile-time BIOS path
     *   --game <toml>       load a game config (single source of truth for
     *                       disc / memcard / window title / debug port)
     *   --disc <path>       override the game config disc path
     *   <positional>        deprecated alias for --bios
     * No --memcard-dir / --game-root flags: those are config-driven. */
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--bios") == 0 && i + 1 < argc) {
            bios_path = argv[++i];
        } else if (std::strcmp(argv[i], "--game") == 0 && i + 1 < argc) {
            game_config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--disc") == 0 && i + 1 < argc) {
            disc_override_path = argv[++i];
        } else if (argv[i][0] != '-') {
            bios_path = argv[i];
        }
    }

    std::string default_game_config_storage;
    if (!game_config_path) {
        std::filesystem::path default_game_config =
            resolve_existing_runtime_path(PSX_DEFAULT_GAME_CONFIG_PATH, argv[0]);
        if (!default_game_config.empty()) {
            default_game_config_storage = default_game_config.string();
            game_config_path = default_game_config_storage.c_str();
        }
    }

    std::filesystem::path memcard_dir;
    std::filesystem::path resolved_disc;
    std::string window_title = PSX_WINDOW_TITLE;
    uint16_t   debug_port    = (uint16_t)DEFAULT_DEBUG_PORT;
    std::string game_name;
    std::string game_id;
    std::string disc_speed;   /* "1x" | "2x" | "4x" | "instant" */
    int        instant_rate  = 0;   /* 0 = cdrom.c built-in default */
    uint32_t   game_entry_pc = 0;
    bool       fast_boot     = false;

    if (game_config_path) {
        try {
            const auto gc = PSXRecompV4::load_game_config(game_config_path);
            game_name = gc.name;
            game_id   = gc.id;
            if (!gc.discs.empty()) resolved_disc = gc.discs.front();
            if (gc.runtime.has_memcard_dir)  memcard_dir   = gc.runtime.memcard_dir;
            if (gc.runtime.has_window_title) window_title  = gc.runtime.window_title;
            if (gc.runtime.has_debug_port)   debug_port    = gc.runtime.debug_port;
            if (gc.runtime.has_disc_speed)   disc_speed    = gc.runtime.disc_speed;
            if (gc.runtime.has_instant_max_per_frame)
                instant_rate = gc.runtime.instant_max_per_frame;
            if (gc.runtime.turbo_loads) {
                g_turbo_loads_enabled = 1;
                std::fprintf(stdout, "psxrecomp: turbo_loads enabled (opt-in)\n");
            }
            g_video_scale     = gc.runtime.video_supersampling;
            g_video_aa        = gc.runtime.video_antialiasing;
            g_video_texfilter = gc.runtime.video_texture_filter;
            g_video_renderer  = gc.runtime.video_renderer;
            game_entry_pc = gc.entry_pc;
            fast_boot     = gc.runtime.fast_boot;
            /* Overlay DLL cache (Layer A). Off unless enabled in [runtime];
             * when on, capture overlay bytes and scan cache/<game_id>/ for
             * precompiled overlay DLLs. */
            if (gc.runtime.overlay_cache) {
                std::filesystem::path exe_dir = exe_dir_from_argv(argv[0]);
                std::string cache_dir = (exe_dir / "cache").string();
                overlay_capture_set_out_dir(exe_dir.string().c_str());
                overlay_capture_set_enabled(1);
                overlay_loader_init(cache_dir.c_str(), game_id.c_str());
                /* Step 2.8 variant-capture automation. Autocapture is ON
                 * whenever the cache is on: it writes the player-shareable
                 * overlay_captures.json (the contribution file the README
                 * promises) even on machines with no compile toolchain.
                 * The background compile additionally needs a configured
                 * command; autocompile_request() no-ops without one. */
                overlay_autocapture_set_enabled(1);
                if (gc.runtime.has_overlay_autocompile_cmd) {
                    autocompile_configure(
                        gc.runtime.overlay_autocompile_cmd.c_str(),
                        gc.project_root.string().c_str());
                    std::fprintf(stdout,
                        "psxrecomp: overlay autocompile enabled\n");
                }
            }
            std::fprintf(stdout, "psxrecomp: loaded game config %s (%s, %s)\n",
                         game_config_path, game_name.c_str(), game_id.c_str());
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "psxrecomp: failed to load --game %s: %s\n",
                         game_config_path, ex.what());
            return 1;
        }
    }

    if (!game_name.empty()) s_picker_game_name = game_name;

    std::filesystem::path resolved_bios = resolve_bios_for_runtime(bios_path, argv[0]);
    if (resolved_bios.empty()) {
        std::fprintf(stderr, "psxrecomp: no BIOS selected; exiting.\n");
        return 1;
    }
    if (game_config_path || disc_override_path || !resolved_disc.empty()) {
        resolved_disc = resolve_disc_for_runtime(resolved_disc, disc_override_path, game_id, argv[0]);
        if (game_config_path && resolved_disc.empty()) {
            std::fprintf(stderr, "psxrecomp: no disc image selected; exiting.\n");
            return 1;
        }
    }

    if (memcard_dir.empty()) {
        memcard_dir = default_memcard_dir(argv[0]);
    }

    std::string bios_path_str    = resolved_bios.string();
    std::string memcard_dir_str  = memcard_dir.string();
    std::string disc_path_str    = resolved_disc.string();

    std::fprintf(stdout, "psxrecomp runtime: loading BIOS from %s\n", bios_path_str.c_str());
    memory_init(bios_path_str.c_str());
    /* Select the renderer backend BEFORE gpu_init() (which runs gr_init ->
     * the backend's init on the VRAM buffer). Software is the default and the
     * fallback; an unavailable OpenGL backend reverts to software. */
    gr_set_backend(g_video_renderer == 1 ? GR_BACKEND_OPENGL : GR_BACKEND_SOFTWARE);
    gpu_init();
    /* Internal-resolution supersampling (SSAA). Must follow gpu_init (which
     * runs sw_renderer_init). scale==1 is a no-op; >1 allocates the hi-res
     * VRAM mirror. */
    if (g_video_scale < 1) g_video_scale = 1;
    if (g_video_scale > SW_MAX_INTERNAL_SCALE) g_video_scale = SW_MAX_INTERNAL_SCALE;
    gr_set_scale(g_video_scale);
    g_video_scale = gr_scale(); /* reflect any clamp / alloc fallback */
    gr_set_texture_filter(g_video_texfilter);
    if (g_video_scale > 1 || g_video_texfilter)
        std::fprintf(stdout,
                     "psxrecomp: supersampling %dx (antialiasing %s, texture filter %s)\n",
                     g_video_scale, g_video_aa ? "on" : "off",
                     g_video_texfilter ? "bilinear" : "nearest");
    dma_init();
    mdec_init();
    timers_init();
    interrupts_init();
    sio_init();
    sio_connect_pad(0);  /* Controller on port 1 */
    spu_init();
    cdrom_init(disc_path_str.empty() ? NULL : disc_path_str.c_str());
    {
        int divisor = 1; /* default: authentic 1x timing */
        if (disc_speed == "instant") divisor = 0;
        else if (disc_speed == "4x") divisor = 4;
        else if (disc_speed == "2x") divisor = 2;
        /* Store for post-BIOS application; boot always runs at 1x so the
         * BIOS disc-init sequence sees correct timing. */
        cdrom_set_game_speed(divisor);
        if (instant_rate > 0) cdrom_set_instant_rate(instant_rate);
        if (divisor != 1)
            std::fprintf(stdout, "psxrecomp: disc_speed=%s (applied post-BIOS, "
                         "instant budget %d/frame)\n",
                         disc_speed.c_str(), cdrom_get_instant_rate());
    }
    memcard_init(memcard_dir_str.c_str());
    std::atexit(memcard_flush_all);
#ifndef PSX_NO_DEBUG_TOOLS
    debug_server_init(debug_port);
#else
    (void)debug_port;
#endif
    /* Heartbeat always on — see freeze_heartbeat.c rationale. */
    freeze_heartbeat_start("psx-runtime");
    /* Register game entry_pc for post-BIOS disc speed switch. Fires once when
     * the BIOS hands control to the game EXE — not on the BIOS shell. */
    if (game_entry_pc != 0)
        fntrace_set_game_range(game_entry_pc, 0);

    /* ---- SDL init ---- */
    /* Scale quality governs SDL's logical-size -> window scaling. Linear when
     * antialiasing is on so the (super)sampled frame stays smooth when the
     * window is resized; nearest preserves crisp pixels otherwise. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, g_video_aa ? "1" : "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    /* Prefer SDL's own HIDAPI driver over platform-native so Steam's virtual
     * Xbox controller (injected by Steam Input / Remote Play) is enumerated
     * as a game controller rather than a raw HID device. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    load_input_config(argv[0]);
    open_configured_controller();
#ifndef PSX_SDL_NO_AUDIO
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec want;
        SDL_AudioSpec have;
        SDL_zero(want);
        want.freq = 44100;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        sdl_audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (sdl_audio_device) {
            SDL_PauseAudioDevice(sdl_audio_device, 0);
        }
    }
#endif

    Uint32 win_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (g_video_renderer == 1) win_flags |= SDL_WINDOW_OPENGL;
    sdl_window = SDL_CreateWindow(
        window_title.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        640, 480,
        win_flags
    );
    if (!sdl_window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    /* OpenGL backend: create the GL context now. On failure, relabel the
     * facade back to software (rasterization already runs through software in
     * this phase) and fall through to the SDL_Renderer present path below. */
    if (g_video_renderer == 1) {
        g_gl_active = (gl_renderer_init_context(sdl_window) != 0);
        if (!g_gl_active) gr_set_backend(GR_BACKEND_SOFTWARE);
    }
    /* Make the active renderer visible in the title bar (no guessing). */
    SDL_SetWindowTitle(sdl_window,
        (window_title + (g_gl_active ? "  [OpenGL]" : "  [Software]")).c_str());

    /* Force OpenGL renderer.
     *
     * History (TombaRecomp/ISSUES.md #6):
     *   1. Originally SDL_RENDERER_ACCELERATED with fallback to software.
     *      Froze "Not Responding" after extended uptime — was thought to
     *      be GPU-driver-side hangs.
     *   2. Switched to SDL_RENDERER_SOFTWARE only. Still froze. Software
     *      renderer goes through Windows GDI; the GDI path hangs the SDL
     *      main thread under heavy emulation load.
     *   3. Bisection: NO_AUDIO+NO_RENDER ran indefinitely (~7+ min, 40k+
     *      frames) but the game never progressed past BIOS boot because
     *      it depends on the renderer being present. NO_AUDIO alone with
     *      software renderer froze at frame 3084 — same as full debug
     *      build. So audio is innocent; software renderer (GDI path) is
     *      the culprit.
     *   4. SDL_HINT_RENDER_DRIVER=opengl + SDL_RENDERER_ACCELERATED.
     *      Ran indefinitely past every prior freeze point. OpenGL driver
     *      uses a different presentation path that doesn't hit the GDI
     *      hang. This is now the default.
     *
     * Note: the freeze became prevalent only after the FMV-speed fix
     * (commit b486c13) raised cycle throughput. Before that, the slower
     * MDEC/DMA workload was below whatever GDI threshold trips the bug. */
    /* The OpenGL force above is a Windows-only workaround for the GDI
     * presentation hang; macOS/Linux have no GDI path, so let SDL choose its
     * native backend (Metal on Apple Silicon). PRESENTVSYNC removes tearing;
     * fall back progressively if a driver can't provide vsync/accel. */
  if (!g_gl_active) {
#ifdef _WIN32
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
#endif
    sdl_renderer = SDL_CreateRenderer(sdl_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!sdl_renderer)
        sdl_renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_ACCELERATED);
    if (!sdl_renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Present in a 640x480 logical space (scaled by the supersampling factor
     * so the full internal resolution reaches a large/fullscreen window; SDL
     * still scales and letterboxes to the real output). Identity in the
     * default window when supersampling is off, so native rendering is
     * unchanged. */
    SDL_RenderSetLogicalSize(sdl_renderer, 640 * g_video_scale, 480 * g_video_scale);
  }

    /* Staging buffer + backing texture are sized for the internal resolution
     * (640x512 native, times the supersampling factor). */
    sdl_pixel_buf = (uint32_t*)std::malloc(
        (size_t)640 * g_video_scale * 512 * g_video_scale * sizeof(uint32_t));
    if (!sdl_pixel_buf) {
        std::fprintf(stderr, "failed to allocate %dx staging buffer\n", g_video_scale);
        return 1;
    }

  if (!g_gl_active) {
    sdl_texture = SDL_CreateTexture(
        sdl_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        640 * g_video_scale, 512 * g_video_scale
    );
    if (!sdl_texture) {
        std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetTextureScaleMode(sdl_texture,
                            g_video_aa ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
  }

    /* Register vblank presentation callback. */
    gpu_set_vblank_callback(sdl_vblank_present);

    /* Initialize CPU state. */
    CPUState cpu;
    std::memset(&cpu, 0, sizeof(cpu));

    /* Wire memory function pointers. */
    cpu.read_word  = psx_read_word;
    cpu.write_word = psx_write_word;
    cpu.read_half  = psx_read_half;
    cpu.write_half = psx_write_half;
    cpu.read_byte  = psx_read_byte;
    cpu.write_byte = psx_write_byte;

    /* Fast boot: try to restore a previously-captured BIOS handoff snapshot.
     * First launch runs BIOS normally (logos visible) and captures state at
     * game handoff. Every subsequent launch restores and skips BIOS entirely. */
    bool fast_boot_restored = false;
    if (fast_boot && game_entry_pc != 0) {
        uint32_t bios_cksum = memory_get_bios_checksum();
        char snap_name[64];
        std::snprintf(snap_name, sizeof(snap_name),
                      "fast_boot_%08X_%08X.bin", bios_cksum, game_entry_pc);
        std::filesystem::path snap_path =
            resolved_bios.parent_path() / snap_name;
        std::string snap_str = snap_path.string();
        if (boot_state_load(snap_str.c_str(), bios_cksum, game_entry_pc, &cpu)) {
            fast_boot_restored = true;
        } else {
            /* First run: show BIOS logos normally, capture state silently at
             * game handoff. Subsequent launches use the snapshot. */
            boot_state_set_capture(snap_str.c_str(), bios_cksum, game_entry_pc);
        }
    }

    if (!fast_boot_restored) {
        /* R3000A reset state. */
        cpu.pc = 0xBFC00000u;
        cpu.cop0[12] = 0x00400000u; /* SR: BEV=1 (boot exception vectors) */
    }

    /* Let memory subsystem see SR for cache-isolation checks. */
    memory_set_sr_ptr(&cpu.cop0[12]);

    /* Wire debug server to CPU state for register queries. */
    debug_server_set_cpu(&cpu);

    /* Execute. */
    std::fprintf(stdout, "psxrecomp runtime: executing from PC=0x%08X\n", cpu.pc);

#if defined(PSX_ORACLE_BUILD)
    std::fprintf(stdout, "psxrecomp ORACLE: interpreter mode (port %d)\n", DEFAULT_DEBUG_PORT);
    interp_init(&cpu);
    interp_trace_enable(1);

    /* Breakpoints focused on VSync diagnostics.
     * Only break on the VSync incrementer — count how many times it's hit. */
    interp_break_add(0x8005A5BCu);  /* VSync counter incrementer — KSEG0 */

    std::fprintf(stderr, "ORACLE: running with BP at 0x8005A5BC (VSync incrementer)...\n");
    std::fflush(stderr);

    uint64_t total_executed = 0;
    const uint64_t max_total = 100000000ULL; /* 100M instructions */
    uint32_t vsync_hits = 0;
    for (;;) {
        uint32_t ran = interp_step(&cpu, 1000000);
        total_executed += ran;
        if (interp_hit_breakpoint()) {
            vsync_hits++;
            if (vsync_hits <= 3 || (vsync_hits % 100 == 0)) {
                std::fprintf(stderr, "ORACLE: VSync hit #%u at %llu instructions, ra=0x%08X, gte_exec=%llu\n",
                             vsync_hits, (unsigned long long)total_executed, cpu.gpr[31],
                             (unsigned long long)gte_get_exec_count());
                /* Dump last 20 trace entries for first 3 hits. */
                if (vsync_hits <= 3) {
                    uint64_t tseq = interp_trace_count();
                    uint32_t tavail = (tseq < 1048576ULL) ? (uint32_t)tseq : 1048576u;
                    uint32_t tstart = (tavail > 20) ? tavail - 20 : 0;
                    for (uint32_t ti = tstart; ti < tavail; ti++) {
                        const InterpTraceEntry* e = interp_trace_get(ti);
                        if (e) std::fprintf(stderr, "  [%llu] PC=0x%08X insn=0x%08X ra=0x%08X v0=0x%08X\n",
                                           (unsigned long long)e->seq, e->pc, e->insn,
                                           e->gpr[31], e->gpr[2]);
                    }
                }
                std::fflush(stderr);
            }
            /* Step past the breakpoint: temporarily remove, step 1, re-add. */
            uint32_t bp_pc = cpu.pc;
            interp_break_remove(bp_pc);
            interp_step(&cpu, 1);
            total_executed++;
            interp_break_add(bp_pc);
        }
        if (ran == 0) {
            std::fprintf(stderr, "ORACLE: halted at PC=0x%08X after %llu total instructions\n",
                         cpu.pc, (unsigned long long)total_executed);
            break;
        }
        if (total_executed >= max_total) {
            std::fprintf(stderr, "ORACLE: reached %llu instructions, stopping. PC=0x%08X\n",
                         (unsigned long long)total_executed, cpu.pc);
            /* Dump last 30 trace entries (ring-relative). */
            uint64_t tseq2 = interp_trace_count();
            uint32_t tavail2 = (tseq2 < 1048576ULL) ? (uint32_t)tseq2 : 1048576u;
            uint32_t tstart2 = (tavail2 > 30) ? tavail2 - 30 : 0;
            for (uint32_t ti = tstart2; ti < tavail2; ti++) {
                const InterpTraceEntry* e = interp_trace_get(ti);
                if (e) std::fprintf(stderr, "  [%llu] PC=0x%08X insn=0x%08X ra=0x%08X\n",
                                   (unsigned long long)e->seq, e->pc, e->insn, e->gpr[31]);
            }
            std::fflush(stderr);
            break;
        }
        /* Poll debug server. */
        debug_server_poll();
    }
    /* Read VSync counter from RAM. */
    uint32_t vsync_counter = cpu.read_word(0x80079D9Cu);
    uint32_t init_flag_48 = cpu.read_word(0x80079D48u);
    uint32_t init_flag_4C = cpu.read_word(0x80079D4Cu);
    std::fprintf(stderr, "ORACLE: VSync counter = 0x%08X (%u), init_flag@48 = 0x%08X, init_flag@4C = 0x%08X\n",
                 vsync_counter, vsync_counter, init_flag_48, init_flag_4C);
    std::fprintf(stderr, "ORACLE: VSync incrementer hit count = %u\n", vsync_hits);
    std::fflush(stderr);
#else
    psx_dispatch(&cpu, cpu.pc);
#endif

    /* If we reach here, all execution completed without MMIO abort. */
    std::fprintf(stdout, "psxrecomp runtime: execution completed, PC=0x%08X\n", cpu.pc);

    shutdown_runtime();
    if (g_gl_active) gl_renderer_shutdown();
    SDL_DestroyTexture(sdl_texture);   /* NULL-safe in GL mode */
    SDL_DestroyRenderer(sdl_renderer); /* NULL-safe in GL mode */
    SDL_DestroyWindow(sdl_window);
    SDL_Quit();

    return 0;
}
