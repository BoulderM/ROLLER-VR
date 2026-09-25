/* PROJECTS/ROLLER/vr_ext_list.h
 *
 * Small, dependency-free helpers for negotiating Vulkan extensions between the
 * OpenXR runtime and SDL_GPU: a fixed-capacity de-duplicated name list that can
 * parse OpenXR's space-separated extension strings (the format returned by
 * xrGetVulkanInstanceExtensionsKHR / xrGetVulkanDeviceExtensionsKHR), and the
 * choice of which XR_KHR_vulkan_enable* variant to use. Pure C so it can be
 * unit-tested without OpenXR or Vulkan headers. */
#ifndef VR_EXT_LIST_H
#define VR_EXT_LIST_H

#include <stdbool.h>

#define VR_EXT_LIST_MAX  64
#define VR_EXT_NAME_MAX  128

typedef struct {
    char         names[VR_EXT_LIST_MAX][VR_EXT_NAME_MAX];
    unsigned int count;
} VrExtList;

void vr_ext_list_init(VrExtList *list);
bool vr_ext_list_contains(const VrExtList *list, const char *name);

/* Adds `name` unless already present (then it is a no-op returning true).
 * Returns false, leaving the list unchanged, for a NULL/empty name, a name of
 * VR_EXT_NAME_MAX characters or more, or when the list is full. */
bool vr_ext_list_add(VrExtList *list, const char *name);

/* Adds every name in a space-separated string (repeated spaces and leading or
 * trailing spaces are fine; NULL or "" adds nothing). Returns false if any name
 * could not be added; names before the failing one stay added. */
bool vr_ext_list_add_spaced(VrExtList *list, const char *spaced);

/* Writes up to out_cap pointers to the stored names (pointing into `list`, valid
 * while the list is unchanged and not copied) into out. Returns how many were
 * written. This is the const char ** shape Vulkan and SDL_GPUVulkanOptions want. */
unsigned int vr_ext_list_ptrs(const VrExtList *list, const char **out, unsigned int out_cap);

typedef enum {
    VR_VK_BINDING_NONE = 0,
    /* XR_KHR_vulkan_enable: the application creates the VkInstance/VkDevice and
     * enables the extensions the runtime lists. This is the model SDL_GPU (which
     * owns device creation) fits. */
    VR_VK_BINDING_ENABLE_V1,
    /* XR_KHR_vulkan_enable2 only: its valid usage requires the runtime to create
     * the instance and device (xrCreateVulkanInstanceKHR/DeviceKHR). Used only as
     * a fallback, with a logged warning, when the runtime lacks enable v1. */
    VR_VK_BINDING_ENABLE2_FALLBACK
} VrVkBinding;

VrVkBinding vr_choose_vk_binding(bool runtime_has_vulkan_enable, bool runtime_has_vulkan_enable2);

#endif /* VR_EXT_LIST_H */
