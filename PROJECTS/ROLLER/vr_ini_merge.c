/* PROJECTS/ROLLER/vr_ini_merge.c -- see vr_ini_merge.h */
#include "vr_ini_merge.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

typedef struct {
    char *data;
    size_t len, cap;
} VrIniBuf;

static int vr_ini_buf_put(VrIniBuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *p = (char *)realloc(b->data, cap);
        if (!p)
            return 0;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

static int vr_ini_read_all(const char *path, char **out, size_t *out_len)
{
    FILE *fp = fopen(path, "rb");
    VrIniBuf b = { 0 };
    char chunk[4096];
    size_t n;

    if (!fp)
        return 0;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        if (!vr_ini_buf_put(&b, chunk, n)) {
            fclose(fp);
            free(b.data);
            return -1;
        }
    }
    if (ferror(fp)) {
        fclose(fp);
        free(b.data);
        return -1;
    }
    fclose(fp);
    if (!b.data && !vr_ini_buf_put(&b, "", 0))
        return -1;
    *out = b.data;
    *out_len = b.len;
    return 1;
}

static int vr_ini_write_all(const char *path, const char *data, size_t len)
{
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return 0;
    int ok = fwrite(data, 1, len, fp) == len;
    ok = (fflush(fp) == 0) && ok;
    ok = (fclose(fp) == 0) && ok;
    return ok;
}

static int vr_ini_replace(const char *from, const char *to)
{
#if defined(_WIN32)
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(from, to) == 0;
#endif
}

/* Writes data to "<path>.tmp", then replaces path with it. On failure path is untouched. */
static int vr_ini_write_atomic(const char *path, const char *data, size_t len)
{
    size_t plen = strlen(path);
    char *tmp = (char *)malloc(plen + 5);
    if (!tmp)
        return 0;
    memcpy(tmp, path, plen);
    memcpy(tmp + plen, ".tmp", 5);
    int ok = vr_ini_write_all(tmp, data, len) && vr_ini_replace(tmp, path);
    if (!ok)
        remove(tmp);
    free(tmp);
    return ok;
}

static int vr_ini_exists(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return 0;
    fclose(fp);
    return 1;
}

static int vr_ini_eq_nocase(const char *a, size_t alen, const char *b)
{
    size_t blen = strlen(b);
    if (alen != blen)
        return 0;
    for (size_t i = 0; i < alen; ++i) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    }
    return 1;
}

/* The key of one line (no line ending), as InputLoadConfig() reads it, or NULL. */
static const char *vr_ini_line_key(const char *line, size_t len, size_t *key_len)
{
    size_t s = 0;
    while (s < len && isspace((unsigned char)line[s]))
        ++s;
    if (s == len || line[s] == '#' || line[s] == ';' || line[s] == '[')
        return NULL;
    const char *eq = memchr(line + s, '=', len - s);
    if (!eq)
        return NULL;
    size_t e = (size_t)(eq - line);
    while (e > s && isspace((unsigned char)line[e - 1]))
        --e;
    *key_len = e - s;
    return line + s;
}

static int vr_ini_is_section(const char *line, size_t len, const char *name)
{
    size_t s = 0;
    while (s < len && isspace((unsigned char)line[s]))
        ++s;
    if (s == len || line[s] != '[')
        return 0;
    if (!name)
        return 1;
    const char *close = memchr(line + s, ']', len - s);
    return close && vr_ini_eq_nocase(line + s + 1, (size_t)(close - (line + s + 1)), name);
}

