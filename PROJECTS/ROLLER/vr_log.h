/* PROJECTS/ROLLER/vr_log.h
 *
 * Diagnostics log for roller-vr: one file, written to directly (not via
 * stdout/stderr redirection -- see vr_log.c's header comment for why), that
 * vr_log_line() and the SDL_Log hook (vr_diag.c) both write to. Pure C
 * (plain stdio only) so it can be unit-tested on its own. */
#ifndef VR_LOG_H
#define VR_LOG_H

#include <stdbool.h>
#include <stddef.h>

/* Truncates/creates `path` (unbuffered, so a crash loses nothing). Returns
 * false if the file cannot be created. A second call while open is a no-op
 * returning true. */
bool vr_log_open(const char *path);

/* Closes the file. Safe to call when not open. */
void vr_log_close(void);

bool vr_log_is_open(void);

/* Flushes the log file to the OS. Returns false if not open, or if the flush
 * itself fails (check errno). Writes are unbuffered already, so this is
 * mainly for callers that want to verify the file is actually writable right
 * now, not for durability. */
bool vr_log_flush(void);

/* Formats "[<sec>.<ms>s] <level> <message>\n" into out. The result is always NUL
 * terminated and always ends in '\n', truncating the message if it does not fit.
 * out_size must be at least 2. Returns the length written, excluding the NUL. */
size_t vr_log_format_line(char *out, size_t out_size, long elapsed_ms,
                          const char *level, const char *message);

/* Writes one formatted line to the log file while open, else to stderr.
 * elapsed time is measured from vr_log_open(); before that it prints 0.000s. */
void vr_log_line(const char *level, const char *fmt, ...);

#endif /* VR_LOG_H */
