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
#include "code_provider.h"
#include "overlay_sljit.h"
#include "overlay_backend.h"
#include "gpu.h"
#include "gpu_sw_renderer.h"
#include "gpu_render.h"
#include "gpu_gl_renderer.h"
#include "gpu_vk_renderer.h"
#include "frame_pacing.h"
#include "latency_ring.h"
#include "sio.h"
#include "spu.h"
#include "spu_shadow.h"
#include "memcard.h"
#include "debug_server.h"
#include "crash_trace.h"
#include "freeze_heartbeat.h"
#include "config_loader.h"
#include "game_options.h"
#include "crc32.h"
#include "disc_identity.h"
#if defined(PSX_LAUNCHER)
#include "launcher.h"
#endif
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
extern "C" int  mdec_recently_active(uint32_t within_frames);

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
/* Guest-side data-read wrappers: same as psx_read_* but charge PS1 main-RAM
 * read wait states (R3000A has no D-cache). Wired to cpu->read_* below so the
 * timing applies to recompiled + interpreted guest loads, not debug/device reads. */
extern "C" uint32_t psx_guest_read_word(uint32_t addr);
extern "C" uint16_t psx_guest_read_half(uint32_t addr);
extern "C" uint8_t  psx_guest_read_byte(uint32_t addr);

/* ---- SDL state ---- */
static SDL_Window*   sdl_window;
static SDL_Renderer* sdl_renderer;
static SDL_Texture*  sdl_texture;
/* Per-player input device routing (PSX ports 1 & 2). Seeded from the
 * [controller] settings the launcher writes; the runtime opens the matching
 * SDL controller (or uses the keyboard) and feeds each PSX pad slot. */
struct PlayerInput {
    int   kind = 0;            /* 0=none, 1=keyboard, 2=controller */
    char  guid[40] = {0};      /* SDL joystick GUID string when kind==controller */
    /* Pad input mode (PSXRecompV4::PadMode): 0=hybrid (default), 1=analog,
     * 2=digital. hybrid_analog is the per-frame auto-switch latch used only in
     * hybrid mode: true => currently presenting DualShock (stick was the last
     * input), false => currently presenting a digital pad (D-pad was last). */
    int   mode = PSXRecompV4::PAD_MODE_HYBRID;
    bool  hybrid_analog = false;
    SDL_GameController* handle = nullptr;
    SDL_JoystickID      instance = -1;
};
static PlayerInput g_players[2];
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
static int           g_fullscreen     = 0;  /* launch the game window in desktop fullscreen */
static int           g_video_screen   = 0;  /* 0=raw,1=crt,2=composite,3=trinitron */
static int           g_video_win_w    = 1280; /* window width (height follows aspect) */
static bool          g_audio_spu_hq   = false; /* SPU float-shadow (env overrides) */
static int           g_auto_skip_fmv  = 0;   /* skip FMVs the instant they're detected */
/* FMV instant-skip via the game's OWN end-of-movie path. Tomba's MDEC player
 * (FUN_8001efe8) tears a movie down when the streamed frame number reaches that
 * movie's per-movie total minus 3; writing the current movie's total down to
 * g_fmv_skip_end_total makes the player end it on the next frame — a natural end
 * that reaches EVERY movie. g_fmv_skip_total_table = base of the per-movie u16
 * total table; g_fmv_skip_movie_id = guest addr of the current-movie-id byte.
 * 0 table => fall back to START injection. See [video] fmv_skip_* in game.toml. */
static uint32_t      g_fmv_skip_total_table = 0;
static uint32_t      g_fmv_skip_movie_id    = 0;
static int           g_fmv_skip_end_total   = 3;
/* Low-latency present options (see [video] in config_loader). Measured on a
 * 60Hz box the dominant input->photon cost is NOT vsync at the swap (that
 * blocks ~tens of us) but that input is sampled ~one pacer-wait (~13.6ms)
 * before the frame the CPU then renders from it. g_low_latency_input re-samples
 * the pad AFTER the wall-clock pacer (just before present) so the next CPU frame
 * reads near-fresh input. g_video_vsync controls the GL swap interval
 * (1=vsync/tear-free, 0=immediate/lowest display latency+tearing, -1=adaptive);
 * it trims the display-side scanout latency the CPU-side ring can't see. */
static int           g_low_latency_input = 1;
static int           g_video_vsync        = 1;

/* FMV auto-skip detection hooks (cdrom.c / mdec.c). */
extern "C" int      cdrom_xa_stream_active(void);
extern "C" uint32_t mdec_get_decode_count(void);

/* Display aspect W:H (default 4:3 = native). Wider aspects enable the
 * widescreen hack: GTE X-squash + stretched present (see [video] aspect_ratio
 * in config_loader.h). */
static int           g_video_aspect_num = 4;
static int           g_video_aspect_den = 3;
/* [widescreen] per-game hooks (see config_loader.h): anchor scratch addr for
 * tagged sprite prims + HUD SPRT center-squash. Inert at 0/false. */
static uint32_t      g_ws_anchor_addr = 0;
static bool          g_ws_hud_sprt = false;
/* Widescreen engages at game entry (fntrace_is_game_started): the BIOS boot
 * — Sony logo, PS logo, shell — presents authentic 4:3 with no GTE squash.
 * Starts true when the configured aspect is already 4:3 (nothing to engage). */
static bool          g_ws_engaged = true;
/* Wide-aspect strategy: native-wide (render the wider FOV into a wider frame,
 * present 1:1 — the GTE is NOT squashed) vs. the legacy squash hack. Default
 * native-wide; toggle live via the ws_nw TCP command for A/B comparison. */
static int           g_ws_native_wide = 1;
/* Logical present width for the SDL_Renderer (software) path; 640*scale at
 * 4:3, wider for wide aspects. Height is always 480*scale. Set at window
 * creation alongside SDL_RenderSetLogicalSize. */
static int           g_logical_w = 640;

/* Clamp a requested window width to the primary display's usable area so an
 * oversized choice (e.g. 1920 on a 1080p panel) still fits on screen. Keeps
 * the given aspect: height = width*den/num. */
static void clamp_window_aspect(int* w, int* h, int num, int den) {
    int width = *w;
    if (width < 640) width = 640;
    SDL_Rect bounds;
    if (SDL_GetDisplayUsableBounds(0, &bounds) == 0 && bounds.w > 0 && bounds.h > 0) {
        if (width > bounds.w)             width = bounds.w;
        if (width * den / num > bounds.h) width = bounds.h * num / den;
    }
    *w = width;
    *h = width * den / num;
}

/* Live native-wide vs squash toggle (ws_nw TCP command) for A/B comparison.
 * Re-engages with the chosen mode in place if widescreen is already running. */
extern "C" void psx_ws_set_native_wide(int on) {
    g_ws_native_wide = on ? 1 : 0;
    if (g_ws_engaged && g_video_aspect_num * 3 != g_video_aspect_den * 4) {
        int mode = g_ws_native_wide ? 2 : 1;
        gte_set_display_aspect(mode == 1 ? g_video_aspect_num : 4,
                               mode == 1 ? g_video_aspect_den : 3);
        gpu_ws_configure(g_video_aspect_num, g_video_aspect_den,
                         g_ws_anchor_addr, g_ws_hud_sprt ? 1 : 0, mode);
    }
}
extern "C" int psx_ws_get_native_wide(void) { return g_ws_native_wide; }

static bool          g_gl_active = false;    /* GL context live -> GL present path */
static bool          g_vk_active = false;    /* Vulkan context live -> VK present path */
/* Present straight from the FBO (fast, no readback). Set PSX_GL_FORCE_CPU_PRESENT=1
 * to force the software readout path instead — a diagnostic/fallback that also
 * keeps CPU VRAM current every frame (so screenshots reflect the screen). */
static int           g_gl_fbo_present = 1;

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
/* Engage turbo only after a load has been continuously in progress for this many
 * frames. Filters brief incidental reads (e.g. a boss/stage-select screen that
 * streams XA music and reads a few preview sectors as you hover): each 1-2 frame
 * blip would otherwise flip turbo on, muting the music for the audio hangover and
 * stuttering the frame rate. Real loads hold for hundreds of frames, so they
 * lose only this brief authentic-paced prefix. */