VrIniMergeResult vr_ini_merge_keys(const char *path, const char *backup_path,
                                   const char *const *keys, const char *const *lines, int count)
{
    char *src = NULL;
    size_t src_len = 0;
    int rd = vr_ini_read_all(path, &src, &src_len);
    if (rd == 0)
        return VR_INI_MERGE_MISSING;
    if (rd < 0 || count < 0 || count > 64)
        return VR_INI_MERGE_ERROR;

    bool found[64] = { false };
    const char *eol = "\n";
    VrIniBuf out = { 0 };
    int ok = 1;
    size_t insert_at = (size_t)-1; /* output offset at the end of the [Input] section */
    int in_input = 0;

    /* Line ending to use for inserted lines: the file's own, from its first line. */
    {
        const char *nl = memchr(src, '\n', src_len);
        if (nl && nl > src && nl[-1] == '\r')
            eol = "\r\n";
    }

    size_t pos = 0;
    while (ok && pos < src_len) {
        size_t start = pos;
        while (pos < src_len && src[pos] != '\n')
            ++pos;
        size_t content_end = pos;
        if (pos < src_len)
            ++pos; /* include '\n' */
        if (content_end > start && src[content_end - 1] == '\r')
            --content_end;
        const char *line = src + start;
        size_t clen = content_end - start;

        if (vr_ini_is_section(line, clen, NULL)) {
            if (in_input)
                insert_at = out.len; /* [Input] ends before this section header */
            in_input = vr_ini_is_section(line, clen, "Input");
        }

        size_t key_len = 0;
        const char *key = vr_ini_line_key(line, clen, &key_len);
        int replaced = 0;
        for (int i = 0; key && i < count; ++i) {
            if (vr_ini_eq_nocase(key, key_len, keys[i])) {
                ok = vr_ini_buf_put(&out, lines[i], strlen(lines[i])) &&
                     vr_ini_buf_put(&out, src + content_end, pos - content_end);
                found[i] = true;
                replaced = 1;
                break;
            }
        }
        if (!replaced)
            ok = vr_ini_buf_put(&out, line, pos - start);
        if (ok && in_input)
            insert_at = out.len;
    }

    /* Missing keys: at the end of [Input], else under a new [Input] at the end. */
    if (ok) {
        VrIniBuf add = { 0 };
        int any_missing = 0;
        for (int i = 0; ok && i < count; ++i) {
            if (found[i])
                continue;
            any_missing = 1;
            ok = vr_ini_buf_put(&add, lines[i], strlen(lines[i])) && vr_ini_buf_put(&add, eol, strlen(eol));
        }
        if (ok && any_missing) {
            if (insert_at == (size_t)-1) {
                if (out.len && out.data[out.len - 1] != '\n')
                    ok = vr_ini_buf_put(&out, eol, strlen(eol));
                ok = ok && vr_ini_buf_put(&out, "[Input]", 7) && vr_ini_buf_put(&out, eol, strlen(eol)) &&
                     vr_ini_buf_put(&out, add.data, add.len);
            } else {
                /* The section's last line may lack a newline (end of file). */
                VrIniBuf merged = { 0 };
                ok = vr_ini_buf_put(&merged, out.data ? out.data : "", insert_at);
                if (ok && insert_at && out.data[insert_at - 1] != '\n')
                    ok = vr_ini_buf_put(&merged, eol, strlen(eol));
                ok = ok && vr_ini_buf_put(&merged, add.data, add.len) &&
                     vr_ini_buf_put(&merged, out.data + insert_at, out.len - insert_at);
                free(out.data);
                out = merged;
            }
        }
        free(add.data);
    }
    if (!ok) {
        free(src);
        free(out.data);
        return VR_INI_MERGE_ERROR;
    }

    if (out.len == src_len && memcmp(out.data, src, src_len) == 0) {
        free(src);
        free(out.data);
        return VR_INI_MERGE_UNCHANGED;
    }

    /* The first backup is the user's original; never replace it. */
    VrIniMergeResult result = VR_INI_MERGE_WRITTEN;
    if (!vr_ini_exists(backup_path) && !vr_ini_write_atomic(backup_path, src, src_len))
        result = VR_INI_MERGE_ERROR;
    if (result == VR_INI_MERGE_WRITTEN && !vr_ini_write_atomic(path, out.data, out.len))
        result = VR_INI_MERGE_ERROR;
    free(src);
    free(out.data);
    return result;
}
