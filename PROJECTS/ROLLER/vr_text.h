/* PROJECTS/ROLLER/vr_text.h -- tiny text helpers shared by the VR settings code. Header-only, pure C. */
#ifndef VR_TEXT_H
#define VR_TEXT_H

#include <stdbool.h>
#include <stddef.h>

static inline char vr_text_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static inline bool vr_text_is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* true if text is `name` (lower case) ignoring case and surrounding white space, and nothing else. */
static inline bool vr_text_matches(const char *text, const char *name)
{
    if (!text || !name)
        return false;
    while (vr_text_is_space(*text))
        ++text;
    size_t i = 0;
    while (name[i] && text[i] && vr_text_lower(text[i]) == name[i])
        ++i;
    if (name[i] != '\0')
        return false;
    const char *rest = text + i;
    while (vr_text_is_space(*rest))
        ++rest;
    return *rest == '\0' && i > 0;
}

#endif /* VR_TEXT_H */
