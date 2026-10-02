/* PROJECTS/ROLLER/vr_diag.c -- see vr_diag.h */
#include "vr_diag.h"
#include "vr_log.h"

#include <SDL3/SDL.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

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

bool vr_diag_init(void)
{
    const char *base = SDL_GetBasePath();
    char path[1024];
    snprintf(path, sizeof(path), "%sroller-vr.log", base ? base : "");

    if (vr_log_open(path)) {
        SDL_SetLogOutputFunction(vr_diag_sdl_log, NULL);
        vr_log_line("INFO", "roller-vr log opened: %s", path);
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
            SDL_SetLogOutputFunction(vr_diag_sdl_log, NULL);
            vr_log_line("WARN", "could not create %s; logging to %s instead", path, fallback_path);
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
