/* tests/vr_ext_list_test.c -- unit tests for PROJECTS/ROLLER/vr_ext_list.c (pure C). */
#include "vr_ext_list.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static void test_add_and_dedupe(void)
{
    VrExtList l;
    vr_ext_list_init(&l);
    CHECK(l.count == 0);
    CHECK(vr_ext_list_add(&l, "VK_KHR_external_memory"));
    CHECK(vr_ext_list_add(&l, "VK_KHR_external_memory")); /* duplicate: no-op, still true */
    CHECK(l.count == 1);
    CHECK(vr_ext_list_contains(&l, "VK_KHR_external_memory"));
    CHECK(!vr_ext_list_contains(&l, "VK_KHR_external_semaphore"));
    CHECK(!vr_ext_list_contains(&l, NULL));
}

static void test_add_rejects_bad_names(void)
{
    VrExtList l;
    vr_ext_list_init(&l);
    CHECK(!vr_ext_list_add(&l, NULL));
    CHECK(!vr_ext_list_add(&l, ""));

    char too_long[VR_EXT_NAME_MAX + 1];
    memset(too_long, 'x', VR_EXT_NAME_MAX);
    too_long[VR_EXT_NAME_MAX] = '\0';
    CHECK(!vr_ext_list_add(&l, too_long));

    char longest_ok[VR_EXT_NAME_MAX];
    memset(longest_ok, 'y', VR_EXT_NAME_MAX - 1);
    longest_ok[VR_EXT_NAME_MAX - 1] = '\0';
    CHECK(vr_ext_list_add(&l, longest_ok));
    CHECK(l.count == 1);
}

static void test_add_full_list(void)
{
    VrExtList l;
    vr_ext_list_init(&l);
    char name[32];
    for (int i = 0; i < VR_EXT_LIST_MAX; i++) {
        snprintf(name, sizeof(name), "VK_TEST_ext_%d", i);
        CHECK(vr_ext_list_add(&l, name));
    }
    CHECK(l.count == VR_EXT_LIST_MAX);
    CHECK(!vr_ext_list_add(&l, "VK_TEST_one_too_many"));
    CHECK(l.count == VR_EXT_LIST_MAX);
    CHECK(vr_ext_list_add(&l, "VK_TEST_ext_3")); /* already present: still fine when full */
}

static void test_spaced_parsing(void)
{
    VrExtList l;
    vr_ext_list_init(&l);
    CHECK(vr_ext_list_add_spaced(&l, "VK_KHR_a VK_KHR_b VK_KHR_a"));
    CHECK(l.count == 2);
    CHECK(strcmp(l.names[0], "VK_KHR_a") == 0);
    CHECK(strcmp(l.names[1], "VK_KHR_b") == 0);

    CHECK(vr_ext_list_add_spaced(&l, "   VK_KHR_c    VK_KHR_d  "));
    CHECK(l.count == 4);
    CHECK(vr_ext_list_add_spaced(&l, ""));
    CHECK(vr_ext_list_add_spaced(&l, "    "));
    CHECK(vr_ext_list_add_spaced(&l, NULL));
    CHECK(l.count == 4);
    CHECK(!vr_ext_list_add_spaced(NULL, "VK_KHR_a"));
}

static void test_spaced_reports_failure_but_keeps_earlier_names(void)
{
    VrExtList l;
    vr_ext_list_init(&l);
    char huge[VR_EXT_NAME_MAX + 8];
    memset(huge, 'z', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';

    char input[VR_EXT_NAME_MAX + 64];
    snprintf(input, sizeof(input), "VK_KHR_ok %s VK_KHR_after", huge);
    CHECK(!vr_ext_list_add_spaced(&l, input));
    CHECK(vr_ext_list_contains(&l, "VK_KHR_ok"));
    CHECK(vr_ext_list_contains(&l, "VK_KHR_after")); /* parsing continues past the bad token */
    CHECK(l.count == 2);
}

static void test_ptrs(void)
{
    VrExtList l;
    vr_ext_list_init(&l);
    vr_ext_list_add_spaced(&l, "VK_KHR_a VK_KHR_b VK_KHR_c");

    const char *out[8];
    unsigned int n = vr_ext_list_ptrs(&l, out, 8);
    CHECK(n == 3);
    CHECK(strcmp(out[0], "VK_KHR_a") == 0 && strcmp(out[2], "VK_KHR_c") == 0);

    n = vr_ext_list_ptrs(&l, out, 2); /* capacity smaller than the list */
    CHECK(n == 2);
    CHECK(vr_ext_list_ptrs(NULL, out, 8) == 0);
    CHECK(vr_ext_list_ptrs(&l, NULL, 8) == 0);
}

static void test_binding_choice(void)
{
    CHECK(vr_choose_vk_binding(true, true) == VR_VK_BINDING_ENABLE_V1);
    CHECK(vr_choose_vk_binding(true, false) == VR_VK_BINDING_ENABLE_V1);
    CHECK(vr_choose_vk_binding(false, true) == VR_VK_BINDING_ENABLE2_FALLBACK);
    CHECK(vr_choose_vk_binding(false, false) == VR_VK_BINDING_NONE);
}

int main(void)
{
    test_add_and_dedupe();
    test_add_rejects_bad_names();
    test_add_full_list();
    test_spaced_parsing();
    test_spaced_reports_failure_but_keeps_earlier_names();
    test_ptrs();
    test_binding_choice();

    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("vr_ext_list_test: ALL PASS\n");
    return 0;
}
