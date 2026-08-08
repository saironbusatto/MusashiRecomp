/* beetle_ramdump — minimal libretro host: boot the Beetle PSX core and dump
 * main RAM.
 *
 * WHY THIS EXISTS: written when the full psx-beetle oracle target could not
 * link (it needed eight symbols no committed patch supplied). That gap is
 * closed as of 2026-08-08 — docs/beetle_oracle_instrumentation.patch supplies
 * them and psx-beetle links. This tool survives because it is still the
 * shortest path to the one thing the HLE boot-hang diagnosis needs: the
 * post-boot contents of kernel RAM from a known-good implementation, to diff
 * against what our HLE boot-skip synthesises. No debug server, no window, no
 * game loop. Use psx-beetle for anything interactive.
 *
 * The core is linked statically (STATIC_LINKING=1 build), so retro_* is called
 * directly rather than dlopen'd.
 *
 * Build (see tools/build_ramdump.sh):
 *   cc -O2 -o beetle_ramdump tools/beetle_ramdump.c \
 *      beetle-psx/libmednafen_psx.a -lstdc++ -lm -lpthread -lz
 *
 * Usage:
 *   beetle_ramdump <system_dir> <disc.cue> <frames> <out.bin>
 *
 * <system_dir> must contain the PS1 BIOS under a name the core looks for. For
 * a valid comparison this MUST be the same BIOS our runtime recompiles
 * (SCPH1001.BIN): kernel table layout differs between BIOS revisions, so
 * diffing against a different revision would report differences that are not
 * bugs. The core wants "scph5501.bin" for NTSC-U, so SCPH1001.BIN is staged
 * under that name; the core warns about the SHA1 and proceeds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#include "libretro.h"

static char g_system_dir[4096];
static char g_save_dir[4096];

static void log_printf(enum retro_log_level level, const char *fmt, ...)
{
    /* Core diagnostics go to stderr so stdout stays clean for our own output. */
    const char *tag = level == RETRO_LOG_ERROR ? "ERR"
                    : level == RETRO_LOG_WARN  ? "WRN" : "INF";
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[core %s] ", tag);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static bool environment_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = g_system_dir;
        return true;

    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = g_save_dir;
        return true;

    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        /* Accept whatever it asks for; we never look at the framebuffer. */
        return true;

    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        struct retro_log_callback *cb = (struct retro_log_callback *)data;
        cb->log = log_printf;
        return true;
    }

    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;

    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        /* No frontend config: every core option takes its default. Leaving
         * value NULL is the documented way to say "not set". */
        struct retro_variable *var = (struct retro_variable *)data;
        var->value = NULL;
        return false;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;

    default:
        return false;
    }
}

static void video_refresh_cb(const void *d, unsigned w, unsigned h, size_t p)
{ (void)d; (void)w; (void)h; (void)p; }

static void audio_sample_cb(int16_t l, int16_t r) { (void)l; (void)r; }

static size_t audio_sample_batch_cb(const int16_t *data, size_t frames)
{ (void)data; return frames; }

static void input_poll_cb(void) {}

static int16_t input_state_cb(unsigned port, unsigned device,
                              unsigned index, unsigned id)
{ (void)port; (void)device; (void)index; (void)id; return 0; }

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr,
                "usage: %s <system_dir> <disc.cue> <frames> <out.bin>\n",
                argv[0]);
        return 2;
    }

    snprintf(g_system_dir, sizeof g_system_dir, "%s", argv[1]);
    snprintf(g_save_dir,   sizeof g_save_dir,   "%s", argv[1]);
    const char *disc   = argv[2];
    const long  frames = strtol(argv[3], NULL, 10);
    const char *outp   = argv[4];

    retro_set_environment(environment_cb);
    retro_set_video_refresh(video_refresh_cb);
    retro_set_audio_sample(audio_sample_cb);
    retro_set_audio_sample_batch(audio_sample_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);

    retro_init();

    struct retro_game_info info;
    memset(&info, 0, sizeof info);
    info.path = disc;          /* content is loaded from the path, not memory */

    if (!retro_load_game(&info)) {
        fprintf(stderr, "beetle_ramdump: retro_load_game failed for %s\n", disc);
        retro_deinit();
        return 1;
    }

    for (long i = 0; i < frames; i++) {
        retro_run();
        if ((i + 1) % 100 == 0)
            fprintf(stderr, "beetle_ramdump: frame %ld/%ld\n", i + 1, frames);
    }

    void  *ram  = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t size = retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (!ram || !size) {
        fprintf(stderr, "beetle_ramdump: core exposed no SYSTEM_RAM\n");
        retro_unload_game();
        retro_deinit();
        return 1;
    }

    FILE *f = fopen(outp, "wb");
    if (!f) {
        perror("beetle_ramdump: fopen");
        retro_unload_game();
        retro_deinit();
        return 1;
    }
    fwrite(ram, 1, size, f);
    fclose(f);

    fprintf(stderr, "beetle_ramdump: wrote %zu bytes of RAM to %s after %ld frames\n",
            size, outp, frames);

    retro_unload_game();
    retro_deinit();
    return 0;
}
