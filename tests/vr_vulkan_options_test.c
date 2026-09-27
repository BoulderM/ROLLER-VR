/* tests/vr_vulkan_options_test.c
 *
 * Checks the SDL fork's SDL_GPUVulkanOptions plumbing (SDL_PROP_GPU_DEVICE_CREATE_
 * VULKAN_OPTIONS_POINTER) against a real Vulkan driver, so it must run on a
 * machine with a Vulkan GPU and a display, for example `fatal-gpu` under Xorg :1:
 *
 *   DISPLAY=:1 SDL_VIDEODRIVER=x11 ./vr_vulkan_options_test
 *
 * Exit status: 0 = all checks passed, 1 = a check failed, 77 = skipped (no usable
 * Vulkan device, as on the dev LXC, which has no GPU). Built by `zig build -Dvr`
 * into <prefix>/bin. */
#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

#define VK_API_1_1 ((1u << 22) | (1u << 12))

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static SDL_GPUDevice *create_device(const SDL_GPUVulkanOptions *options)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, false);
    if (options)
        SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER, (void *)options);
    SDL_GPUDevice *device = SDL_CreateGPUDeviceWithProperties(props);
    SDL_DestroyProperties(props);
    return device;
}

int main(void)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        printf("SKIP: SDL_Init(VIDEO) failed: %s\n", SDL_GetError());
        return 77;
    }

    /* Baseline: without options a device must come up, otherwise nothing below means anything. */
    SDL_GPUDevice *baseline = create_device(NULL);
    if (!baseline) {
        printf("SKIP: no usable Vulkan GPU device: %s\n", SDL_GetError());
        SDL_Quit();
        return 77;
    }
    printf("baseline device driver: %s\n", SDL_GetGPUDeviceDriver(baseline));
    SDL_DestroyGPUDevice(baseline);

    /* Real options: Vulkan 1.1 plus the external-memory extensions OpenXR runtimes commonly list. */
    static const char *instance_names[] = { "VK_KHR_external_memory_capabilities" };
    static const char *device_names[] = { "VK_KHR_external_memory" };
    SDL_GPUVulkanOptions good;
    SDL_zero(good);
    good.vulkan_api_version = VK_API_1_1;
    good.instance_extension_count = 1;
    good.instance_extension_names = instance_names;
    good.device_extension_count = 1;
    good.device_extension_names = device_names;
    SDL_GPUDevice *device = create_device(&good);
    CHECK(device != NULL);
    if (device) {
        SDL_PropertiesID props = SDL_GetGPUDeviceProperties(device);
        CHECK(SDL_GetPointerProperty(props, SDL_PROP_GPU_DEVICE_VULKAN_INSTANCE_POINTER, NULL) != NULL);
        CHECK(SDL_GetPointerProperty(props, SDL_PROP_GPU_DEVICE_VULKAN_DEVICE_POINTER, NULL) != NULL);
        SDL_DestroyGPUDevice(device);
    } else {
        printf("  error: %s\n", SDL_GetError());
    }

    /* An unsupported instance extension must fail device creation and name the extension. */
    static const char *bogus_instance[] = { "VK_FAKE_not_a_real_instance_extension" };
    SDL_GPUVulkanOptions bad_instance;
    SDL_zero(bad_instance);
    bad_instance.instance_extension_count = 1;
    bad_instance.instance_extension_names = bogus_instance;
    device = create_device(&bad_instance);
    CHECK(device == NULL);
    CHECK(strstr(SDL_GetError(), "VK_FAKE_not_a_real_instance_extension") != NULL);
    if (device)
        SDL_DestroyGPUDevice(device);

    /* Same for an unsupported device extension. */
    static const char *bogus_device[] = { "VK_FAKE_not_a_real_device_extension" };
    SDL_GPUVulkanOptions bad_device;
    SDL_zero(bad_device);
    bad_device.device_extension_count = 1;
    bad_device.device_extension_names = bogus_device;
    device = create_device(&bad_device);
    CHECK(device == NULL);
    CHECK(strstr(SDL_GetError(), "VK_FAKE_not_a_real_device_extension") != NULL);
    if (device)
        SDL_DestroyGPUDevice(device);

    /* Not checked here: that vulkan_api_version reaches vkCreateInstance. A Vulkan 1.1+
     * implementation must never reject a too-high apiVersion, so device creation cannot show
     * it. It is verified end to end instead: with API 1.1 requested, the Vulkan validation
     * layer no longer reports VUID-VkImageMemoryBarrier-None-09120 (see the Monado run). */

    /* Options with nothing requested behave like no options. */
    SDL_GPUVulkanOptions empty;
    SDL_zero(empty);
    device = create_device(&empty);
    CHECK(device != NULL);
    if (device)
        SDL_DestroyGPUDevice(device);

    /* A later SUCCESSFUL creation must not leave a stale error from an earlier,
     * unrelated failure lying around: vr_diag_fatal() reports whatever
     * SDL_GetError() holds, and the failed device-extension request just above
     * (bad_device) left "VK_FAKE_not_a_real_device_extension" in it -- confirmed
     * still present after the successful "empty" creation right above this. */
    device = create_device(NULL);
    CHECK(device != NULL);
    CHECK(strstr(SDL_GetError(), "VK_FAKE_not_a_real_device_extension") == NULL);
    if (device)
        SDL_DestroyGPUDevice(device);

    SDL_Quit();
    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("vr_vulkan_options_test: ALL PASS\n");
    return 0;
}
