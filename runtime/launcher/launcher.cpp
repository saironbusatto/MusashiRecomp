// launcher.cpp — see launcher.h. RmlUi (HTML/CSS) front-end over SDL2 + GL3.
//
// Uses RmlUi's official SDL platform + GL3 renderer backends (lib/RmlUi/Backends).
// The base RenderInterface_GL3 is used directly (no SDL_image dependency) — the
// minimal launcher draws with CSS, not external <img> bitmaps; image-rich polish
// is a later phase.

#include "launcher.h"

#include "config_loader.h"
#include "disc_identity.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementDocument.h>

#include "RmlUi_Platform_SDL.h"
#include "RmlUi_Renderer_GL3.h"

#include "third_party/stb_image.h"

#include <SDL.h>

#include <cstdio>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#  include <windows.h>
#  include <commdlg.h>
#endif

namespace fs = std::filesystem;

namespace {

// RenderInterface_GL3 only decodes uncompressed TGA. Override LoadTexture to
// decode PNG via stb_image (falling back to the base TGA path), so the launcher
// can use <img> with PNG art. RmlUi textures are premultiplied-alpha RGBA.
class LauncherRenderInterface : public RenderInterface_GL3 {
public:
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dims, const Rml::String& source) override {
        Rml::FileInterface* fi = Rml::GetFileInterface();
        Rml::FileHandle fh = fi ? fi->Open(source) : Rml::FileHandle(0);
        if (!fh) return RenderInterface_GL3::LoadTexture(dims, source);
        fi->Seek(fh, 0, SEEK_END);
        const size_t sz = (size_t)fi->Tell(fh);
        fi->Seek(fh, 0, SEEK_SET);
        std::vector<unsigned char> buf(sz);
        fi->Read(buf.data(), sz, fh);
        fi->Close(fh);

        int w = 0, h = 0, comp = 0;
        unsigned char* px = stbi_load_from_memory(buf.data(), (int)sz, &w, &h, &comp, 4);
        if (!px) return RenderInterface_GL3::LoadTexture(dims, source);  // maybe a TGA

        const size_t n = (size_t)w * (size_t)h;
        for (size_t i = 0; i < n; i++) {  // straight -> premultiplied alpha
            const unsigned a = px[i * 4 + 3];
            px[i * 4 + 0] = (unsigned char)(px[i * 4 + 0] * a / 255);
            px[i * 4 + 1] = (unsigned char)(px[i * 4 + 1] * a / 255);
            px[i * 4 + 2] = (unsigned char)(px[i * 4 + 2] * a / 255);
        }
        dims.x = w; dims.y = h;
        Rml::TextureHandle th = GenerateTexture({px, n * 4}, dims);
        stbi_image_free(px);
        return th;
    }
};

// Mirror of the user-tunable settings, in the value shapes the RML binds to.
struct LauncherModel {
    int  renderer        = 0;  // 0=software, 1=opengl
    int  supersampling   = 1;  // 1..4
    bool antialiasing    = true;
    int  texture_filter  = 0;  // 0=nearest, 1=bilinear
    int  crt             = 0;  // 0=raw,1=crt,2=composite,3=trinitron
    bool spu_hq          = false;

    Rml::String bios_path;
    Rml::String disc_path;

    // Display labels (kept in sync with the enum/int values above).
    Rml::String renderer_label;
    Rml::String crt_label;
    Rml::String texfilter_label;

    // Disc verification (recomputed whenever disc_path changes).
    Rml::String disc_file;      // file name only, e.g. "tomba.cue"
    Rml::String disc_region;    // "NTSC-U (USA)" | "PAL" | "NTSC-J" | "—"
    Rml::String disc_serial;    // "SCUS-94236" | "—"
    bool        v_header   = false;  // ISO9660 header present
    bool        v_crc      = false;  // CRC/hash (or serial identity) check passed
    bool        v_verified = false;  // overall verdict good
    Rml::String verdict_title;  // big line, e.g. "Tomba! disc verified"
    Rml::String verdict_detail; // sub line
    Rml::String verdict_state;  // "ok" | "warn" | "bad" | "none" — drives colour

