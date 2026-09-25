/* tests/vr_log_test.c -- unit tests for PROJECTS/ROLLER/vr_log.c (pure C, no SDL). */
#include "vr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static void test_format_basic(void)
{
    char buf[128];
    size_t n = vr_log_format_line(buf, sizeof(buf), 12345, "INFO", "hello");
    CHECK(strcmp(buf, "[12.345s] INFO hello\n") == 0);
    CHECK(n == strlen(buf));

    vr_log_format_line(buf, sizeof(buf), 7, "WARN", "x");
    CHECK(strcmp(buf, "[0.007s] WARN x\n") == 0);

    vr_log_format_line(buf, sizeof(buf), -5, NULL, NULL);
    CHECK(strcmp(buf, "[0.000s] INFO \n") == 0);
}

static void test_format_truncates_but_keeps_newline(void)
{
    char buf[16];
    size_t n = vr_log_format_line(buf, sizeof(buf), 1, "ERROR", "a very long message that cannot fit");
    CHECK(n == strlen(buf));
    CHECK(n == sizeof(buf) - 1);
    CHECK(buf[n - 1] == '\n');

    char tiny[2];
    n = vr_log_format_line(tiny, sizeof(tiny), 1, "INFO", "x");
    CHECK(tiny[0] == '\n' && tiny[1] == '\0' && n == 1);
    CHECK(vr_log_format_line(tiny, 1, 1, "INFO", "x") == 0);
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *data = calloc(1, 4096);
    size_t got = fread(data, 1, 4095, f);
    data[got] = '\0';
    fclose(f);
    return data;
}

static void test_open_captures_stdout_stderr_and_restores(void)
{
    const char *path = "vr_log_test.tmp.log";
    remove(path);

    CHECK(!vr_log_is_open());
    CHECK(vr_log_open(path));
    CHECK(vr_log_is_open());
    CHECK(vr_log_open(path)); /* second open is a no-op */

    vr_log_line("WARN", "x=%d", 7);
    fprintf(stderr, "raw stderr line\n");
    printf("raw stdout line\n");
    vr_log_close();
    CHECK(!vr_log_is_open());
    vr_log_close(); /* closing twice is safe */

    char *text = slurp(path);
    CHECK(text != NULL);
    if (text) {
        CHECK(strstr(text, "WARN x=7\n") != NULL);
        CHECK(strstr(text, "raw stderr line\n") != NULL);
        CHECK(strstr(text, "raw stdout line\n") != NULL);
        free(text);
    }
    remove(path);
}

static void test_open_failure_leaves_streams_alone(void)
{
    CHECK(!vr_log_open("/nonexistent-dir-for-vr-log-test/x.log"));
    CHECK(!vr_log_is_open());
    CHECK(!vr_log_open(NULL));
    printf("stdout still works after a failed open\n");
}

int main(void)
{
    test_format_basic();
    test_format_truncates_but_keeps_newline();
    test_open_captures_stdout_stderr_and_restores();
    test_open_failure_leaves_streams_alone();

    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("vr_log_test: ALL PASS\n");
    return 0;
}
