/* text_xlate.h — on-the-fly string translation / localization (framework).
 *
 * A reusable psxrecomp feature: capture the game's source strings from guest
 * memory and substitute translated bytes on the fly so a JP-only title can be
 * played in the target language. See docs/STRING_TRANSLATION.md for the design.
 *
 * Mechanism (all always-on, hung off the psx_dispatch chokepoint):
 *  - CAPTURE: on every dispatch, scan the arg registers for pointers to source
 *    text records (encoding-profile validated). Every distinct record is hashed
 *    (FNV-1a64) and recorded into an in-memory inventory ring queryable over the
 *    TCP debug server (the "enumerate every string" authoring source). Never
 *    arm-then-run — the ring records from boot; the debug command queries it.
 *  - APPLY: when an arg points at a record present in the loaded translation
 *    table, transcode the target-language string to the game's glyph codes,
 *    write it into transient guest-stack scratch (below $sp), and repoint the
 *    arg register — the ORIGINAL draw routine then renders the replacement.
 *    KV-gated, so a stray pointer that isn't in the table is left untouched.
 *
 * Encoding is abstracted behind an EncodingProfile (validator + terminator rules
 * + UTF-8 -> glyph transcoder) selected per title; Tsumu uses the shift_jis
 * profile. No generated-code edits, no regen: the hook is called from
 * fntrace_record (runtime source), so it works in Release.
 */
#ifndef PSXRECOMP_TEXT_XLATE_H
#define PSXRECOMP_TEXT_XLATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* Called at the top of every psx_dispatch iteration (from fntrace_record),
 * BEFORE the target function runs, with cpu->gpr[4..7] holding this call's
 * args. Cheap no-op when the module is uninitialised or disarmed. */
void text_xlate_on_dispatch(struct CPUState* cpu, uint32_t target);

/* One-time init. Loads every translations/ *.toml under project_root and
 * selects the active `language` column. language "jp"/"off"/"" (or no tables)
 * disables APPLY; CAPTURE still runs (always-on inventory). The PSX_LANG env
 * var overrides `language` when set. Safe to call once at startup; copies what
 * it needs. */
void text_xlate_init(const char* project_root, const char* language);

/* Debug-server surface (observability; no log files — Rule 3). subcmd:
 *   "stats"   -> counters (calls, hits, distinct captured, table size, lang)
 *   "dump"    -> JSON array of every captured record {hash, addr, pc, len,
 *                count, translated, sjis_hex} — the authoring inventory
 *   "todo"    -> JSON array of captured records with NO table entry yet
 *   "reload"  -> re-read the translation table from disk
 * Returns bytes written into out (<= cap). */
int text_xlate_debug_json(const char* subcmd, char* out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_TEXT_XLATE_H */