    // View toggle: "dashboard" (default) | "settings".
    Rml::String view = "dashboard";

    // Player / memory-card cards — shell only this pass; enable toggles are
    // real bound state so they animate, the rest is static markup until the
    // Phase 4 (memcard introspection) / Phase 5 (controller enumeration) wiring.
    bool p1_enabled  = true;
    bool p2_enabled  = false;
    bool mc1_enabled = true;
    bool mc2_enabled = true;

    bool launch_requested = false;
    bool quit_requested   = false;
};

const char* renderer_name(int v)  { return v == 1 ? "OpenGL" : "Software"; }
const char* texfilter_name(int v) { return v == 1 ? "Bilinear" : "Nearest"; }
const char* crt_name(int v) {
    switch (v) {
        case 1:  return "CRT";
        case 2:  return "Composite";
        case 3:  return "Trinitron";
        default: return "Raw (off)";
    }
}

void refresh_labels(LauncherModel& m) {
    m.renderer_label  = renderer_name(m.renderer);
    m.crt_label       = crt_name(m.crt);
    m.texfilter_label = texfilter_name(m.texture_filter);
}

std::string region_long(const std::string& r) {
    if (r == "NTSC-U") return "NTSC-U (USA)";
    if (r == "NTSC-J") return "NTSC-J (Japan)";
    if (r == "PAL")    return "PAL (Europe)";
    return r;
}

// Re-run disc verification against m.disc_path and update the panel fields.
// `expected_serial`/`expected_crc` come from game.toml (via GameInfo);
// `game_name` is used for the verdict headline.
void refresh_disc_status(LauncherModel& m, const std::string& game_name,
                         const std::string& expected_serial,
                         uint32_t expected_crc, bool has_expected_crc) {
    m.v_header = m.v_crc = m.v_verified = false;
    m.disc_region = m.disc_serial = "—";
    m.disc_file = "—";

    if (m.disc_path.empty()) {
        m.verdict_title  = "No disc selected";
        m.verdict_detail = "Choose a disc image to verify it against this build.";
        m.verdict_state  = "none";
        return;
    }
    m.disc_file = fs::path(std::string(m.disc_path)).filename().generic_string();

    // Only spend time hashing when there is an expected CRC to compare against.
    const PSXRecompV4::DiscIdentity id = PSXRecompV4::identify_disc(
        fs::path(std::string(m.disc_path)), expected_serial,
        expected_crc, has_expected_crc, /*compute_crc=*/has_expected_crc);

    if (!id.opened) {
        m.verdict_title  = "Disc not found";
        m.verdict_detail = "Could not open the image or its CUE-referenced BIN.";
        m.verdict_state  = "bad";
        return;
    }
    if (!id.has_header) {
        m.verdict_title  = "Not a PlayStation disc";
        m.verdict_detail = "No ISO9660 header at the expected sectors.";
        m.verdict_state  = "bad";
        return;
    }
    m.v_header = true;
    if (!id.detected_serial.empty()) m.disc_serial = id.detected_serial;
    else if (!expected_serial.empty()) m.disc_serial = expected_serial;
    if (!id.region.empty()) m.disc_region = region_long(id.region);

    const bool serial_ok = !id.expected_serial_given || id.serial_matches;

    // Middle check: exact CRC match if we have one to compare, otherwise the
    // serial-based identity match stands in for the hash.
    if (id.expected_crc_given && id.crc_computed)
        m.v_crc = id.crc_matches;
    else
        m.v_crc = serial_ok && id.expected_serial_given;

    m.v_verified = m.v_header && serial_ok &&
                   (!id.expected_crc_given || id.crc_matches);

    const std::string nm = game_name.empty() ? std::string("Disc") : game_name;
    if (m.v_verified) {
        m.verdict_title  = nm + " disc verified";
        m.verdict_detail = "Correct disc image loaded. Ready to launch.";
        m.verdict_state  = "ok";
    } else if (!serial_ok) {
        m.verdict_title  = "Wrong disc?";
        m.verdict_detail = "Serial does not match this build (expected " + expected_serial + ").";
        m.verdict_state  = "bad";
    } else if (id.expected_crc_given && id.crc_computed && !id.crc_matches) {
        m.verdict_title  = "Disc image differs";
        m.verdict_detail = "Right game, but the image hash does not match the expected dump.";
        m.verdict_state  = "warn";
    } else {
        // Header ok, nothing authoritative to compare against.
        m.verdict_title  = "PlayStation disc";
        m.verdict_detail = "Recognised PlayStation disc. No reference hash configured.";
        m.verdict_state  = "ok";
        m.v_verified     = true;
    }
}

