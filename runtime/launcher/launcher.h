// launcher.h — integrated RmlUi launcher front-end.
//
// Shown in the runtime's SDL/OpenGL window before the emulator boots: the user
// picks renderer / supersampling / AA / colour model / SPU-HQ, the BIOS, disc,
// memory cards and controllers, then presses LAUNCH. The chosen values are
// written back into the UserSettings the runtime then applies (and persisted to
// settings.toml by the caller).
//
// Design note — the launcher does NOT create or own the window or GL context;
// the caller passes an already-current GL 3.3 context. This keeps the module a
// pure overlay so a future "re-open settings while the game is running" path can
// reuse it without owning the window lifecycle.

#pragma once

struct SDL_Window;

namespace PSXRecompV4 { struct UserSettings; }

namespace psx_launcher {

enum class Result {
    Launch,  // user pressed LAUNCH — proceed to boot with `io`
    Quit,    // user closed the window — caller should exit
    Unavailable, // launcher could not initialise (assets/GL); caller boots as if skipped
};

// Run the launcher loop to completion. `gl_context` is an SDL_GLContext (void*
// to avoid leaking SDL types into this header) already created and current on
// `window`. `io` is seeded with the effective settings (game.toml ∪ settings.toml)
// and, on Result::Launch, updated in place with the user's choices. `assets_dir`
// is the directory holding launcher.rml / .rcss / fonts.
Result run(SDL_Window* window, void* gl_context,
           PSXRecompV4::UserSettings& io,
           const char* game_name, const char* assets_dir);

} // namespace psx_launcher
