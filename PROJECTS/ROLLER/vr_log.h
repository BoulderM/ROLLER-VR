/* PROJECTS/ROLLER/vr_log.h
 *
 * Diagnostics log for roller-vr: one file next to the exe that captures
 * everything written to stdout and stderr (the VR runtime code prints with
 * printf/fprintf(stderr)), plus timestamped lines from vr_log_line(). Pure C
 * (stdio + dup/dup2 only, no SDL) so it can be unit-tested on its own. */
#ifndef VR_LOG_H
#define VR_LOG_H

#include <stdbool.h>
#include <stddef.h>

/* Truncates/creates `path` and redirects stdout and stderr into it (unbuffered,
 * so a crash loses nothing). Returns false, leaving stdout/stderr untouched, if
 * the file cannot be created. A second call while open is a no-op returning true. */
bool vr_log_open(const char *path);

/* Restores the original stdout/stderr and closes the file. Safe to call when not open. */
void vr_log_close(void);

bool vr_log_is_open(void);

/* Formats "[<sec>.<ms>s] <level> <message>\n" into out. The result is always NUL
 * terminated and always ends in '\n', truncating the message if it does not fit.
 * out_size must be at least 2. Returns the length written, excluding the NUL. */
size_t vr_log_format_line(char *out, size_t out_size, long elapsed_ms,
                          const char *level, const char *message);

/* Writes one formatted line to stderr (the log file while open). elapsed time is
 * measured from vr_log_open(); before that it prints 0.000s. */
void vr_log_line(const char *level, const char *fmt, ...);

#endif /* VR_LOG_H */