#if defined(_WIN32)
// Native open-file dialog. Returns "" if cancelled.
std::string win_pick_file(SDL_Window* parent, const char* title, const char* filter) {
    char buf[MAX_PATH] = {0};
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;  // e.g. "BIOS (*.bin)\0*.bin\0All files\0*.*\0\0"
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = sizeof(buf);
    ofn.lpstrTitle  = title;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    (void)parent;
    if (GetOpenFileNameA(&ofn)) return std::string(buf);
    return std::string();
}
#else
std::string win_pick_file(SDL_Window*, const char*, const char*) { return std::string(); }
#endif

// Load at least one font face so RmlUi can render text. Tries bundled fonts in
// assets_dir, then a couple of platform fallbacks. Returns true if any loaded.
bool load_fonts(const fs::path& assets_dir) {
    const char* bundled[] = {
        "fonts/LatoLatin-Regular.ttf",
        "fonts/LatoLatin-Bold.ttf",
        "LatoLatin-Regular.ttf",
    };
    bool any = false;
    for (const char* rel : bundled) {
        const fs::path p = assets_dir / rel;
        std::error_code ec;
        if (fs::exists(p, ec) && Rml::LoadFontFace(p.generic_string())) any = true;
    }
    if (any) return true;
#if defined(_WIN32)
    const char* sys[] = { "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf" };
    for (const char* p : sys)
        if (Rml::LoadFontFace(p)) any = true;
#endif
    return any;
}

} // namespace

