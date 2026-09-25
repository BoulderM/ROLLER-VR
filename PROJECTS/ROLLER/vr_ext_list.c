/* PROJECTS/ROLLER/vr_ext_list.c -- see vr_ext_list.h */
#include "vr_ext_list.h"

#include <string.h>

void vr_ext_list_init(VrExtList *list)
{
    if (!list)
        return;
    list->count = 0;
}

bool vr_ext_list_contains(const VrExtList *list, const char *name)
{
    if (!list || !name)
        return false;
    for (unsigned int i = 0; i < list->count; i++) {
        if (strcmp(list->names[i], name) == 0)
            return true;
    }
    return false;
}

bool vr_ext_list_add(VrExtList *list, const char *name)
{
    if (!list || !name || name[0] == '\0')
        return false;
    size_t len = strlen(name);
    if (len >= VR_EXT_NAME_MAX)
        return false;
    if (vr_ext_list_contains(list, name))
        return true;
    if (list->count >= VR_EXT_LIST_MAX)
        return false;
    memcpy(list->names[list->count], name, len + 1);
    list->count++;
    return true;
}

bool vr_ext_list_add_spaced(VrExtList *list, const char *spaced)
{
    if (!list)
        return false;
    if (!spaced)
        return true;

    bool ok = true;
    const char *p = spaced;
    while (*p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != ' ')
            p++;
        size_t len = (size_t)(p - start);
        if (len >= VR_EXT_NAME_MAX) {
            ok = false;
            continue;
        }
        char token[VR_EXT_NAME_MAX];
        memcpy(token, start, len);
        token[len] = '\0';
        if (!vr_ext_list_add(list, token))
            ok = false;
    }
    return ok;
}

unsigned int vr_ext_list_ptrs(const VrExtList *list, const char **out, unsigned int out_cap)
{
    if (!list || !out)
        return 0;
    unsigned int n = list->count < out_cap ? list->count : out_cap;
    for (unsigned int i = 0; i < n; i++)
        out[i] = list->names[i];
    return n;
}

VrVkBinding vr_choose_vk_binding(bool runtime_has_vulkan_enable, bool runtime_has_vulkan_enable2)
{
    if (runtime_has_vulkan_enable)
        return VR_VK_BINDING_ENABLE_V1;
    if (runtime_has_vulkan_enable2)
        return VR_VK_BINDING_ENABLE2_FALLBACK;
    return VR_VK_BINDING_NONE;
}
