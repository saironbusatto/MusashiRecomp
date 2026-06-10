#ifndef PSXRECOMP_CRASH_TRACE_H
#define PSXRECOMP_CRASH_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install signal/SEH/atexit handlers. Call once early in main(). */
void psx_crash_trace_install_handlers(void);

/* Manually trigger a crash dump. `reason` is a short string identifying
 * the trigger (e.g. "fail_fast_unknown_dispatch", "trap_crash"). On
 * Windows, `seh_info` is the EXCEPTION_POINTERS* from the SEH filter or
 * NULL. Writes to psx_last_run_report.json (overwrite). */
void psx_crash_trace_dump(const char *reason, void *seh_info);

/* Tag a deliberate exit() so the atexit report can attribute it
 * (e.g. "tcp_quit", "sdl_window_close"). An "atexit" report whose
 * exit_origin is still "unknown" means an untagged exit fired. */
void psx_crash_trace_set_exit_origin(const char *origin);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_CRASH_TRACE_H */