namespace psx_launcher {

Result run(SDL_Window* window, void* gl_context,
           PSXRecompV4::UserSettings& io,
           const GameInfo& game, const char* assets_dir)
{
    (void)gl_context;  // already created + current; we only need the window.

    const std::string expected_serial = game.expected_serial ? game.expected_serial : "";
    const uint32_t    expected_crc    = game.expected_crc;
    const bool        has_expected_crc = game.has_expected_crc;

    if (!RmlGL3::Initialize()) {
        std::fprintf(stderr, "launcher: RmlGL3::Initialize failed\n");
        return Result::Unavailable;
    }

    SystemInterface_SDL system_interface;
    system_interface.SetWindow(window);
    LauncherRenderInterface render_interface;
    if (!render_interface) {
        std::fprintf(stderr, "launcher: GL3 render interface init failed\n");
        RmlGL3::Shutdown();
        return Result::Unavailable;
    }

    Rml::SetSystemInterface(&system_interface);
    Rml::SetRenderInterface(&render_interface);
    if (!Rml::Initialise()) {
        std::fprintf(stderr, "launcher: Rml::Initialise failed\n");
        RmlGL3::Shutdown();
        return Result::Unavailable;
    }

    const fs::path assets = assets_dir ? fs::path(assets_dir) : fs::current_path();
    if (!load_fonts(assets))
        std::fprintf(stderr, "launcher: warning — no font face loaded; text will not render\n");

    int win_w = 0, win_h = 0;
    SDL_GL_GetDrawableSize(window, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) { win_w = 1280; win_h = 800; }
    render_interface.SetViewport(win_w, win_h);

    Rml::Context* context = Rml::CreateContext("launcher", Rml::Vector2i(win_w, win_h));
    if (!context) {
        std::fprintf(stderr, "launcher: CreateContext failed\n");
        Rml::Shutdown();
        RmlGL3::Shutdown();
        return Result::Unavailable;
    }

    // ---- Seed the model from the effective settings ----
    LauncherModel m;
    m.renderer       = io.renderer;
    m.supersampling  = io.supersampling;
    m.antialiasing   = io.antialiasing;
    m.texture_filter = io.texture_filter;
    m.crt            = io.screen_kind;
    m.spu_hq         = io.spu_hq;
    m.bios_path      = io.has_bios_path ? io.bios_path.generic_string() : Rml::String();
    m.disc_path      = io.has_disc_path ? io.disc_path.generic_string() : Rml::String();
    refresh_labels(m);
    const std::string game_name_s = game.name ? game.name : "";
    refresh_disc_status(m, game_name_s, expected_serial, expected_crc, has_expected_crc);

    // ---- Data model: bind fields + action callbacks ----
    Rml::DataModelConstructor c = context->CreateDataModel("settings");
    if (!c) {
        Rml::Shutdown();
        RmlGL3::Shutdown();
        return Result::Unavailable;
    }
    Rml::String title = game.name ? Rml::String(game.name) : Rml::String("PSX");
    c.BindFunc("game_name", [title](Rml::Variant& out) { out = title; });
    c.Bind("supersampling",  &m.supersampling);
    c.Bind("antialiasing",   &m.antialiasing);
    c.Bind("spu_hq",         &m.spu_hq);
    c.Bind("renderer_label", &m.renderer_label);
    c.Bind("crt_label",      &m.crt_label);
    c.Bind("texfilter_label",&m.texfilter_label);
    c.Bind("bios_path",      &m.bios_path);
    c.Bind("disc_path",      &m.disc_path);
    c.Bind("disc_file",      &m.disc_file);
    c.Bind("disc_region",    &m.disc_region);
    c.Bind("disc_serial",    &m.disc_serial);
    c.Bind("v_header",       &m.v_header);
    c.Bind("v_crc",          &m.v_crc);
    c.Bind("v_verified",     &m.v_verified);
    c.Bind("verdict_title",  &m.verdict_title);
    c.Bind("verdict_detail", &m.verdict_detail);
    c.Bind("verdict_state",  &m.verdict_state);
    c.Bind("view",           &m.view);
    c.Bind("p1_enabled",     &m.p1_enabled);
    c.Bind("p2_enabled",     &m.p2_enabled);
    c.Bind("mc1_enabled",    &m.mc1_enabled);
    c.Bind("mc2_enabled",    &m.mc2_enabled);

    Rml::DataModelHandle handle = c.GetModelHandle();

    c.BindEventCallback("cycle_renderer",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.renderer ^= 1; refresh_labels(m);
            handle.DirtyVariable("renderer_label");
        });
    c.BindEventCallback("cycle_ss",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.supersampling = (m.supersampling % 4) + 1;
            handle.DirtyVariable("supersampling");
        });
    c.BindEventCallback("toggle_aa",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.antialiasing = !m.antialiasing;
            handle.DirtyVariable("antialiasing");
        });
    c.BindEventCallback("cycle_texfilter",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.texture_filter ^= 1; refresh_labels(m);
            handle.DirtyVariable("texfilter_label");
        });
    c.BindEventCallback("cycle_crt",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.crt = (m.crt + 1) % 4; refresh_labels(m);
            handle.DirtyVariable("crt_label");
        });
    c.BindEventCallback("toggle_spu",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.spu_hq = !m.spu_hq;
            handle.DirtyVariable("spu_hq");
        });
    c.BindEventCallback("browse_bios",
        [&m, window, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            std::string p = win_pick_file(window, "Select PlayStation BIOS",
                "BIOS image (*.bin;*.rom)\0*.bin;*.rom\0All files (*.*)\0*.*\0\0");
            if (!p.empty()) {
                m.bios_path = fs::path(p).generic_string();
                handle.DirtyVariable("bios_path");
            }
        });
    auto do_browse_disc =
        [&m, window, handle, game_name_s, expected_serial, expected_crc, has_expected_crc]
        (Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            std::string p = win_pick_file(window, "Select disc image",
                "Disc image (*.cue;*.bin;*.iso)\0*.cue;*.bin;*.iso\0All files (*.*)\0*.*\0\0");
            if (!p.empty()) {
                m.disc_path = fs::path(p).generic_string();
                refresh_disc_status(m, game_name_s, expected_serial, expected_crc, has_expected_crc);
                for (const char* v : {"disc_path", "disc_file", "disc_region", "disc_serial",
                                      "v_header", "v_crc", "v_verified",
                                      "verdict_title", "verdict_detail", "verdict_state"})
                    handle.DirtyVariable(v);
            }
        };
    c.BindEventCallback("browse_disc", do_browse_disc);
    c.BindEventCallback("change_iso",  do_browse_disc);

    c.BindEventCallback("show_settings",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.view = "settings"; handle.DirtyVariable("view");
        });
    c.BindEventCallback("show_dashboard",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.view = "dashboard"; handle.DirtyVariable("view");
        });
    c.BindEventCallback("toggle_p1",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.p1_enabled = !m.p1_enabled; handle.DirtyVariable("p1_enabled");
        });
    c.BindEventCallback("toggle_p2",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.p2_enabled = !m.p2_enabled; handle.DirtyVariable("p2_enabled");
        });
    c.BindEventCallback("toggle_mc1",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.mc1_enabled = !m.mc1_enabled; handle.DirtyVariable("mc1_enabled");
        });
    c.BindEventCallback("toggle_mc2",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.mc2_enabled = !m.mc2_enabled; handle.DirtyVariable("mc2_enabled");
        });
    c.BindEventCallback("launch",
        [&m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { m.launch_requested = true; });
    c.BindEventCallback("quit",
        [&m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { m.quit_requested = true; });

    // ---- Load the document ----
    const fs::path rml = assets / "launcher.rml";
    Rml::ElementDocument* doc = context->LoadDocument(rml.generic_string());
    if (!doc) {
        std::fprintf(stderr, "launcher: failed to load %s — booting without launcher\n",
                     rml.generic_string().c_str());
        Rml::Shutdown();
        RmlGL3::Shutdown();
        return Result::Unavailable;
    }
    doc->Show();

    // ---- Main loop ----
    Result result = Result::Quit;
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                m.quit_requested = true;
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    SDL_GL_GetDrawableSize(window, &win_w, &win_h);
                    render_interface.SetViewport(win_w, win_h);
                    context->SetDimensions(Rml::Vector2i(win_w, win_h));
                }
                RmlSDL::InputEventHandler(context, ev);
                break;
            default:
                RmlSDL::InputEventHandler(context, ev);
                break;
            }
        }

        if (m.launch_requested) { result = Result::Launch; running = false; }
        if (m.quit_requested)   { result = Result::Quit;   running = false; }

        context->Update();

        render_interface.Clear();
        render_interface.BeginFrame();
        context->Render();
        render_interface.EndFrame();
        SDL_GL_SwapWindow(window);
    }

    // ---- Commit choices on launch ----
    if (result == Result::Launch) {
        io.renderer = m.renderer;             io.has_renderer = true;
        io.supersampling = m.supersampling;   io.has_supersampling = true;
        io.antialiasing = m.antialiasing;     io.has_antialiasing = true;
        io.texture_filter = m.texture_filter; io.has_texture_filter = true;
        io.screen_kind = m.crt;               io.has_screen_kind = true;
        io.spu_hq = m.spu_hq;                 io.has_spu_hq = true;
        if (!m.bios_path.empty()) { io.bios_path = fs::path(std::string(m.bios_path)); io.has_bios_path = true; }
        if (!m.disc_path.empty()) { io.disc_path = fs::path(std::string(m.disc_path)); io.has_disc_path = true; }
    }

    Rml::Shutdown();
    RmlGL3::Shutdown();
    return result;
}

} // namespace psx_launcher
