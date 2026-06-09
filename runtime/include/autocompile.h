/* autocompile.h — background overlay compile for variant-capture automation
 * (step 2.8). Spawns the configured compile command (compile_overlays.py
 * invocation from game.toml [runtime] overlay_autocompile_cmd) after an
 * automatic capture, collects its output into an in-memory ring (no log
 * files — CLAUDE.md §3; read it via the autocompile_status TCP command),
 * and on success has the emu thread rescan the overlay-DLL cache so the new
 * variant goes native in-session, without a restart. */
#ifndef PSXRECOMP_AUTOCOMPILE_H
#define PSXRECOMP_AUTOCOMPILE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Store the command line + working directory. Empty cmd disables. */
void autocompile_configure(const char *cmd, const char *cwd);
int  autocompile_configured(void);

/* 1 while a compile process is running. */
int  autocompile_busy(void);

/* Kick a compile if configured and idle. Returns 1 if started. */
int  autocompile_request(void);

/* Emu-thread tick: applies a finished compile (cache rescan on success).
 * Must be called from the same thread that owns the overlay loader. */
void autocompile_poll_main(void);

/* JSON status blob for the debug server: state, run/fail counters, last
 * exit code, and the output tail. Returns bytes written. */
int  autocompile_status_json(char *out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_AUTOCOMPILE_H */
