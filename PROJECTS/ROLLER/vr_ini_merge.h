/* PROJECTS/ROLLER/vr_ini_merge.h
 *
 * Targeted edit of a ROLLER.INI-style "Key=Value" file (pure C, no SDL), used by the
 * VR build to persist the automatic player-1 VR controller binding without rewriting
 * the rest of the user's file. Every line whose key is not being set is kept byte for
 * byte, including comments, sections, unknown keys and line endings.
 *
 * Keys are matched the way the game's reader (InputLoadConfig()) matches them: the
 * text before the first '=', trimmed, case-insensitive; blank lines and lines starting
 * with '#', ';' or '[' are never keys. Every occurrence of a key is replaced (the
 * reader lets the last one win, so replacing them all keeps the meaning unambiguous).
 * Keys that are missing are inserted at the end of the "[Input]" section, or appended
 * under a new "[Input]" line when the file has none (the reader ignores sections).
 *
 * The write is atomic: the new content goes to "<path>.tmp", which then replaces
 * <path> (MoveFileEx(MOVEFILE_REPLACE_EXISTING) on Windows, rename() elsewhere).
 * Before the first change, the original bytes are copied to backup_path. */
#ifndef VR_INI_MERGE_H
#define VR_INI_MERGE_H

#include <stdbool.h>

typedef enum {
    VR_INI_MERGE_WRITTEN = 0,   /* file changed (backup made if *backed_up was false) */
    VR_INI_MERGE_UNCHANGED,     /* every key already had exactly this line; nothing written */
    VR_INI_MERGE_MISSING,       /* path does not exist; nothing written */
    VR_INI_MERGE_ERROR          /* read/backup/write/replace failed; the original is untouched */
} VrIniMergeResult;

/* keys[i] is set to the full line lines[i] (no line ending, e.g. "P1Left=axis;..."),
 * for i < count. *backed_up: in/out; when false, backup_path is (over)written with the
 * original content before the change and the flag is set, so one run backs up once. */
VrIniMergeResult vr_ini_merge_keys(const char *path, const char *backup_path,
                                   const char *const *keys, const char *const *lines, int count,
                                   bool *backed_up);

#endif /* VR_INI_MERGE_H */
