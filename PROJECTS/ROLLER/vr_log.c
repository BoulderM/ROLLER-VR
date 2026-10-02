/* PROJECTS/ROLLER/vr_log.c -- see vr_log.h */
#include "vr_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static FILE *s_file = NULL;
static struct timespec s_start;

bool vr_log_open(const char *path)
{
    if (s_file)
        return true;
    if (!path)
        return false;

    FILE *f = fopen(path, "wb");
    if (!f)
        return false;

    /* Unbuffered: each vr_log_line()/vr_log_flush() call reaches the OS
     * immediately, so a crash or exit() right after loses nothing. (An
     * earlier version of this file instead dup2()'d the process's own
     * stdout/stderr file descriptors onto this file, to also catch any
     * stray raw printf()/fprintf(stderr, ...) elsewhere in the codebase.
     * That relied on dup2() behaving a specific way for a GUI-subsystem
     * process with no console; on real Windows hardware with this
     * toolchain's CRT it produced a log file that opened fine but silently
     * received zero bytes -- EBADF out of fflush(stderr) moments after a
     * successful dup2(), confirmed not reproducible under Wine, so it's
     * specific to the genuine Windows CRT, not a logic bug in the dup2
     * sequence itself (2026-10-02). Writing directly to this FILE* instead
     * avoids the whole class of platform-specific fd-redirection quirks;
     * the cost is that stray raw printf/fprintf calls outside vr_log_line()
     * and the SDL_Log hook no longer land in the log, which in practice
     * isn't a real loss -- every VR startup/runtime diagnostic already goes
     * through vr_log_line() or SDL_Log directly, never raw stdio.) */
    setvbuf(f, NULL, _IONBF, 0);

    s_file = f;
    timespec_get(&s_start, TIME_UTC);
    return true;
}

void vr_log_close(void)
{
    if (!s_file)
        return;
    fclose(s_file);
    s_file = NULL;
}

bool vr_log_is_open(void)
{
    return s_file != NULL;
}

bool vr_log_flush(void)
{
    if (!s_file)
        return false;
    return fflush(s_file) == 0;
}

size_t vr_log_format_line(char *out, size_t out_size, long elapsed_ms,
                          const char *level, const char *message)
{
    if (!out || out_size < 2)
        return 0;
    if (elapsed_ms < 0)
        elapsed_ms = 0;
    if (!level)
        level = "INFO";
    if (!message)
        message = "";

    int n = snprintf(out, out_size, "[%ld.%03lds] %s %s\n",
                     elapsed_ms / 1000, elapsed_ms % 1000, level, message);
    if (n < 0) {
        out[0] = '\n';
        out[1] = '\0';
        return 1;
    }
    if ((size_t)n >= out_size) {
        /* Truncated: keep the "always ends in newline" guarantee. */
        out[out_size - 2] = '\n';
        out[out_size - 1] = '\0';
        return out_size - 1;
    }
    return (size_t)n;
}

void vr_log_line(const char *level, const char *fmt, ...)
{
    char message[1024];
    char line[1200];

    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt ? fmt : "", args);
    va_end(args);

    long elapsed_ms = 0;
    if (s_file) {
        struct timespec now;
        timespec_get(&now, TIME_UTC);
        elapsed_ms = (long)((now.tv_sec - s_start.tv_sec) * 1000 +
                            (now.tv_nsec - s_start.tv_nsec) / 1000000);
    }
    vr_log_format_line(line, sizeof(line), elapsed_ms, level, message);
    /* Falls back to stderr if the log isn't open (e.g. called before
     * vr_log_open() or after a failed open) -- harmless on Windows (writes
     * to nothing, since this is a GUI-subsystem process with no console),
     * and still useful on Linux/console builds and manual testing. */
    fputs(line, s_file ? s_file : stderr);
}
