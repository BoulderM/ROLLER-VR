/* PROJECTS/ROLLER/vr_diag.c -- see vr_diag.h */
#include "vr_diag.h"
#include "vr_log.h"

#include <SDL3/SDL.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *sdl_priority_name(SDL_LogPriority priority)
{
    switch (priority) {
    case SDL_LOG_PRIORITY_VERBOSE:  return "VERBOSE";
    case SDL_LOG_PRIORITY_DEBUG:    return "DEBUG";
    case SDL_LOG_PRIORITY_INFO:     return "INFO";
    case SDL_LOG_PRIORITY_WARN:     return "WARN";
    case SDL_LOG_PRIORITY_ERROR:    return "ERROR";
    case SDL_LOG_PRIORITY_CRITICAL: return "CRITICAL";
    default:                        return "LOG";
    }
}

static void SDLCALL vr_diag_sdl_log(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
    (void)userdata;
    vr_log_line(sdl_priority_name(priority), "[sdl:%d] %s", category, message ? message : "");
}

/* Writes a known marker line, flushes it, then independently re-opens and
 * re-reads the same file from disk to confirm the write actually landed on
 * disk rather than just appearing to succeed. This isolates the logging
 * mechanism itself from everything that runs after it: the file existing
 * but ending up empty (2026-10-02, real Windows hardware; a Wine
 * reproduction of the same freopen/dup2/setvbuf sequence did NOT show this,
 * so whatever it is, it's specific to the genuine Windows CRT, not a logic
 * bug in vr_log.c's approach as such) needs a direct answer, not a guess.
 * On failure, fills out_detail (at least 256 bytes) with the exact evidence. */
static bool vr_diag_verify_log(const char *path, char *out_detail, size_t out_detail_size)
{
    vr_log_line("INFO", "SELF-TEST marker line");
    bool flush_ok = vr_log_flush();
    int flush_errno = errno;

    FILE *check = fopen(path, "rb");
    bool found = false;
    long size = -1;
    if (check) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof(buf) - 1, check);
        buf[n] = '\0';
        found = (n > 0) && (strstr(buf, "SELF-TEST marker line") != NULL);
        fseek(check, 0, SEEK_END);
        size = ftell(check);
        fclose(check);
    }

    if (flush_ok && found)
        return true;

    snprintf(out_detail, out_detail_size,
        "vr_log_flush() %s (errno=%d: %s); re-reading \"%s\" found %ld byte(s) "
        "on disk, self-test marker %s.",
        flush_ok ? "succeeded" : "FAILED", flush_errno, strerror(flush_errno), path, size,
        found ? "present" : "NOT present");
    return false;
}

/* Common tail of a successful vr_log_open(): route SDL_Log into it, log that
 * it opened, then immediately verify with vr_diag_verify_log() and surface
 * any discrepancy in its own message box -- separate from vr_diag_fatal()'s,
 * so a self-test failure is never confused with whatever runs afterward. */
static void vr_diag_on_opened(const char *path, const char *note)
{
    SDL_SetLogOutputFunction(vr_diag_sdl_log, NULL);
    if (note)
        vr_log_line("WARN", "%s", note);
    vr_log_line("INFO", "roller-vr log opened: %s", path);

    char detail[384];
    if (vr_diag_verify_log(path, detail, sizeof(detail)))
        return;

#if defined(_WIN32)
    char msg[640];
    snprintf(msg, sizeof(msg), "Log self-test failed for %s:\n%s", path, detail);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "ROLLER VR - log self-test", msg, NULL);
#else
    fprintf(stderr, "vr_diag: log self-test failed for %s: %s\n", path, detail);
#endif
}

bool vr_diag_init(void)
{
    const char *base = SDL_GetBasePath();
    char path[1024];
    snprintf(path, sizeof(path), "%sroller-vr.log", base ? base : "");

    if (vr_log_open(path)) {
        vr_diag_on_opened(path, NULL);
        return true;
    }

    /* The executable's own directory isn't always writable (run straight off
     * a read-only network share, a protected install dir, antivirus holding
     * it, ...). Fall back to the per-user app-data directory SDL guarantees
     * is writable before giving up -- a silently-missing log is exactly the
     * failure mode that leaves a VR startup crash undiagnosable (2026-10-02:
     * the OpenXR-prep error box appeared with no roller-vr.log anywhere). */
    char *pref = SDL_GetPrefPath("ROLLER", "roller-vr");
    if (pref) {
        char fallback_path[1024];
        snprintf(fallback_path, sizeof(fallback_path), "%sroller-vr.log", pref);
        SDL_free(pref);
        if (vr_log_open(fallback_path)) {
            char note[1200];
            snprintf(note, sizeof(note), "could not create %s; logging to %s instead", path, fallback_path);
            vr_diag_on_opened(fallback_path, note);
            return true;
        }
    }

    /* Both locations failed. This must not be silent: it's a GUI-subsystem
     * process with no console, so fprintf(stderr, ...) alone is invisible
     * (that was the bug -- this function used to return here with no other
     * signal at all). */
#if defined(_WIN32)
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "ROLLER VR",
        "Could not create a VR log file next to the executable or in the "
        "per-user app data folder. Continuing without VR diagnostics -- if "
        "startup fails next, there will be no roller-vr.log to send back.", NULL);
#else
    fprintf(stderr, "vr_diag: could not create %s or a fallback in the SDL pref path; "
                     "logging to the console instead\n", path);
#endif
    return false;
}

void vr_diag_fatal(const char *fmt, ...)
{
    char message[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt ? fmt : "fatal error", args);
    va_end(args);

    vr_log_line("FATAL", "%s", message);
#if defined(_WIN32)
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ROLLER VR", message, NULL);
#endif
    vr_log_close();
    exit(1);
}
