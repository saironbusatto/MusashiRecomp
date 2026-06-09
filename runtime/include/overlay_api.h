#ifndef OVERLAY_API_H
#define OVERLAY_API_H

/* Shared ABI between psx-runtime.exe and overlay DLLs.
 *
 * After LoadLibrary, the runtime calls overlay_init() with this struct.
 * The DLL stores the pointers and routes all cross-module calls through them.
 */

#include "cpu_state.h"
#include <stdint.h>

typedef struct {
    /* Core dispatch: routes call_by_address() and out-of-overlay jal */
    void (*dispatch_call)(CPUState *cpu, uint32_t addr, uint32_t ra);
    /* Interrupt check: called after every function return in overlay */
    void (*check_interrupts)(CPUState *cpu);
    /* GTE coprocessor 2 execution */
    void (*gte_execute)(CPUState *cpu, uint32_t cmd);
    /* MIPS syscall (break/syscall instructions) */
    void (*psx_syscall)(CPUState *cpu, uint32_t code);
    /* Unresolved dispatch target */
    void (*psx_unknown_dispatch)(CPUState *cpu, uint32_t addr, uint32_t phys);
    /* Debug instrumentation: called at every function entry (may be NULL) */
    void (*log_call_entry)(uint32_t func_addr);
    /* RestoreState/ReturnFromException longjmp escape (interrupts.c). The
     * recompiler emits this at longjmp-return sites in exception-context
     * kernel code (the install-slot / kernel-window class). Appended LAST:
     * overlay_init copies the struct by value, so older DLLs built against
     * the shorter struct simply never read this member. */
    void (*psx_restore_state_escape)(void);
} OverlayCallbacks;

#ifdef __cplusplus
extern "C" {
#endif

/* Exported by every overlay DLL.  Call once after LoadLibrary. */
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
void overlay_init(const OverlayCallbacks *cbs);

#ifdef __cplusplus
}
#endif

#endif /* OVERLAY_API_H */
