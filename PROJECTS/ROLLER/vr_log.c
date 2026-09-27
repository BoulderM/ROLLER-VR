/* PROJECTS/ROLLER/vr_log.c -- see vr_log.h */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* fileno(), dup(), dup2() under strict -std=c11 */
#endif

#include "vr_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <io.h>
#define VR_DUP   _dup
#define VR_DUP2  _dup2
#define VR_CLOSE _close
#define VR_FILENO _fileno
#else
#include <unistd.h>
#define VR_DUP   dup
#define VR_DUP2  dup2
#define VR_CLOSE close
#define VR_FILENO fileno
#endif

static FILE *s_file = NULL;
static int s_saved_out = -1;
static int s_saved_err = -1;
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

#if defined(_WIN32)
    /* roller-vr.exe is a GUI-subsystem process; started with no console attached
     * (the normal case), fds 0/1/2 have no valid OS handle behind them at all, so
     * VR_DUP()/VR_DUP2() on them below is unreliable (confirmed against mingw-w64/
     * MSVC GUI-app reports: dup2 onto an unattached stdio fd does not reliably make
     * later fprintf(stdout/stderr, ...) calls reach the target). freopen() onto the
     * null device first gives both streams a real, valid fd to redirect from, the
     * same state a console-attached process already has -- the block below then
     * behaves identically whether or not a console was actually present. Loses the
     * (minor, GUI-only) ability to fall back to a console on failure below; the
     * fatal-error path still reaches the user via a message box (vr_diag_fatal). */
    if (!freopen("NUL", "w", stdout) || !freopen("NUL", "w", stderr)) {
        fclose(f);
        return false;
    }
#endif

    fflush(stdout);
    fflush(stderr);
    s_saved_out = VR_DUP(1);
    s_saved_err = VR_DUP(2);
    if (VR_DUP2(VR_FILENO(f), 1) < 0 || VR_DUP2(VR_FILENO(f), 2) < 0) {
        if (s_saved_out >= 0) { VR_DUP2(s_saved_out, 1); VR_CLOSE(s_saved_out); }
        if (s_saved_err >= 0) { VR_DUP2(s_saved_err, 2); VR_CLOSE(s_saved_err); }
        s_saved_out = s_saved_err = -1;
        fclose(f);
        return false;
    }
    /* Nothing is pending (flushed above), so switching to unbuffered is safe. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    s_file = f;
    timespec_get(&s_start, TIME_UTC);
    return true;
}

void vr_log_close(void)
{
    if (!s_file)
        return;
    fflush(stdout);
    fflush(stderr);
    if (s_saved_out >= 0) { VR_DUP2(s_saved_out, 1); VR_CLOSE(s_saved_out); }
    if (s_saved_err >= 0) { VR_DUP2(s_saved_err, 2); VR_CLOSE(s_saved_err); }
    s_saved_out = s_saved_err = -1;
    fclose(s_file);
    s_file = NULL;
}

bool vr_log_is_open(void)
{
    return s_file != NULL;
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
    fputs(line, stderr);
}
