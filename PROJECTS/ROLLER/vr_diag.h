/* PROJECTS/ROLLER/vr_diag.h
 *
 * roller-vr diagnostics that need SDL: opens roller-vr.log next to the
 * executable (see vr_log.h), routes SDL_Log output into it, and provides the
 * fatal-error exit used by the VR startup path. */
#ifndef VR_DIAG_H
#define VR_DIAG_H

#include <stdbool.h>

/* Opens roller-vr.log in the executable's directory and captures stdout, stderr
 * and SDL_Log output in it. Returns false (logging stays on the console) if the
 * file cannot be created. Call once, early, after SDL_Init(). */
bool vr_diag_init(void);

/* Logs the message, shows it in a message box on Windows (where a GUI process
 * has no visible console), then exits the process with status 1. Never returns. */
void vr_diag_fatal(const char *fmt, ...);

#endif /* VR_DIAG_H */