#define TURBO_LOADS_ENGAGE_FRAMES 20
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
    // Inside an AppImage the binary lives in a read-only mount and argv[0] can
    // be a bare name or symlink (launched via PATH / a .desktop file). $APPIMAGE
    // is the .AppImage's own path, so settings.toml anchors next to it. Prefer
    // it; fall back to argv[0], then cwd. (settings.toml is read/written ONLY
    // here — one location next to the exe, no directory walk-up.)
    if (const char* appimg = std::getenv("APPIMAGE"); appimg && appimg[0]) {
        exe_dir = fs::absolute(appimg, ec).parent_path();
        if (ec) exe_dir.clear();
    }
    if (exe_dir.empty() && argv0 && argv0[0]) {
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

static bool read_at(std::ifstream& f, uint64_t offset, uint8_t* out, size_t len) {
    f.clear();
    f.seekg((std::streamoff)offset, std::ios::beg);
    if (!f.good()) return false;
    f.read(reinterpret_cast<char*>(out), (std::streamsize)len);
    return f.gcount() == (std::streamsize)len;
}

struct DiscValidation {
    bool opened = false;
    bool has_header = false;
    bool id_matches = false;
    std::string detail;
};

static DiscValidation validate_disc_image(const std::filesystem::path& selected_path,
                                          const std::string& game_id) {
    // Delegate to the shared disc-identity module so the launch-time check and
    // the launcher badge can never drift apart. No CRC here — the launch check
    // only cares about openability / header / serial; the launcher does the
    // (slower) CRC pass when an expected CRC is configured.
    const PSXRecompV4::DiscIdentity id =
        PSXRecompV4::identify_disc(selected_path, game_id, /*expected_crc*/0,
                                   /*has_expected_crc*/false, /*compute_crc*/false);
    DiscValidation v;
    v.opened     = id.opened;
    v.has_header = id.has_header;
    v.id_matches = game_id.empty() ? true : id.serial_matches;
    v.detail     = id.detail;
    if (id.opened && id.has_header && !v.id_matches && v.detail.empty()) {
        v.detail = "The disc header is readable, but it does not contain the expected game ID " +
                   uppercase_ascii(game_id) + " in the early disc metadata.";
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

extern "C" void overlay_compile_worker_stop(void);
static void shutdown_runtime(void) {
    overlay_compile_worker_stop();   /* signal + join the off-thread sljit worker */
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

/* Linear gain ramp g0 -> g1 across a block of interleaved stereo frames. */
static void sdl_audio_gain_ramp(int16_t* buf, int frames, float g0, float g1) {
    if (frames <= 0) return;
    const float step = (g1 - g0) / (float)frames;
    float g = g0;
    for (int f = 0; f < frames; f++, g += step) {
        buf[f * 2 + 0] = (int16_t)((float)buf[f * 2 + 0] * g);
        buf[f * 2 + 1] = (int16_t)((float)buf[f * 2 + 1] * g);
    }
}

/* Fade-in state: samples of rising ramp still to apply after an unmute.
 * Consumed by sdl_audio_pump across however many pump calls it spans.
 * MUST fit sdl_audio_buf (2048 frames): the fade-out tail renders this many
 * frames in one spu_render call. 1764 frames = 40 ms. */
static const int sdl_audio_fade_samples = 44100 * 40 / 1000;  /* 40 ms */
static int       sdl_audio_fadein_left  = 0;

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
    if (sdl_audio_fadein_left > 0) {
        const float g0 = 1.0f - (float)sdl_audio_fadein_left
                                / (float)sdl_audio_fade_samples;
        int ramp = sdl_audio_fadein_left < frames ? sdl_audio_fadein_left : frames;
        const float g1 = 1.0f - (float)(sdl_audio_fadein_left - ramp)
                                / (float)sdl_audio_fade_samples;
        sdl_audio_gain_ramp(sdl_audio_buf, ramp, g0, g1);
        sdl_audio_fadein_left -= ramp;
    }
    SDL_QueueAudio(sdl_audio_device, sdl_audio_buf,
                   (uint32_t)frames * bytes_per_frame);
}

/* Audio gating across turbo-loads transitions.
 *
 * The mute model stays: during turbo the guest runs at host speed, so
 * rendered SPU audio is time-compressed garble — we stop pumping, the queue
 * drains, voice positions freeze, and music resumes in place afterward.
 * What changes is the EDGES:
 *   - entering turbo: render one short tail of the current voice state,
 *     ramp it to silence, and queue it — the drain ends in a fade instead
 *     of a hard cut;
 *   - leaving turbo: hold the mute for a short hangover first (loads often
 *     re-trigger within a few frames; without the debounce the mute would
 *     flicker audibly), then resume pumping with a rising ramp applied
 *     across the first ~50 ms of samples (sdl_audio_pump above). */
static void sdl_audio_update(int turbo_active) {
    if (!sdl_audio_device) return;
    const int HANGOVER_FRAMES = 8;  /* ~133 ms at 60 fps */
    static int muted = 0;
    static int hangover = 0;

    if (turbo_active) {
        if (!muted) {
            int tail = sdl_audio_fade_samples;
            const int buf_cap = (int)(sizeof(sdl_audio_buf) / (2 * sizeof(int16_t)));
            if (tail > buf_cap) tail = buf_cap;
            /* An unmute ramp may still be in progress; start the down-ramp
             * from its current gain so the edge stays continuous. */
            const float g0 = 1.0f - (float)sdl_audio_fadein_left
                                    / (float)sdl_audio_fade_samples;
            sdl_audio_fadein_left = 0;
            spu_render(sdl_audio_buf, tail);
            sdl_audio_gain_ramp(sdl_audio_buf, tail, g0, 0.0f);
            SDL_QueueAudio(sdl_audio_device, sdl_audio_buf,
                           (uint32_t)tail * sizeof(int16_t) * 2u);
            muted = 1;
        }
        hangover = HANGOVER_FRAMES;
        return;
    }
    if (muted) {
        if (hangover > 0) { hangover--; return; }
        muted = 0;
        sdl_audio_fadein_left = sdl_audio_fade_samples;
    }
    sdl_audio_pump();
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

static void close_player(PlayerInput& p) {
    if (p.handle) {
        SDL_GameControllerClose(p.handle);
        p.handle = nullptr;
        p.instance = -1;
    }
}

static void close_controller(void) {
    close_player(g_players[0]);
    close_player(g_players[1]);
}

/* Open the SDL controller whose GUID matches p.guid. If no exact GUID match
 * exists (e.g. a different physical unit of the same model, or Steam's virtual
 * pad at an unpredictable slot), fall back to the first controller not already
 * claimed by the other player. */
static void open_player(PlayerInput& p, const PlayerInput& other) {
    if (p.kind != 2 || p.handle) return;

    int chosen = -1, fallback = -1;
    const int joysticks = SDL_NumJoysticks();
    for (int i = 0; i < joysticks; i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_JoystickGUID g = SDL_JoystickGetDeviceGUID(i);
        char buf[40] = {0};
        SDL_JoystickGetGUIDString(g, buf, sizeof(buf));
        /* Skip a device already opened by the other player. */
        SDL_JoystickID inst = SDL_JoystickGetDeviceInstanceID(i);
        if (other.handle && other.instance == inst) continue;
        if (p.guid[0] && std::strcmp(buf, p.guid) == 0) { chosen = i; break; }
        if (fallback < 0) fallback = i;
    }
    if (chosen < 0) chosen = fallback;
    if (chosen < 0) return;

    p.handle = SDL_GameControllerOpen(chosen);
    if (p.handle) {
        SDL_Joystick* joy = SDL_GameControllerGetJoystick(p.handle);
        p.instance = joy ? SDL_JoystickInstanceID(joy) : -1;
        const char* name = SDL_GameControllerName(p.handle);
        std::fprintf(stdout, "psxrecomp runtime: opened controller for slot: %s\n",
                     name ? name : "(unnamed)");
    }
}

/* The pad type a mode reports before any input has been sampled (boot /
 * hotplug). analog pins DualShock; digital and hybrid both start as a digital
 * pad (hybrid only flips to DualShock once the player nudges the stick). */
static int pad_mode_boot_analog(int mode) {
    /* Hybrid boots ANALOG-on (matches a DualShock powered up with the analog LED
     * lit): the seamless auto-switch then drops to digital only if the player
     * reaches for the d-pad. Pinned-analog also boots analog; digital boots off. */
    return (mode == PSXRecompV4::PAD_MODE_ANALOG ||
            mode == PSXRecompV4::PAD_MODE_HYBRID) ? 1 : 0;
}

/* Open/close SDL handles so they match g_players, and (re)assert each slot's
 * PSX connection + pad type. Safe to call repeatedly (hotplug, boot). */
static void refresh_player_devices(void) {
    for (int s = 0; s < 2; s++) {
        PlayerInput& p = g_players[s];
        if (p.kind != 2) close_player(p);           /* keyboard/none: no handle */
        else open_player(p, g_players[s ^ 1]);
        sio_set_pad_connected(s, p.kind != 0 ? 1 : 0);
        sio_set_pad_analog(s, pad_mode_boot_analog(p.mode), 0x80, 0x80, 0x80, 0x80);
    }
}

/* Parse a [controller] device string into a player slot:
 *   "none" -> no pad; "keyboard" -> keyboard map; otherwise an SDL GUID. */
static void set_player_device(PlayerInput& p, const std::string& dev, int mode) {
    p.mode = mode;
    /* Hybrid starts in ANALOG (analog LED on); the auto-switch drops to digital
     * only when the player uses the d-pad. */
    p.hybrid_analog = true;
    p.guid[0] = '\0';
    std::string d = lower_copy(trim_copy(dev));
    if (d.empty() || d == "none") { p.kind = 0; }
    else if (d == "keyboard")     { p.kind = 1; }
    else if (d == "auto" || d == "gamepad" || d == "controller") {
        /* First available SDL game controller (guid empty -> open_player falls
         * back to the first connected pad). Lets a user default to "my
         * controller" without pinning a specific GUID. */
        p.kind = 2;  /* p.guid already cleared above */
    }
    else {
        p.kind = 2;
        std::snprintf(p.guid, sizeof(p.guid), "%s", trim_copy(dev).c_str());
    }
}

static bool controller_source_pressed_h(SDL_GameController* h, const ControllerSource& source) {
    if (!h) return false;

    switch (source.kind) {
    case ControllerSource::Kind::Button:
        return SDL_GameControllerGetButton(
            h, (SDL_GameControllerButton)source.id) != 0;
    case ControllerSource::Kind::AxisPositive:
        return SDL_GameControllerGetAxis(
            h, (SDL_GameControllerAxis)source.id) > controller_deadzone;
    case ControllerSource::Kind::AxisNegative:
        return SDL_GameControllerGetAxis(
            h, (SDL_GameControllerAxis)source.id) < -controller_deadzone;
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

static uint16_t controller_pad_buttons(SDL_GameController* h) {
    uint16_t buttons = 0xFFFF;  /* all released */
    if (!h) return buttons;
    for (const auto& entry : controller_map) {
        for (const auto& source : entry.sources) {
            if (controller_source_pressed_h(h, source)) {
                buttons &= (uint16_t)~entry.bit;
                break;
            }
        }
    }
    return buttons;
}

/* Map an SDL axis (-32768..32767) to a PSX analog byte (0..255, 0x80 centred),
 * applying the configured centre deadzone: travel within controller_deadzone
 * reads as centred (0x80) and the remaining travel is rescaled to the full range
 * so the stick still reaches the extremes. Gives clean variable analog speed
 * with no centre drift. */
static uint8_t axis_to_pad_byte(int16_t v) {
    const int dz = controller_deadzone;
    int av = v < 0 ? -(int)v : (int)v;        /* |v|, 0..32768 */
    if (av <= dz) return 0x80;                /* inside deadzone -> centred */
    int range = 32767 - dz;
    if (range < 1) range = 1;
    int mag = (av - dz) * 32767 / range;      /* 0..32767 past the deadzone */
    if (mag > 32767) mag = 32767;
    int sv = v < 0 ? -mag : mag;
    int b = (sv + 32768) >> 8;                /* 0..255 */
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint8_t)b;
}

/* Buttons for a player's selected device (0xFFFF = none pressed). */
static uint16_t pad_buttons_for(const PlayerInput& p) {
    if (p.kind == 1) return pad_from_keyboard();
    if (p.kind == 2) return controller_pad_buttons(p.handle);
    return 0xFFFF;
}

/* Analog stick bytes (lx,ly,rx,ry) for a player; centred if no live source.
 *
 * The host left stick maps to the LEFT analog axes (variable). When fold_dpad
 * is set, the physical D-pad (and, for a keyboard player, the arrow keys) is
 * ALSO folded onto the left axes at full deflection, so an analog-mode game
 * whose movement reads only the stick magnitude (e.g. Tomba) still responds to
 * the D-pad/keyboard — the faithful "seamless" behaviour: the stick gives
 * variable speed, the D-pad gives full speed, both at once. Only the PHYSICAL
 * D-pad is folded in (not the stick->d-pad button mapping), so the stick keeps
 * its true variable magnitude. fold_dpad is false in HYBRID mode (there the
 * D-pad instead flips the pad to digital, so the game runs its own d-pad path)
 * and true in pinned-ANALOG mode. The keyboard branch always folds the arrows
 * — they are that player's only stick source. */
static void pad_sticks_for(const PlayerInput& p, uint8_t out[4], bool fold_dpad) {
    out[0] = out[1] = out[2] = out[3] = 0x80;
    if (p.kind == 1) {
        const Uint8* keys = SDL_GetKeyboardState(NULL);
        if (keys[SDL_SCANCODE_LEFT])  out[0] = 0x00;
        if (keys[SDL_SCANCODE_RIGHT]) out[0] = 0xFF;
        if (keys[SDL_SCANCODE_UP])    out[1] = 0x00;
        if (keys[SDL_SCANCODE_DOWN])  out[1] = 0xFF;
        return;
    }
    if (p.kind == 2 && p.handle) {
        out[0] = axis_to_pad_byte(SDL_GameControllerGetAxis(p.handle, SDL_CONTROLLER_AXIS_LEFTX));
        out[1] = axis_to_pad_byte(SDL_GameControllerGetAxis(p.handle, SDL_CONTROLLER_AXIS_LEFTY));
        out[2] = axis_to_pad_byte(SDL_GameControllerGetAxis(p.handle, SDL_CONTROLLER_AXIS_RIGHTX));
        out[3] = axis_to_pad_byte(SDL_GameControllerGetAxis(p.handle, SDL_CONTROLLER_AXIS_RIGHTY));
        if (fold_dpad) {
            if (SDL_GameControllerGetButton(p.handle, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  out[0] = 0x00;
            if (SDL_GameControllerGetButton(p.handle, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) out[0] = 0xFF;
            if (SDL_GameControllerGetButton(p.handle, SDL_CONTROLLER_BUTTON_DPAD_UP))    out[1] = 0x00;
            if (SDL_GameControllerGetButton(p.handle, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  out[1] = 0xFF;
        }
    }
}

/* HYBRID-mode auto-switch detectors (mirror the DualShock analog LED toggling).
 * hybrid_stick_active: the LEFT stick is deflected past the deadzone — the
 * player has reached for analog. hybrid_dpad_active: any D-pad direction (or,
 * for the keyboard, an arrow key) is held — the player wants classic digital.
 * The keyboard has no analog stick, so a keyboard player stays digital. */
static bool hybrid_stick_active(const PlayerInput& p) {
    if (p.kind != 2 || !p.handle) return false;
    const int dz = controller_deadzone;
    int lx = SDL_GameControllerGetAxis(p.handle, SDL_CONTROLLER_AXIS_LEFTX);
    int ly = SDL_GameControllerGetAxis(p.handle, SDL_CONTROLLER_AXIS_LEFTY);
    return lx > dz || lx < -dz || ly > dz || ly < -dz;
}
static bool hybrid_dpad_active(const PlayerInput& p, bool kb_always) {
    if (p.kind == 2 && p.handle) {
        if (SDL_GameControllerGetButton(p.handle, SDL_CONTROLLER_BUTTON_DPAD_LEFT)  ||
            SDL_GameControllerGetButton(p.handle, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ||
            SDL_GameControllerGetButton(p.handle, SDL_CONTROLLER_BUTTON_DPAD_UP)    ||
            SDL_GameControllerGetButton(p.handle, SDL_CONTROLLER_BUTTON_DPAD_DOWN))
            return true;
    }
    if (p.kind == 1 || kb_always) {
        const Uint8* keys = SDL_GetKeyboardState(NULL);
        if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_RIGHT] ||
            keys[SDL_SCANCODE_UP]   || keys[SDL_SCANCODE_DOWN])
            return true;
    }
    return false;
}

/* Sample each player's live device state into the matching SIO pad slot.
 * override >= 0 forces port-1 buttons (debug-server input injection). Called
 * once at cycle start (covers the turbo/FMV-skip paths that present nothing)
 * and, when g_low_latency_input is set, AGAIN after the pacer wait so the next
 * CPU frame reads near-fresh input instead of input ~one frame stale.
 *
 * HYBRID mode auto-switches DualShock<->digital from the most-recent input
 * (stick -> analog, D-pad -> digital) so the game runs its own analog or
 * digital input path exactly as on hardware (mirrors the inline block this
 * helper was extracted from for the low-latency re-sample). */
/* Dev builds (PSX_DEBUG_TOOLS — the dev/debug configuration) ALWAYS feed the
 * keyboard into Player 1 alongside whatever device the launcher routed to port 1,
 * so a tester can drive the game from EITHER the keyboard OR the selected
 * controller (e.g. a DualShock) at the same time, with no reconfiguration. The
 * keyboard map and the controller are merged (active-low AND) onto the P1 pad
 * word. Release builds keep the single launcher-selected device per port. */
#if defined(PSX_DEBUG_TOOLS)
static const bool g_dev_kb_p1 = true;
#else
static const bool g_dev_kb_p1 = false;
#endif

static void sample_pad_into_sio(int override) {
    if (override >= 0) {
        sio_set_pad_state_slot(0, (uint16_t)override);
        return;
    }
    for (int s = 0; s < 2; s++) {
        PlayerInput& p = g_players[s];
        const bool kb_here = (g_dev_kb_p1 && s == 0);
        if (p.kind == 0 && !kb_here) continue;  /* no device in this port */

        /* Buttons: merge the assigned device with the keyboard (PSX pad word is
         * active-low, so AND combines "pressed on either source"). */
        uint16_t btn = (p.kind != 0) ? pad_buttons_for(p) : (uint16_t)0xFFFF;
        if (kb_here) btn &= pad_from_keyboard();
        sio_set_pad_state_slot(s, btn);

        /* Resolve the pad type this frame from the player's mode (a port driven
         * by the keyboard alone presents as a digital pad). */
        const int mode = (p.kind != 0) ? p.mode : (int)PSXRecompV4::PAD_MODE_DIGITAL;
        int eff_analog;
        uint8_t st[4] = { 0x80, 0x80, 0x80, 0x80 };
        if (mode == PSXRecompV4::PAD_MODE_DIGITAL) {
            eff_analog = 0;
        } else if (mode == PSXRecompV4::PAD_MODE_ANALOG) {
            eff_analog = 1;
            pad_sticks_for(p, st, /*fold_dpad=*/true);
        } else { /* HYBRID */
            if (hybrid_stick_active(p))               p.hybrid_analog = true;
            else if (hybrid_dpad_active(p, kb_here))  p.hybrid_analog = false;
            eff_analog = p.hybrid_analog ? 1 : 0;
            if (eff_analog) pad_sticks_for(p, st, /*fold_dpad=*/false);
        }
        /* In dev builds also fold the keyboard arrows onto the analog stick, so an
         * analog-mode P1 still steers from the keyboard. */
        if (kb_here && eff_analog) {
            const Uint8* keys = SDL_GetKeyboardState(NULL);
            if (keys[SDL_SCANCODE_LEFT])  st[0] = 0x00;
            if (keys[SDL_SCANCODE_RIGHT]) st[0] = 0xFF;
            if (keys[SDL_SCANCODE_UP])    st[1] = 0x00;
            if (keys[SDL_SCANCODE_DOWN])  st[1] = 0xFF;
        }
        /* Push sticks every frame; request the pad type (digital/analog) through
         * the coherent channel so a hybrid stick<->d-pad flip is applied only at
         * an idle, non-config bus boundary (never mid-poll / mid-handshake). This
         * is the fix for the v0.5.0 phantom-input regression: slamming the type
         * each frame raced Tomba's DualShock config handshake -> garbage button
         * reads. eff_analog still reflects this frame's mode (digital / analog /
         * hybrid auto-switch). */
        sio_set_pad_sticks(s, st[0], st[1], st[2], st[3]);
        sio_request_pad_type(s, eff_analog);
    }
}

/* PSX native vblank cadence: NTSC ≈ 59.94 Hz. Wall-clock target keeps
 * audio sample generation (735 samples/vblank * 60 = 44100/sec) matched
 * to the SDL audio device drain rate, eliminating queue overflow drops
 * and underruns. Uncapped, the host runs the simulation at whatever
 * speed it can — typically several × realtime — and audio glitches. */
static constexpr double PSX_FRAME_PERIOD_MS = 1000.0 / 59.94;

/* ── Host-stack-usage profile (RECURSION_BUG.md §17) ──────────────────────────
 * The decisive instrument for the long-run freeze. The guest call graph mirrors
 * onto the 1MB guest fiber stack (traps.c), and the native-stack guard trips at
 * ~768KB used. The open question is the SHAPE of host-stack usage over frames:
 *   - LINEAR climb from gameplay start  => a per-frame interp<->compiled boundary
 *     leak: every game tick leaks one un-unwound host call chain (~13 frames),
 *     overflowing the fiber after ~50k frames. 50k is a CAPACITY limit, not a
 *     trigger. (Model A — predicted.)
 *   - FLAT, then a cliff at one frame   => a within-one-frame runaway re-entry at
 *     a real ~frame-50k trigger. (Model W — the old §15 premise.)
 * (TEB StackBase - rsp), sampled on the guest fiber each vblank, IS the leaked
 * depth. Sampled in sdl_vblank_present — a gpu_vblank_tick callback off
 * psx_check_interrupts, i.e. ON the guest fiber, every vblank, in both builds.
 * Decimated 1-per-STRIDE frames so the ring spans the whole run; "now" carries
 * the exact latest sample. ALWAYS-ON: read live via the `stack_profile` TCP
 * command while the game is still responsive (no need to reach the overflow),
 * and dumped in the crash report. Query the ring; never arm-and-capture. */
#if defined(_WIN32)
#include <intrin.h>   /* __readgsqword — fiber TEB StackBase */
static size_t host_stack_used(void) {
    char probe;
    uintptr_t base = (uintptr_t)__readgsqword(0x08);   /* TEB StackBase (high) */
    uintptr_t sp   = (uintptr_t)&probe;
    return (base > sp) ? (size_t)(base - sp) : 0;
}
#else
static size_t host_stack_used(void) { return 0; }
#endif

#define STACK_PROFILE_CAP    512u
#define STACK_PROFILE_STRIDE 128u   /* 512 * 128 = 65536 frames of coverage */
typedef struct { uint32_t frame; uint32_t used_kb; } StackSample;
static StackSample g_stack_prof[STACK_PROFILE_CAP];
static uint32_t    g_stack_prof_seq    = 0;
static uint32_t    g_stack_used_now_kb = 0;
static uint32_t    g_stack_frame_now   = 0;
static uint32_t    g_stack_used_max_kb = 0;

static void stack_profile_sample(void) {
    extern uint64_t s_frame_count;
    uint32_t f  = (uint32_t)s_frame_count;
    uint32_t kb = (uint32_t)(host_stack_used() >> 10);
    g_stack_frame_now   = f;
    g_stack_used_now_kb = kb;
    if (kb > g_stack_used_max_kb) g_stack_used_max_kb = kb;
    if ((f % STACK_PROFILE_STRIDE) == 0) {
        StackSample *e = &g_stack_prof[g_stack_prof_seq++ & (STACK_PROFILE_CAP - 1u)];
        e->frame = f; e->used_kb = kb;
    }
}

/* JSON dump of the profile (callable from C: crash report + TCP command). */
extern "C" int stack_profile_json(char *out, int cap) {
    uint32_t total = g_stack_prof_seq;
    uint32_t avail = total < STACK_PROFILE_CAP ? total : STACK_PROFILE_CAP;
    uint32_t start = total - avail;
    int n = snprintf(out, cap,
        "{\"now\":{\"f\":%u,\"kb\":%u},\"max_kb\":%u,\"stride\":%u,\"count\":%u,\"samples\":[",
        g_stack_frame_now, g_stack_used_now_kb, g_stack_used_max_kb,
        STACK_PROFILE_STRIDE, avail);
    for (uint32_t i = 0; i < avail && n < cap - 48; i++) {
        StackSample *e = &g_stack_prof[(start + i) & (STACK_PROFILE_CAP - 1u)];
        n += snprintf(out + n, cap - n, "%s{\"f\":%u,\"kb\":%u}",
                      i ? "," : "", e->frame, e->used_kb);
    }
    n += snprintf(out + n, cap - n, "]}");
    return n;
}

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

    /* Host-stack-usage profile sample — frame counter is now current, and we are
     * on the guest fiber (see §17 block above). BEFORE the turbo/fast-boot early
     * returns so the curve is captured even when presents are skipped. */
    stack_profile_sample();

    /* Step 2.8 automation ticks — BEFORE the turbo/fast-boot early returns
     * below, so capture detection and compile pickup keep running while the
     * frontend is skipping presents. Both are cheap and emu-thread-only. */
    overlay_autocapture_tick();
    {   /* Apply a finished batch compile via the active code provider (gcc:
         * cache rescan on done; sljit: no-op — it produces synchronously). */
        const CodeProvider *cp = code_provider_active();
        if (cp->poll_main) cp->poll_main();
    }

    /* Pump SDL events to prevent window freeze. */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            psx_crash_trace_set_exit_origin("sdl_window_close");
            shutdown_runtime();
            std::exit(0);
        } else if (ev.type == SDL_CONTROLLERDEVICEADDED) {
            refresh_player_devices();
        } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
            if (ev.cdevice.which == g_players[0].instance ||
                ev.cdevice.which == g_players[1].instance) {
                close_controller();
                refresh_player_devices();
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

    /* Sample each player's device and feed the matching SIO pad slot.
     * Debug server input override (when active) drives port 1 only. With
     * g_low_latency_input this early sample is re-done after the pacer wait
     * (below) for the interactive present path; it still covers the turbo /
     * FMV-skip paths that early-return before pacing. */
    sample_pad_into_sio(override);

    /* Latency ring: open this present cycle's slot, stamping when input was
     * sampled into SIO.  Always-on; queried via the debug server "latency". */
    latency_ring_frame_begin();

    /* Turbo-active test, shared by the audio gate here and the pacing/
     * present gate below. sdl_audio_update owns the mute + fade-in/out +
     * debounce across turbo transitions (see its comment). */
    int turbo_loads_active = 0;
    if (g_turbo_loads_enabled) {
        extern int fntrace_is_game_started(void);
        /* Hysteresis: count consecutive frames the load has held, and only
         * engage turbo once it is SUSTAINED (TURBO_LOADS_ENGAGE_FRAMES). This
         * stops brief incidental reads on music screens (boss/stage select) from
         * flickering turbo on and chopping the audio. cdrom_load_in_progress()
         * already bridges short intra-load gaps (CD_BURST_GAP_FRAMES), so a real
         * load's counter does not reset mid-load. */
        static int load_run = 0;
        if (fntrace_is_game_started() && cdrom_load_in_progress()) {
            if (load_run < (1 << 20)) load_run++;
        } else {
            load_run = 0;
        }
        if (load_run >= TURBO_LOADS_ENGAGE_FRAMES)
            turbo_loads_active = 1;
    }

    /* FMV auto-skip ([video] auto_skip_fmv). A streaming FMV is XA audio + MDEC
     * video together. Detect "MDEC produced a frame since the last vblank AND XA
     * is streaming"; a short hold rides out brief inter-frame gaps. The present/
     * pacing safety-net + audio mute below hide the one or two transition frames.
     *
     * End the movie the GAME's own way, chosen per game:
     *  - fmv_skip_total_table set (Tomba): write the CURRENT movie's per-movie
     *    frame-total down to fmv_skip_end_total, so the player's MDEC loop
     *    (FUN_8001efe8: teardown when streamed frame# >= total - 3) terminates on
     *    its next frame — a NATURAL end that reaches EVERY movie, including ones
     *    whose caller never polls the skip button. Only the active movie's table
     *    entry is touched (via the movie-id byte), so nothing else is disturbed.
     *  - no table configured (generic): hold START so a movie whose handler polls
     *    the pad aborts itself (can't reach unskippable movies). */
    int fmv_skip_active = 0;
    if (g_auto_skip_fmv) {
        static uint32_t s_last_mdec = 0;
        static int      s_fmv_hold  = 0;
        uint32_t mc = mdec_get_decode_count();
        int mdec_decoding = (mc != s_last_mdec);
        s_last_mdec = mc;
        int xa = cdrom_xa_stream_active();
        if (mdec_decoding && xa) s_fmv_hold = 4;
        else if (s_fmv_hold > 0) s_fmv_hold--;
        fmv_skip_active = (s_fmv_hold > 0) && xa;
        if (fmv_skip_active) {
            if (g_fmv_skip_total_table) {
                /* End the active movie via its own frame-count teardown. */
                uint8_t mid = psx_read_byte(g_fmv_skip_movie_id);
                psx_write_half(g_fmv_skip_total_table + (uint32_t)mid * 2u,
                               (uint16_t)g_fmv_skip_end_total);
            } else {
                /* Generic fallback: hold START (PSX pad word is active-low; START
                 * is bit 3) so the game's FMV handler aborts the movie itself. */
                sio_set_pad_state_slot(0, (uint16_t)~(1u << 3));
            }
        }
    }

#ifndef PSX_SDL_NO_AUDIO
    /* Turbo-loads no longer mutes audio. Loads at 1x+turbo are sub-second, and
     * the mute/fade/hangover model produced more audible disruption (cuts and
     * "restarts") than just letting the stream play through; the audio queue is
     * capped (sdl_audio_pump max_queue_bytes) so a brief turbo burst can't build
     * unbounded latency. FMV-skip still mutes: it fast-forwards an entire movie,
     * where rendering the time-compressed audio would be pure noise. */
    sdl_audio_update(fmv_skip_active);
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

    /* FMV auto-skip: run uncapped (no wall-clock pacing) and suppress nearly all
     * presents so the fast-forwarded movie is invisible. Present 1-in-30 only so
     * the window keeps pumping and never looks hung during the brief skip. */
    if (fmv_skip_active) {
        static int fs_skip = 0;
        const int FMV_PRESENT_EVERY = 30;
        fs_skip = (fs_skip + 1) % FMV_PRESENT_EVERY;
        if (fs_skip != 0) return;
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
    latency_ring_mark(LAT_PACED);

    /* Low-latency input: the early sample above is now ~one pacer-wait old.
     * Refresh the device state and re-sample right before present so the next
     * CPU frame reads near-fresh input (the dominant input->photon cost on a
     * vsync-light box). Re-stamp the ring's input mark to measure from here. */
    if (g_low_latency_input) {
        SDL_GameControllerUpdate();  /* refresh pad state after the wait */
        SDL_PumpEvents();            /* refresh keyboard state */
        sample_pad_into_sio(override);
        latency_ring_restamp_input();
    }

    /* Engage widescreen at game entry: BIOS boot stays authentic 4:3. */
    if (!g_ws_engaged) {
        extern int fntrace_is_game_started(void);
        if (fntrace_is_game_started()) {
            g_ws_engaged = true;
            int mode = g_ws_native_wide ? 2 : 1;
            /* Native-wide: GTE drawn un-squashed — feed it the 4:3 ratio
             * (identity squash). Squash mode: feed the real wide aspect. */
            gte_set_display_aspect(mode == 1 ? g_video_aspect_num : 4,
                                   mode == 1 ? g_video_aspect_den : 3);
            gpu_ws_configure(g_video_aspect_num, g_video_aspect_den,
                             g_ws_anchor_addr, g_ws_hud_sprt ? 1 : 0, mode);
        }
    }

    /* ---- Display from our VRAM ---- */
    uint32_t w = 0, h = 0;
    uint32_t present_w = 0;  /* display width actually presented (w + native-wide EXTRA) */
    int active_scale = 1;   /* hi-res mirror used only for 15-bit display */
    bool fmv_frame = false;  /* FMV/boot — present pillarboxed 4:3 in widescreen */
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
                } else if (g_vk_active) {
                    vk_renderer_present_blank();
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
        /* 4:3-pinned frames: the pre-game BIOS boot, plus (once engaged) every
         * frame the widescreen layer presents native — FMV video and full-2D
         * menu/title screens. gpu_ws_present_native_43() is the single source
         * of truth, shared with the GTE/GPU squash so content and present stay
         * locked: we squash IFF we stretch. */
        fmv_frame = !g_ws_engaged || gpu_ws_present_native_43() != 0;

        /* Canonical present width. Native-wide does NOT widen the canonical read
         * (that bled across adjacent framebuffers); it composites into a separate
         * wide surface and presents from there instead (see gpu wide compositor). */
        present_w = w;

        /* Native-wide present: on a game frame, if the active backend has the
         * wide compositor, present the wider surface (canonical width + EXTRA)
         * from the displayed buffer's surface. FMV/menu frames stay 4:3. */
        bool wide_present = (!fmv_frame && g_ws_engaged &&
                             ws_native_wide_active() && gr_wide_supported());
        if (wide_present) present_w = w + (uint32_t)ws_nw_extra();

        /* OpenGL: 15-bit frames ALWAYS present straight from the authoritative
         * VRAM FBO — one deterministic path (the old per-frame FBO-vs-CPU
         * alternation caused the menu seam/jitter). 24-bit (FMV) frames and
         * the PSX_GL_FORCE_CPU_PRESENT diagnostic sync the FBO down and use
         * the CPU readout below. */
#ifndef PSX_SDL_NO_RENDER
        if (g_gl_active && g_gl_fbo_present && !di.depth24) {
            if (wide_present) {
                /* GPU-direct native-wide present: blit the displayed buffer's
                 * wide FBO straight to the window (GPU-side, like the canonical
                 * present_vram). This avoids the glFinish + glReadPixels CPU
                 * round-trip the old path did EVERY frame — that readback was the
                 * GL-only slowdown (SW's wide path is pure CPU, no GPU sync, so
                 * SW stayed smooth). Falls through to the CPU readout path only if
                 * the wide surface for this buffer doesn't exist yet. */
                if (gl_renderer_present_wide_fbo((int)di.display_x, (int)di.display_y,
                                                 (int)h, g_video_aa ? 1 : 0))
                    return;
            } else {
                gl_renderer_present_vram((int)di.display_x, (int)di.display_y,
                                         (int)present_w, (int)h, g_video_aa ? 1 : 0,
                                         fmv_frame ? 1 : 0);
                return;
            }
        }
        if (g_gl_active) gl_renderer_sync_cpu();
        /* Vulkan owns every frame: 15-bit frames present straight from the GPU
         * VRAM image (deterministic blit, no readback), mirroring the GL path;
         * 24-bit (FMV) frames go through the CPU present (Phase 3). The Vulkan
         * window has no SDL_Renderer, so we must never fall through below. */
        if (g_vk_active) {
            if (di.depth24) {
                vk_renderer_present_cpu(NULL, (int)present_w, (int)h,
                                        g_video_aa ? 1 : 0, fmv_frame ? 1 : 0);
            } else if (wide_present &&
                       vk_renderer_present_wide((int)di.display_x, (int)di.display_y,
                                                (int)h, g_video_aa ? 1 : 0)) {
                /* presented wide */
            } else {
                vk_renderer_present_vram((int)di.display_x, (int)di.display_y,
                                         (int)present_w, (int)h, g_video_aa ? 1 : 0,
                                         fmv_frame ? 1 : 0);
            }
            return;
        }
#endif

        /* The hi-res mirror is a 15-bit copy of VRAM; 24-bit display (FMV)
         * reads packed bytes the mirror can't represent, so fall back to the
         * native path for those frames (the present filter still upscales). */
        active_scale = (g_video_scale > 1 && !di.depth24) ? g_video_scale : 1;

        if (wide_present) {
            /* The wide compositor surface is at the renderer's REAL internal
             * scale, which gr_render_wide_display uses for its readback pitch.
             * Use gr_scale() (== that internal scale) rather than g_video_scale:
             * on the GL backend g_video_scale can be stale (1) while the renderer
             * supersamples at 2, and a mismatch makes the present read the wide
             * buffer at the wrong width (a magnified top-left slice). gr_scale()
             * is guaranteed to match the readback. Falls back to the canonical
             * hires path if the displayed buffer has no surface yet. */
            int s = gr_scale();
            int sw = (int)present_w * s;
            int n = gr_render_wide_display(sdl_pixel_buf, (int)(sw * sizeof(uint32_t)),
                                           (int)di.display_x, (int)di.display_y, (int)h);
            if (n > 0) {
                active_scale = s;
            } else {
                wide_present = false;
                present_w = w;
            }
        }
        if (!wide_present) {
            if (active_scale > 1) {
                int sw = (int)present_w * active_scale;
                gr_render_display_hires(sdl_pixel_buf, (int)(sw * sizeof(uint32_t)),
                                        (int)di.display_x, (int)di.display_y,
                                        (int)present_w, (int)h);
            } else {
                for (uint32_t y = 0; y < h; y++) {
                    for (uint32_t x = 0; x < present_w; x++) {
                        sdl_pixel_buf[y * present_w + x] = gpu_display_pixel_argb(&di, x, y);
                    }
                }
            }
        }
    }

    /* Update only the active display rectangle. The backing texture is sized
     * 640x512 (times the supersampling factor), while games can switch to
     * smaller modes such as 320x224 for FMV; presenting the full texture would
     * leave the active image stuck in the upper-left portion of the window. */
#ifndef PSX_SDL_NO_RENDER
    int src_w = (int)present_w * active_scale;
    int src_h = (int)h * active_scale;
    if (g_gl_active) {
        /* OpenGL present: upload the active display rect and draw a full-screen
         * quad. SDL_GL_SwapWindow handles vsync; the wall-clock pacer above
         * still owns timing. 24-bit (FMV) frames pin to native 4:3. */
        gl_renderer_present(sdl_pixel_buf, src_w, src_h, g_video_aa ? 1 : 0,
                            fmv_frame ? 1 : 0);
    } else {
    SDL_Rect src = { 0, 0, src_w, src_h };
    SDL_UpdateTexture(sdl_texture, &src, sdl_pixel_buf,
                      (int)(src_w * sizeof(uint32_t)));

    /* FMV (24-bit) frames are authored 4:3 with no GTE squash to compensate
     * the widescreen stretch — pillarbox them at native 4:3 instead. */
    int dst_w = fmv_frame ? 640 * g_video_scale : g_logical_w;
    SDL_Rect dst = { (g_logical_w - dst_w) / 2, 0, dst_w, 480 * g_video_scale };
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
        latency_ring_mark(LAT_SWAP_BEGIN);
        const Uint64 t0 = SDL_GetPerformanceCounter();
        SDL_RenderPresent(sdl_renderer);
        const Uint64 t1 = SDL_GetPerformanceCounter();
        latency_ring_mark(LAT_SWAP_END);
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
    bool        bios_from_cli = false;  /* CLI --bios/positional wins over settings.toml */
    /* Launcher overrides (mirrors snesrecomp): --launcher forces the GUI back on
     * even when [launcher] skip_launcher = true is set; --no-launcher (and the
     * PSX_NO_LAUNCHER env) forces it off. --launcher wins if both are given. */
    bool        force_launcher    = false;
    bool        force_no_launcher = false;
    /* CLI overrides for running several instances side by side (soak fleet).
     * These win over any game-config value and, crucially, work for the BIOS
     * (which has no [game]-block config schema, so debug_port/renderer can't be
     * supplied via --game). -1 = "not set on the CLI". */
    int         cli_debug_port = -1;
    int         cli_renderer   = -1;   /* 0=software 1=opengl 2=vulkan */
    const char* cli_window_title = nullptr;  /* label windows in a fleet */
    /* Parse args.
     *   --bios <path>       override the compile-time BIOS path
     *   --game <toml>       load a game config (single source of truth for
     *                       disc / memcard / window title / debug port)
     *   --disc <path>       override the game config disc path
     *   --debug-port <n>    override the TCP debug-server port (multi-instance)
     *   --renderer <name>   override the renderer: software|opengl|vulkan
     *   --launcher          force the GUI launcher (overrides skip_launcher)
     *   --no-launcher       skip the GUI launcher (boot straight in)
     *   <positional>        deprecated alias for --bios
     * No --memcard-dir / --game-root flags: those are config-driven. */
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--bios") == 0 && i + 1 < argc) {
            bios_path = argv[++i];
            bios_from_cli = true;
        } else if (std::strcmp(argv[i], "--game") == 0 && i + 1 < argc) {
            game_config_path = argv[++i];
        } else if (std::strcmp(argv[i], "--disc") == 0 && i + 1 < argc) {
            disc_override_path = argv[++i];
        } else if (std::strcmp(argv[i], "--debug-port") == 0 && i + 1 < argc) {
            cli_debug_port = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--renderer") == 0 && i + 1 < argc) {
            const char* r = argv[++i];
            if      (std::strcmp(r, "software") == 0) cli_renderer = 0;
            else if (std::strcmp(r, "opengl")   == 0) cli_renderer = 1;
            else if (std::strcmp(r, "vulkan")   == 0) cli_renderer = 2;
        } else if (std::strcmp(argv[i], "--window-title") == 0 && i + 1 < argc) {
            cli_window_title = argv[++i];
        } else if (std::strcmp(argv[i], "--launcher") == 0) {
            force_launcher = true;
        } else if (std::strcmp(argv[i], "--no-launcher") == 0) {
            force_no_launcher = true;
        } else if (argv[i][0] != '-') {
            bios_path = argv[i];
            bios_from_cli = true;
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
    std::filesystem::path memcard1_path;   /* explicit slot-1 .mcd (empty => dir/card1.mcd) */
    std::filesystem::path memcard2_path;   /* explicit slot-2 .mcd (empty => dir/card2.mcd) */
    bool memcard1_enabled = true;
    bool memcard2_enabled = true;
    /* [controller] device routing (defaults: P1 keyboard/digital, P2 none). */
    /* Dev builds default Player 1 to the first connected controller ("auto"):
     * combined with the always-on keyboard merge (g_dev_kb_p1) this means the
     * selected controller AND the keyboard both drive P1 with no launcher setup.
     * If no controller is present, "auto" opens nothing and the keyboard merge
     * still drives P1. Release keeps "keyboard" (the launcher assigns devices). */
#if defined(PSX_DEBUG_TOOLS)
    std::string p1_device = "auto";
#else
    std::string p1_device = "keyboard";
#endif
    std::string p2_device = "none";
    int  p1_mode = PSXRecompV4::PAD_MODE_HYBRID;
    int  p2_mode = PSXRecompV4::PAD_MODE_HYBRID;
    bool ctrl_allow_hybrid = true;  /* game.toml [controller] allow_hybrid; false hides Hybrid in the launcher */
    int  resolved_deadzone = -1;  /* <0 => keep input.ini/runtime default (12000) */
    std::filesystem::path resolved_disc;
    std::string window_title = PSX_WINDOW_TITLE;
    uint16_t   debug_port    = (uint16_t)DEFAULT_DEBUG_PORT;
    std::string game_name;
    std::string game_id;
    bool        game_has_disc_crc = false;
    uint32_t    game_disc_crc     = 0;
    std::string disc_speed;   /* "1x" | "2x" | "4x" | "instant" */
    int        instant_rate  = 0;   /* 0 = cdrom.c built-in default */
    uint32_t   game_entry_pc = 0;
    bool       fast_boot     = false;

    if (game_config_path) {
        try {
            const auto gc = PSXRecompV4::load_game_config(game_config_path);
            game_name = gc.name;
            game_id   = gc.id;
            game_has_disc_crc = gc.has_disc_crc;
            game_disc_crc     = gc.disc_crc;
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
            g_video_scale      = gc.runtime.video_supersampling;
            g_video_aa         = gc.runtime.video_antialiasing;
            g_video_texfilter  = gc.runtime.video_texture_filter;
            g_video_renderer   = gc.runtime.video_renderer;
            g_video_screen     = gc.runtime.video_screen_kind;
            g_video_aspect_num = gc.runtime.video_aspect_num;
            g_video_aspect_den = gc.runtime.video_aspect_den;
            g_low_latency_input = gc.runtime.video_low_latency_input ? 1 : 0;
            g_video_vsync       = gc.runtime.video_vsync;
            g_fmv_skip_total_table = gc.runtime.video_fmv_skip_total_table;
            g_fmv_skip_movie_id    = gc.runtime.video_fmv_skip_movie_id;
            if (gc.runtime.video_fmv_skip_end_total)
                g_fmv_skip_end_total = gc.runtime.video_fmv_skip_end_total;
            g_ws_anchor_addr   = gc.ws_sprite_anchor_addr;
            g_ws_hud_sprt      = gc.ws_hud_sprt_squash;
            /* [widescreen] full_2d — opt a pure-2D sprite game (MMX6) into the
             * widescreen present path. Applied to the GPU layer up front so the
             * ws engage at game entry classifies every frame as gameplay. */
            gpu_ws_set_full_2d(gc.ws_full_2d ? 1 : 0);
            /* Register the [widescreen.backdrop] store PCs so the dirty-RAM
             * interpreter applies the backdrop screenX squash on the interp
             * path (overlay backdrop handlers run interpreted when no cache
             * DLL is loaded — the recompiler emit only covers native). */
            if (!gc.ws_backdrop_x_sites.empty())
                psx_ws_set_backdrop_sites(gc.ws_backdrop_x_sites.data(),
                                          (int)gc.ws_backdrop_x_sites.size());
            g_audio_spu_hq     = gc.runtime.audio_spu_hq;
            g_auto_skip_fmv    = gc.runtime.video_auto_skip_fmv ? 1 : 0;
            /* [controller] game-declared input defaults (settings.toml/launcher
             * still override below). */
            if (gc.runtime.has_default_mode) {
                p1_mode = gc.runtime.default_p1_mode;
                p2_mode = gc.runtime.default_p2_mode;
            }
            ctrl_allow_hybrid = gc.runtime.controller_allow_hybrid;
            if (gc.runtime.has_deadzone) resolved_deadzone = gc.runtime.deadzone;
            /* LEGACY per-game pad-config opt-in (default modern). Only Tomba sets
             * it, so its launcher Hybrid mode's analog<->digital flip doesn't make
             * libpad manufacture a 1-frame "pad unplugged". sio_init() does not
             * touch this flag, so applying it here (config-load time) is stable.
             * Full history + removal plan: psxrecomp sio.c g_pad_legacy_cfg. */
            sio_set_legacy_cfg(gc.runtime.legacy_pad_config ? 1 : 0);
            { const char *e = std::getenv("PSX_GL_FORCE_CPU_PRESENT");
              if (e && e[0] && e[0] != '0') g_gl_fbo_present = 0; }
            game_entry_pc = gc.entry_pc;
            fast_boot     = gc.runtime.fast_boot;
            /* Pin the overlay-region floor to THIS game's main-EXE text end so
             * runtime-loaded overlays (which load just above it) are dispatched
             * via in-interpreter local-flow chaining, NOT the slow block-by-block
             * + bail-prone non-local-call path. Hardcoding the floor to Tomba 1's
             * text end (0x98000) wedged Tomba 2 (text ends 0x38800, overlays at
             * 0x85000+) at the Whoopee-Camp splash. See dirty_ram_interp.h. */
            {
                extern uint32_t g_overlay_region_floor;
                uint32_t text_end = (gc.load_address + gc.text_size) & 0x1FFFFFFFu;
                if (text_end > 0x00010000u /* DIRTY_RAM_KERNEL_WINDOW_END */)
                    g_overlay_region_floor = text_end;
                std::fprintf(stdout,
                    "psxrecomp: overlay_region_floor = 0x%05X (game text end)\n",
                    g_overlay_region_floor);
            }
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
                /* Resolve the overlay tier first so we wire the RIGHT compiler's
                 * autocompile command. gcc is "available" only when a gcc cmd is
                 * configured AND a gcc toolchain is actually reachable (a real
                 * dev/production box). auto => gcc if so, else tcc; auto-no-gcc =>
                 * tcc even with gcc present (simulate a toolchain-less user box).
                 * env PSX_OVERLAY_BACKEND overrides. Tiers: static > gcc > tcc >
                 * (sljit, deprecated/off) > interp. */
                const char *cfg_backend = gc.runtime.overlay_backend.empty()
                        ? nullptr : gc.runtime.overlay_backend.c_str();
                int gcc_avail = gc.runtime.has_overlay_autocompile_cmd
                                && autocompile_toolchain_available();
                OverlayBackend eff = overlay_backend_resolve(cfg_backend, gcc_avail);
                /* gcc and tcc run the IDENTICAL recompiler->C->DLL->load pipeline;
                 * only the compiler binary differs. Wire the autocompile spawn with
                 * the command for the resolved tier (tcc cmd for the tcc tier, gcc
                 * cmd otherwise). gcc shards already on disk still LOAD either way
                 * (the loader is compiler-blind), so a tcc box uses shipped gcc
                 * shards first and fills the rest with tcc. */
                std::string built_tcc_cmd;  /* runtime-constructed bundled tcc cmd */
                const std::string *ac_cmd = nullptr;
                if (eff == OVERLAY_BACKEND_TCC) {
                    if (gc.runtime.has_overlay_autocompile_cmd_tcc) {
                        ac_cmd = &gc.runtime.overlay_autocompile_cmd_tcc;  /* explicit override (dev) */
                    } else {
                        /* PRODUCTION: construct the tcc autocompile cmd from the
                         * self-contained toolchain bundled beside the exe
                         * (<exe>/overlay_toolchain/ = embedded python + tcc +
                         * recompiler + compile_overlays.py + runtime headers). No
                         * system python or gcc required. */
                        extern int g_psx_cps_mode;
                        std::filesystem::path xd = exe_dir_from_argv(argv[0]);
                        std::filesystem::path tk = xd / "overlay_toolchain";
                        std::filesystem::path py = tk / "python" / "python.exe";
                        if (std::filesystem::exists(py)) {
                            built_tcc_cmd =
                                py.string() + " " + (tk / "compile_overlays.py").string() +
                                " --captures " + (xd / "overlay_captures.json").string() +
                                " --game-toml " + std::string(game_config_path ? game_config_path : "game.toml") +
                                " --recompiler " + (tk / "psxrecomp-game.exe").string() +
                                " --runtime-include " + (tk / "include").string() +
                                " --out-dir " + (xd / "cache").string() +
                                (g_psx_cps_mode ? " --cps" : "") +
                                " --compiler tcc --tcc " + (tk / "tcc" / "tcc.exe").string();
                            ac_cmd = &built_tcc_cmd;
                            std::fprintf(stdout,
                                "psxrecomp: tcc tier using bundled toolchain (%s)\n",
                                tk.string().c_str());
                        } else {
                            std::fprintf(stdout,
                                "psxrecomp: tcc tier active but no bundled toolchain at %s "
                                "(overlay gaps -> interpreter)\n", tk.string().c_str());
                        }
                    }
                } else {
                    if (gc.runtime.has_overlay_autocompile_cmd)
                        ac_cmd = &gc.runtime.overlay_autocompile_cmd;
                }
                if (ac_cmd) {
                    /* Pin the compile's WRITE cache + READ captures to the SAME
                     * canonical locations the loader uses (cache_dir = <exe>/cache,
                     * <exe>/overlay_captures.json — set above). The framework owns
                     * the cache location; no game.toml --out-dir/--captures can make
                     * the write drift from the read. Single source of truth, all
                     * games, dev or prod. */
                    std::string captures_path =
                        (exe_dir / "overlay_captures.json").string();
                    autocompile_set_cache_paths(cache_dir.c_str(),
                                                captures_path.c_str());
                    autocompile_configure(ac_cmd->c_str(),
                                          gc.project_root.string().c_str());
                    std::fprintf(stdout,
                        "psxrecomp: overlay autocompile enabled (%s); cache=%s\n",
                        overlay_backend_name(eff), cache_dir.c_str());
                }
                code_provider_init(cfg_backend, gcc_avail);
                /* Now that the backend is resolved, apply the sljit live policy:
                 * a toolchain-less (sljit) machine runs validated shards live so
                 * it self-improves on the normal play path; PSX_OVERLAY_SLJIT_LIVE
                 * overrides. (Validated-live, not blind — see the dispatch gate.) */
                overlay_loader_apply_live_policy();
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

    /* Layer the launcher-written settings.toml (next to the exe) over the
     * bundled game.toml. Any field present there overrides the config value;
     * the command line (--bios/--disc) still wins over the file. Absent =>
     * fall through to game.toml. The file is the launcher's persistence. */
    bool skip_launcher_setting = false;  /* [launcher] skip_launcher from settings.toml */
    std::string settings_bios_storage;  /* must outlive resolve_bios_for_runtime */
    {
        std::filesystem::path settings_path =
            exe_dir_from_argv(argv[0]) / "settings.toml";
        const PSXRecompV4::UserSettings us =
            PSXRecompV4::load_user_settings(settings_path);
        if (us.has_skip_launcher)  skip_launcher_setting = us.skip_launcher;
        if (us.has_renderer)       g_video_renderer  = us.renderer;
        if (us.has_supersampling)  g_video_scale     = us.supersampling;
        if (us.has_window_width)   g_video_win_w     = us.window_width;
        if (us.has_antialiasing)   g_video_aa        = us.antialiasing;
        if (us.has_texture_filter) g_video_texfilter = us.texture_filter;
        if (us.has_screen_kind)    g_video_screen    = us.screen_kind;
        if (us.has_auto_skip_fmv)  g_auto_skip_fmv   = us.auto_skip_fmv ? 1 : 0;
        if (us.has_turbo_loads)    g_turbo_loads_enabled = us.turbo_loads ? 1 : 0;
        if (us.has_fast_boot)      fast_boot = us.fast_boot;
        if (us.has_fullscreen)     g_fullscreen      = us.fullscreen ? 1 : 0;
        if (us.has_aspect_ratio) {
            g_video_aspect_num = us.aspect_num;
            g_video_aspect_den = us.aspect_den;
        }
        if (us.has_spu_hq)         g_audio_spu_hq    = us.spu_hq;
        if (us.has_bios_path && !bios_from_cli) {
            settings_bios_storage = us.bios_path.string();
            bios_path = settings_bios_storage.c_str();
        }
        if (us.has_disc_path && !disc_override_path) resolved_disc = us.disc_path;
        if (us.has_memcard_dir)                      memcard_dir   = us.memcard_dir;
        if (us.has_memcard1_path)    memcard1_path    = us.memcard1_path;
        if (us.has_memcard2_path)    memcard2_path    = us.memcard2_path;
        if (us.has_memcard1_enabled) memcard1_enabled = us.memcard1_enabled;
        if (us.has_memcard2_enabled) memcard2_enabled = us.memcard2_enabled;
        if (us.has_p1_device) p1_device = us.p1_device;
        if (us.has_p2_device) p2_device = us.p2_device;
        if (us.has_p1_mode) p1_mode = us.p1_mode;
        if (us.has_p2_mode) p2_mode = us.p2_mode;
        if (us.has_deadzone)  resolved_deadzone = us.deadzone;
        if (us.has_low_latency_input) g_low_latency_input = us.low_latency_input ? 1 : 0;
        if (us.has_vsync)             g_video_vsync       = us.vsync;
    }

    /* Latency knobs: env overrides win over config (for A/B measurement).
     * PSX_LOW_LATENCY_INPUT=0/1 ; PSX_VSYNC=1(vsync)/0(immediate)/-1(adaptive). */
    if (const char *e = std::getenv("PSX_LOW_LATENCY_INPUT")) g_low_latency_input = atoi(e) ? 1 : 0;
    if (const char *e = std::getenv("PSX_VSYNC"))             g_video_vsync       = atoi(e);

    /* Resolve the effective memory-card directory now (before the launcher) so
     * the launcher can introspect the real card files. The same default is used
     * by the runtime below. */
    if (memcard_dir.empty()) memcard_dir = default_memcard_dir(argv[0]);

    /* The game's OWN native OPTION settings (game_options.toml, next to
     * game.toml) — persisted across launches, kept separate from game.toml
     * (recomp config) and settings.toml (launcher). Values are saved to
     * <memcard_dir>/<game_id>.options. Best-effort: a malformed file disables
     * the feature, it never blocks boot. */
    if (game_config_path) {
        try {
            std::filesystem::path go_path =
                std::filesystem::path(game_config_path).parent_path() / "game_options.toml";
            const PSXRecompV4::GameOptions go = PSXRecompV4::load_game_options(go_path);
            if (!go.options.empty()) {
                std::vector<uint32_t>    go_addrs;
                std::vector<uint8_t>     go_sizes;
                std::vector<const char*> go_names;
                std::vector<int32_t>     go_vmins;
                std::vector<int32_t>     go_vmaxs;
                for (const auto& o : go.options) {
                    go_addrs.push_back(o.addr);
                    go_sizes.push_back((uint8_t)o.size);
                    go_names.push_back(o.name.c_str());
                    /* Declared range validates the persisted value at restore; no
                     * range => full int32 span (accept anything that fits). */
                    go_vmins.push_back(o.has_range ? (int32_t)o.vmin : (int32_t)0x80000000);
                    go_vmaxs.push_back(o.has_range ? (int32_t)o.vmax : (int32_t)0x7FFFFFFF);
                }
                std::string go_state = (memcard_dir /
                    ((game_id.empty() ? std::string("game") : game_id) + ".options")).string();
                game_options_configure(go_state.c_str(), go_addrs.data(), go_sizes.data(),
                                       go_names.data(), go_vmins.data(), go_vmaxs.data(),
                                       (int)go_addrs.size());
                std::fprintf(stdout,
                    "psxrecomp: game options persistence armed (%d field(s)) -> %s\n",
                    (int)go_addrs.size(), go_state.c_str());
            }
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "psxrecomp: game_options.toml ignored: %s\n", ex.what());
        }
    }

#if defined(PSX_LAUNCHER)
    /* Integrated launcher: shown in its own GL window before the emulator
     * boots. Seeded with the effective settings (game.toml ∪ settings.toml);
     * on LAUNCH the user's choices are persisted to settings.toml and applied.
     * The launcher window/context is fully torn down before the emulator's own
     * window is created, so the emulator boot path below is untouched.
     *
     * Skip the GUI (boot straight in) when ANY of: PSX_NO_LAUNCHER=1 env,
     * --no-launcher, or the persisted [launcher] skip_launcher setting — unless
     * --launcher forces it back on (mirrors snesrecomp's SkipLauncher / --launcher).
     * This removes the dismiss-the-launcher round-trip for scripted/debug runs. */
    const bool want_launcher =
        force_launcher ||
        (!std::getenv("PSX_NO_LAUNCHER") && !force_no_launcher && !skip_launcher_setting);
    if (want_launcher) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) == 0) {
            PSXRecompV4::UserSettings seed;
            seed.renderer = g_video_renderer;             seed.has_renderer = true;
            seed.supersampling = g_video_scale;           seed.has_supersampling = true;
            seed.antialiasing = g_video_aa;               seed.has_antialiasing = true;
            seed.texture_filter = g_video_texfilter;      seed.has_texture_filter = true;
            seed.screen_kind = g_video_screen;            seed.has_screen_kind = true;
            seed.auto_skip_fmv = (g_auto_skip_fmv != 0);  seed.has_auto_skip_fmv = true;
            seed.turbo_loads = (g_turbo_loads_enabled != 0); seed.has_turbo_loads = true;
            seed.fast_boot = fast_boot;                   seed.has_fast_boot = true;
            seed.fullscreen = (g_fullscreen != 0);        seed.has_fullscreen = true;
            seed.aspect_num = g_video_aspect_num;
            seed.aspect_den = g_video_aspect_den;         seed.has_aspect_ratio = true;
            seed.spu_hq = g_audio_spu_hq;                 seed.has_spu_hq = true;
            seed.skip_launcher = skip_launcher_setting;   seed.has_skip_launcher = true;
            if (bios_path && bios_path[0]) { seed.bios_path = bios_path; seed.has_bios_path = true; }
            if (!resolved_disc.empty())    { seed.disc_path = resolved_disc; seed.has_disc_path = true; }
            seed.memcard_dir = memcard_dir;          seed.has_memcard_dir = true;
            seed.memcard1_enabled = memcard1_enabled; seed.has_memcard1_enabled = true;
            seed.memcard2_enabled = memcard2_enabled; seed.has_memcard2_enabled = true;
            if (!memcard1_path.empty()) { seed.memcard1_path = memcard1_path; seed.has_memcard1_path = true; }
            if (!memcard2_path.empty()) { seed.memcard2_path = memcard2_path; seed.has_memcard2_path = true; }
            seed.p1_device = p1_device; seed.has_p1_device = true;
            seed.p2_device = p2_device; seed.has_p2_device = true;
            seed.p1_mode = p1_mode; seed.has_p1_mode = true;
            seed.p2_mode = p2_mode; seed.has_p2_mode = true;
            seed.deadzone = resolved_deadzone >= 0 ? resolved_deadzone : 12000;
            seed.has_deadzone = true;
            seed.window_width = g_video_win_w; seed.has_window_width = true;

            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
            SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

            /* Launcher opens at the same 4:3 size the game will, so there's no
             * jarring resize on LAUNCH. The dense dashboard needs a usable
             * minimum, so the launcher floor is 1280 wide even if the game
             * window is set smaller. */
            int lwin_w = g_video_win_w < 1280 ? 1280 : g_video_win_w;
            int lwin_h = 0;
            clamp_window_aspect(&lwin_w, &lwin_h, 4, 3);
            std::string lwin_title = (game_name.empty() ? std::string("PSX") : game_name)
                                     + " \xE2\x80\x94 Launcher";
            SDL_Window* lwin = SDL_CreateWindow(
                lwin_title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                lwin_w, lwin_h, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
            psx_launcher::Result lr = psx_launcher::Result::Unavailable;
            if (lwin) {
                SDL_GLContext lctx = SDL_GL_CreateContext(lwin);
                if (lctx) {
                    SDL_GL_MakeCurrent(lwin, lctx);
                    SDL_GL_SetSwapInterval(1);
                    std::string assets = exe_dir_from_argv(argv[0]).string();
                    psx_launcher::GameInfo ginfo;
                    ginfo.name             = game_name.empty() ? nullptr : game_name.c_str();
                    ginfo.expected_serial  = game_id.empty()   ? nullptr : game_id.c_str();
                    ginfo.expected_crc     = game_disc_crc;
                    ginfo.has_expected_crc = game_has_disc_crc;
                    ginfo.allow_hybrid     = ctrl_allow_hybrid;
                    lr = psx_launcher::run(lwin, lctx, seed, ginfo, assets.c_str());
                    SDL_GL_DeleteContext(lctx);
                }
                SDL_DestroyWindow(lwin);
            }
            /* Reset GL attributes so the emulator window starts from defaults. */
            SDL_GL_ResetAttributes();

            if (lr == psx_launcher::Result::Quit) {
                std::fprintf(stdout, "psxrecomp: launcher closed; exiting.\n");
                return 0;
            }
            if (lr == psx_launcher::Result::Launch) {
                g_video_renderer  = seed.renderer;
                g_video_scale     = seed.supersampling;
                g_video_aa        = seed.antialiasing;
                g_video_texfilter = seed.texture_filter;
                g_video_screen    = seed.screen_kind;
                g_auto_skip_fmv   = seed.auto_skip_fmv ? 1 : 0;
                g_turbo_loads_enabled = seed.turbo_loads ? 1 : 0;
                fast_boot = seed.fast_boot;
                g_fullscreen      = seed.fullscreen ? 1 : 0;
                g_video_aspect_num = seed.aspect_num;
                g_video_aspect_den = seed.aspect_den;
                g_audio_spu_hq    = seed.spu_hq;
                skip_launcher_setting = seed.skip_launcher;
                if (seed.has_bios_path) {
                    settings_bios_storage = seed.bios_path.string();
                    bios_path = settings_bios_storage.c_str();
                }
                if (seed.has_disc_path) resolved_disc = seed.disc_path;
                memcard1_enabled = seed.memcard1_enabled;
                memcard2_enabled = seed.memcard2_enabled;
                if (seed.has_memcard1_path) memcard1_path = seed.memcard1_path;
                if (seed.has_memcard2_path) memcard2_path = seed.memcard2_path;
                p1_device = seed.p1_device; p2_device = seed.p2_device;
                p1_mode = seed.p1_mode; p2_mode = seed.p2_mode;
                if (seed.has_deadzone) resolved_deadzone = seed.deadzone;
                g_video_win_w = seed.window_width;
                /* Persist the user's choices next to the exe. */
                PSXRecompV4::save_user_settings(
                    exe_dir_from_argv(argv[0]) / "settings.toml", seed);
            }
        }
    }
#endif

    /* CLI overrides win over config — applied last, before backend/port init.
     * Enables a soak fleet: several instances on distinct ports + renderers,
     * including BIOS instances that have no [game]-block config. */
    if (cli_debug_port >= 0) debug_port      = (uint16_t)cli_debug_port;
    if (cli_renderer   >= 0) g_video_renderer = cli_renderer;
    if (cli_window_title)    window_title     = cli_window_title;

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

    /* memcard_dir was resolved to its default before the launcher (above). */

    std::string bios_path_str    = resolved_bios.string();
    std::string memcard_dir_str  = memcard_dir.string();
    std::string disc_path_str    = resolved_disc.string();

    std::fprintf(stdout, "psxrecomp runtime: loading BIOS from %s\n", bios_path_str.c_str());
    memory_init(bios_path_str.c_str());
    /* Select the renderer backend BEFORE gpu_init() (which runs gr_init ->
     * the backend's init on the VRAM buffer). Software is the default and the
     * fallback; an unavailable OpenGL backend reverts to software. */
    gr_set_backend(g_video_renderer == 2 ? GR_BACKEND_VULKAN :
                   g_video_renderer == 1 ? GR_BACKEND_OPENGL : GR_BACKEND_SOFTWARE);
    std::fprintf(stdout, "psxrecomp: renderer backend requested: %s\n",
                 g_video_renderer == 2 ? "vulkan" :
                 g_video_renderer == 1 ? "opengl" : "software");
    gpu_init();
    /* Internal-resolution supersampling (SSAA). Must follow gpu_init (which
     * runs sw_renderer_init). scale==1 is a no-op; >1 allocates the hi-res
     * VRAM mirror. */
    if (g_video_scale < 1) g_video_scale = 1;
    if (g_video_scale > SW_MAX_INTERNAL_SCALE) g_video_scale = SW_MAX_INTERNAL_SCALE;
    gr_set_scale(g_video_scale);
    g_video_scale = gr_scale(); /* reflect any clamp / alloc fallback */
    gr_set_texture_filter(g_video_texfilter);
    /* Display aspect. Identity at the default 4:3. The present letterbox uses
     * this aspect; native-wide fills it with a genuinely wider frame (no
     * stretch), squash mode stretches the 4:3 frame into it. */
    gl_renderer_set_display_aspect(g_video_aspect_num, g_video_aspect_den);
    if (g_video_aspect_num * 3 != g_video_aspect_den * 4) {
        /* Hold widescreen off through the BIOS boot (authentic 4:3 logos);
         * the per-frame present path engages it at game entry. */
        g_ws_engaged = false;
        std::fprintf(stdout,
                     "psxrecomp: widescreen %d:%d (%s%s%s; engages at game entry)\n",
                     g_video_aspect_num, g_video_aspect_den,
                     g_ws_native_wide ? "native-wide, present 1:1"
                                      : "GTE X-squash + stretched present",
                     g_ws_anchor_addr ? " + sprite tags" : "",
                     g_ws_hud_sprt ? " + HUD squash" : "");
    }
    /* Present-time screen-colour model (verified-enhancement LUT). Default raw
     * is byte-identical; PSX_SCREEN env overrides this at scanout. */
    gpu_set_screen_kind(g_video_screen);
    if (g_video_scale > 1 || g_video_texfilter)
        std::fprintf(stdout,
                     "psxrecomp: supersampling %dx (antialiasing %s, texture filter %s)\n",
                     g_video_scale, g_video_aa ? "on" : "off",
                     g_video_texfilter ? "bilinear" : "nearest");
    if (g_video_screen != 0)
        std::fprintf(stdout, "psxrecomp: screen-colour model %s\n",
                     g_video_screen == 1 ? "crt" : g_video_screen == 2 ? "composite"
                                                 : "trinitron");
    dma_init();
    mdec_init();
    timers_init();
    interrupts_init();
    sio_init();
    /* Seed per-player device routing from the resolved [controller] config.
     * SDL controller handles are opened later (after SDL_Init); here we only
     * set the PSX-visible connection + pad type so the BIOS sees the right
     * ports during early boot. */
    set_player_device(g_players[0], p1_device, p1_mode);
    set_player_device(g_players[1], p2_device, p2_mode);
    for (int s = 0; s < 2; s++) {
        /* Dev builds keep P1 connected even with no controller so the always-on
         * keyboard (g_dev_kb_p1) can drive port 1 standalone. */
        const bool kb_p1 = (g_dev_kb_p1 && s == 0);
        sio_set_pad_connected(s, (g_players[s].kind != 0 || kb_p1) ? 1 : 0);
        sio_set_pad_analog(s, pad_mode_boot_analog(g_players[s].mode), 0x80, 0x80, 0x80, 0x80);
    }
    /* SPU float-shadow gate must be set before spu_init() (which runs
     * spu_shadow_reset()). Default OFF; PSX_AUDIO_SHADOW env overrides. */
    spu_shadow_set_enabled(g_audio_spu_hq ? 1 : 0);
    if (g_audio_spu_hq)
        std::fprintf(stdout, "psxrecomp: SPU float-shadow enabled (verified-enhancement)\n");
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
    {
        std::string mc1 = memcard1_path.string();
        std::string mc2 = memcard2_path.string();
        const MemcardSlotConfig slots[2] = {
            { mc1.empty() ? nullptr : mc1.c_str(), memcard1_enabled ? 1 : 0 },
            { mc2.empty() ? nullptr : mc2.c_str(), memcard2_enabled ? 1 : 0 },
        };
        memcard_init_slots(memcard_dir_str.c_str(), slots);
    }
    std::atexit(memcard_flush_all);
    /* Persist the game's native OPTION settings on any exit path (belt-and-
     * suspenders; save-on-change already persists them live). No-op if not yet
     * armed, so it can't overwrite the saved file with boot defaults. */
    std::atexit(game_options_save_now);
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
    /* ...but HIDAPI's Xbox sub-driver is OFF by default on Windows (Xbox pads are
     * normally RAWINPUT/XInput there). With RAWINPUT disabled above, a PHYSICAL
     * Xbox One/Series controller would be claimed by nobody -> not a GameController
     * -> zero input (PS5 DualSense works regardless: its HIDAPI driver is on by
     * default). Enable the HIDAPI Xbox driver so HIDAPI handles Xbox pads too. */
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_XBOX, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    load_input_config(argv[0]);
    /* The launcher / settings.toml / game.toml deadzone (when set) is the
     * user-facing authority; apply it over the input.ini value here, after
     * load_input_config has read input.ini. */
    if (resolved_deadzone >= 0)
        controller_deadzone = std::max(0, std::min(32767, resolved_deadzone));
    refresh_player_devices();  /* open SDL handles to match the player config */
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
    if (g_video_renderer == 2) win_flags |= SDL_WINDOW_VULKAN;
    /* Fullscreen on launch (launcher "Fullscreen on launch" toggle). DESKTOP
     * fullscreen keeps the desktop resolution and letterboxes the image, matching
     * the in-game F11 / Alt+Enter hotkey behaviour. */
    if (g_fullscreen) win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    /* Open at the user-chosen window size (default 1280 wide) instead of the
     * old hardcoded 640x480, so the game doesn't boot into a tiny window. The
     * height follows the configured display aspect (4:3 native, wider for the
     * widescreen hack); the present path letterboxes to the same aspect, so
     * the image scales to fill the larger window with no further distortion. */
    int game_w = g_video_win_w, game_h = 0;
    clamp_window_aspect(&game_w, &game_h, g_video_aspect_num, g_video_aspect_den);
    sdl_window = SDL_CreateWindow(
        window_title.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        game_w, game_h,
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
        gl_renderer_set_swap_interval(g_video_vsync);   /* applied at context init */
        g_gl_active = (gl_renderer_init_context(sdl_window) != 0);
        if (!g_gl_active) gr_set_backend(GR_BACKEND_SOFTWARE);
        /* The GL backend establishes its real internal scale HERE (raster init),
         * which is AFTER the earlier `g_video_scale = gr_scale()` sync (that ran
         * before this and so saw the default scale 1). Re-sync now so the staging
         * buffer below is sized for the true scale and the native-wide present
         * (which uses gr_scale()) matches it — otherwise sdl_pixel_buf is
         * undersized and the wide readback overflows it. */
        g_video_scale = gr_scale();
    }
    /* Vulkan backend: create the instance/device/swapchain on the
     * SDL_WINDOW_VULKAN window. On failure, fall back to software (vkb_init
     * already initialized the software renderer on the shared VRAM array). */
    if (g_video_renderer == 2) {
        vk_renderer_set_present_mode(g_video_vsync);
        g_vk_active = (vk_renderer_init_context(sdl_window) != 0);
        if (!g_vk_active) gr_set_backend(GR_BACKEND_SOFTWARE);
        g_video_scale = gr_scale();
    }
    latency_ring_set_backend(g_vk_active ? "vulkan" : g_gl_active ? "opengl" : "software");
    latency_ring_set_present_mode(g_video_vsync);
    /* Title bar shows the clean game title (set at window creation); the active
     * renderer is reported via the debug server / config, not appended here. */

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
  if (!g_gl_active && !g_vk_active) {
#ifdef _WIN32
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
#endif
    /* Vsync off (g_video_vsync==0) drops PRESENTVSYNC for lowest display
     * latency; the wall-clock pacer still holds 59.94Hz (may tear). */
    Uint32 rflags = SDL_RENDERER_ACCELERATED |
                    (g_video_vsync != 0 ? SDL_RENDERER_PRESENTVSYNC : 0u);
    sdl_renderer = SDL_CreateRenderer(sdl_window, -1, rflags);
    if (!sdl_renderer)
        sdl_renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_ACCELERATED);
    if (!sdl_renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Present in a logical space of the configured aspect (640x480 at native
     * 4:3, wider at widescreen aspects; scaled by the supersampling factor so
     * the full internal resolution reaches a large/fullscreen window; SDL
     * still scales and letterboxes to the real output). Identity in the
     * default window when supersampling is off, so native rendering is
     * unchanged. */
    g_logical_w = 480 * g_video_aspect_num * g_video_scale / g_video_aspect_den;
    SDL_RenderSetLogicalSize(sdl_renderer, g_logical_w, 480 * g_video_scale);
  }

    /* Staging buffer + backing texture are sized for the internal resolution
     * (640x512 native, times the supersampling factor). */
    sdl_pixel_buf = (uint32_t*)std::malloc(
        (size_t)640 * g_video_scale * 512 * g_video_scale * sizeof(uint32_t));
    if (!sdl_pixel_buf) {
        std::fprintf(stderr, "failed to allocate %dx staging buffer\n", g_video_scale);
        return 1;
    }

  if (!g_gl_active && !g_vk_active) {
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
    cpu.read_word  = psx_guest_read_word;  /* +6cyc main-RAM read wait states */
    cpu.write_word = psx_write_word;
    cpu.read_half  = psx_guest_read_half;
    cpu.write_half = psx_write_half;
    cpu.read_byte  = psx_guest_read_byte;
    cpu.write_byte = psx_write_byte;
    /* Wire the sljit JIT host-helper table (cpu-relative => position-independent
     * shards; prerequisite for the persisted sljit shard cache). Harmless when
     * sljit isn't the active backend. */
    overlay_sljit_init_helpers(&cpu);

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

    /* If we reach here, all execution completed without MMIO abort.
     * During normal operation the guest runs an infinite main loop and the
     * top-level psx_dispatch NEVER returns. Reaching this point means the
     * outermost trampoline loop saw cpu->pc == 0 (some jr/tail-transfer
     * published a null PC) — an abnormal boot exit. Dump the always-on
     * fntrace ring tail (last dispatch chain) to a JSON artifact so we can
     * see exactly which targets led to the null PC. (CLAUDE.md ring-buffer
     * model: consume the always-on ring after the fact, not arm-and-time.) */
    {
        FILE* tf = std::fopen("psx_cps_exit_trace.json", "wb");
        if (tf) {
            std::fprintf(tf, "{\n  \"final_pc\": \"0x%08X\",\n  \"final_ra\": \"0x%08X\",\n"
                            "  \"final_sp\": \"0x%08X\",\n  \"fntrace_seq\": %llu,\n  \"tail\": [\n",
                         cpu.pc, cpu.gpr[31], cpu.gpr[29],
                         (unsigned long long)g_fntrace_seq);
            uint64_t seq = g_fntrace_seq;
            uint32_t n = seq < 128u ? (uint32_t)seq : 128u;
            for (uint32_t i = 0; i < n; i++) {
                uint64_t idx = seq - n + i;
                const FntraceEntry* e = &g_fntrace_ring[idx % FNTRACE_RING_CAP];
                std::fprintf(tf,
                    "    {\"seq\":%llu,\"frame\":%u,\"target\":\"0x%08X\",\"ra\":\"0x%08X\","
                    "\"sp\":\"0x%08X\",\"a0\":\"0x%08X\",\"a1\":\"0x%08X\"}%s\n",
                    (unsigned long long)idx, e->frame, e->target, e->ra, e->sp,
                    e->a0, e->a1, (i + 1 < n) ? "," : "");
            }
            std::fprintf(tf, "  ],\n  \"stack\": [\n");
            uint32_t sbase = (cpu.gpr[29] - 0x40u) & ~3u;
            for (int w = 0; w < 40; w++) {
                uint32_t a = sbase + (uint32_t)w * 4u;
                uint32_t v = cpu.read_word(a);
                std::fprintf(tf, "    {\"addr\":\"0x%08X\",\"val\":\"0x%08X\"}%s\n",
                             a, v, (w < 39) ? "," : "");
            }
            std::fprintf(tf, "  ]\n}\n");
            std::fclose(tf);
        }
    }

    /* Diagnostic: the guest published a null PC at the top level (abnormal). With
     * PSX_EXIT_HALT set, halt-and-serve here instead of shutting down so the
     * still-loaded overlays + full guest state are live-inspectable over TCP. */
    { const char *e = std::getenv("PSX_EXIT_HALT");
      if (e && e[0] && e[0] != '0') {
          extern void psx_fatal_halt(const char *reason);
          psx_fatal_halt("top-level dispatch returned PC=0 (abnormal boot exit — inspect live)");
      }
    }

    std::fprintf(stdout, "psxrecomp runtime: execution completed, PC=0x%08X\n", cpu.pc);

    shutdown_runtime();
    if (g_gl_active) gl_renderer_shutdown();
    SDL_DestroyTexture(sdl_texture);   /* NULL-safe in GL mode */
    SDL_DestroyRenderer(sdl_renderer); /* NULL-safe in GL mode */
    SDL_DestroyWindow(sdl_window);
    SDL_Quit();

    return 0;
}
