/* tests/vr_ini_merge_test.c -- unit tests for PROJECTS/ROLLER/vr_ini_merge.c (pure C, no SDL). */
#include "vr_ini_merge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static char g_ini[512], g_bak[512], g_tmp[512];

static void write_file(const char *path, const char *s)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) { printf("FAIL cannot write %s\n", path); g_failures++; return; }
    fwrite(s, 1, strlen(s), fp);
    fclose(fp);
}

static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    char *buf = (char *)calloc(1, 65536);
    size_t n = fread(buf, 1, 65535, fp);
    buf[n] = '\0';
    fclose(fp);
    return buf;
}

static int file_exists(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (fp) fclose(fp);
    return fp != NULL;
}

static void cleanup(void)
{
    remove(g_ini);
    remove(g_bak);
    remove(g_tmp);
}

/* The P1 driving lines, in InputSaveConfig()'s format, bound to the VR pad. */
static const char *const k_keys[6] = { "P1Left", "P1Right", "P1Up", "P1Down", "P1UpGear", "P1DownGear" };
static const char *const k_lines[6] = {
    "P1Left=axis;guid=0300aaaa;name=ROLLER VR Controllers;vendor=0;product=0;version=0;ordinal=-1;index=0;dir=-1;mode=centered;deadzone=8000;threshold=16000;rest=0;invert=0;standard=1;scancode=44",
    "P1Right=axis;guid=0300aaaa;name=ROLLER VR Controllers;vendor=0;product=0;version=0;ordinal=-1;index=0;dir=1;mode=centered;deadzone=8000;threshold=16000;rest=0;invert=0;standard=1;scancode=45",
    "P1Up=axis;guid=0300aaaa;name=ROLLER VR Controllers;vendor=0;product=0;version=0;ordinal=-1;index=5;dir=1;mode=pedal;deadzone=2000;threshold=16000;rest=0;invert=0;standard=1;scancode=20",
    "P1Down=axis;guid=0300aaaa;name=ROLLER VR Controllers;vendor=0;product=0;version=0;ordinal=-1;index=4;dir=1;mode=pedal;deadzone=2000;threshold=16000;rest=0;invert=0;standard=1;scancode=33",
    "P1UpGear=button;guid=0300aaaa;name=ROLLER VR Controllers;vendor=0;product=0;version=0;ordinal=-1;index=10;standard=1;scancode=19",
    "P1DownGear=button;guid=0300aaaa;name=ROLLER VR Controllers;vendor=0;product=0;version=0;ordinal=-1;index=9;standard=1;scancode=32",
};

/* A keyboard-only user file with a comment, an unknown key and P2 on a wheel. */
static const char *k_user_ini =
    "InputVersion=2\n"
    "[Settings]\n"
    "MusicSource=MIDI\n"
    "; my own comment\n"
    "SomeFutureKey=keep me\n"
    "[CommunityTracks]\n"
    "SelectedTrackType=Stock\n"
    "[Input]\n"
    "P1Left=keyboard;scancode=44\n"
    "P1Right=keyboard;scancode=45\n"
    "P1Up=keyboard;scancode=20\n"
    "P1Down=keyboard;scancode=33\n"
    "P1UpGear=keyboard;scancode=19\n"
    "P1DownGear=keyboard;scancode=32\n"
    "P1Cheat=keyboard;scancode=46\n"
    "P2Left=axis;guid=03000000;name=Wheel;vendor=1133;product=49760;version=0;ordinal=0;index=0;dir=-1;mode=centered;deadzone=8000;threshold=16000;rest=0;invert=0;standard=0;scancode=75\n"
    "P2Right=keyboard;scancode=77\n";

/* Every line of `before` that is not a P1 driving line must appear unchanged, in
 * order, in `after`; and every P1 driving line in `after` must be the pad line. */
static void check_only_p1_lines_changed(const char *before, const char *after)
{
    const char *b = before, *a = after;
    while (*b) {
        const char *bnl = strchr(b, '\n');
        size_t blen = bnl ? (size_t)(bnl - b) + 1 : strlen(b);
        int is_p1 = 0;
        for (int i = 0; i < 6; ++i) {
            size_t kl = strlen(k_keys[i]);
            if (strncmp(b, k_keys[i], kl) == 0 && b[kl] == '=') {
                is_p1 = 1;
                CHECK(strncmp(a, k_lines[i], strlen(k_lines[i])) == 0);
            }
        }
        if (!is_p1)
            CHECK(strncmp(a, b, blen) == 0);
        const char *anl = strchr(a, '\n');
        a = anl ? anl + 1 : a + strlen(a);
        b += blen;
    }
    CHECK(*a == '\0');
}

static void test_replace_keyboard_lines(void)
{
    cleanup();
    write_file(g_ini, k_user_ini);
    CHECK(vr_ini_merge_keys(g_ini, g_bak, k_keys, k_lines, 6) == VR_INI_MERGE_WRITTEN);
    char *after = read_file(g_ini);
    char *bak = read_file(g_bak);
    CHECK(after && bak);
    if (after && bak) {
        CHECK(strcmp(bak, k_user_ini) == 0);
        check_only_p1_lines_changed(k_user_ini, after);
        CHECK(strstr(after, "P1Cheat=keyboard;scancode=46\n") != NULL);
        CHECK(strstr(after, "SomeFutureKey=keep me\n") != NULL);
        CHECK(strstr(after, "; my own comment\n") != NULL);
    }
    CHECK(!file_exists(g_tmp));

    /* Same lines again: nothing to do, and the backup is not touched. */
    write_file(g_bak, "sentinel");
    CHECK(vr_ini_merge_keys(g_ini, g_bak, k_keys, k_lines, 6) == VR_INI_MERGE_UNCHANGED);
    char *bak2 = read_file(g_bak);
    CHECK(bak2 && strcmp(bak2, "sentinel") == 0);

    /* A later change keeps the existing backup: it is never replaced. */
    const char *k1[1] = { "P1Left" };
    const char *l1[1] = { "P1Left=keyboard;scancode=44" };
    CHECK(vr_ini_merge_keys(g_ini, g_bak, k1, l1, 1) == VR_INI_MERGE_WRITTEN);
    char *bak3 = read_file(g_bak);
    CHECK(bak3 && strcmp(bak3, "sentinel") == 0);
    free(after); free(bak); free(bak2); free(bak3);
}

static void test_insert_missing_into_input_section(void)
{
    cleanup();
    const char *ini =
        "[Input]\r\n"
        "P1Cheat=keyboard;scancode=46\r\n"
        "[Other]\r\n"
        "X=1\r\n";
    write_file(g_ini, ini);
    const char *keys[2] = { "P1Left", "P1Right" };
    const char *lines[2] = { "P1Left=A", "P1Right=B" };
    CHECK(vr_ini_merge_keys(g_ini, g_bak, keys, lines, 2) == VR_INI_MERGE_WRITTEN);
    char *after = read_file(g_ini);
    CHECK(after && strcmp(after,
        "[Input]\r\n"
        "P1Cheat=keyboard;scancode=46\r\n"
        "P1Left=A\r\n"
        "P1Right=B\r\n"
        "[Other]\r\n"
        "X=1\r\n") == 0);
    free(after);
}

static void test_append_section_and_case_and_duplicates(void)
{
    cleanup();
    /* No [Input], no trailing newline; a key in other case and twice. */
    const char *ini = "[Settings]\n  p1left = old\nMusicSource=MIDI\nP1LEFT=older";
    write_file(g_ini, ini);
    const char *keys[2] = { "P1Left", "P1Up" };
    const char *lines[2] = { "P1Left=A", "P1Up=C" };
    CHECK(vr_ini_merge_keys(g_ini, g_bak, keys, lines, 2) == VR_INI_MERGE_WRITTEN);
    char *after = read_file(g_ini);
    CHECK(after && strcmp(after, "[Settings]\nP1Left=A\nMusicSource=MIDI\nP1Left=A\n[Input]\nP1Up=C\n") == 0);
    free(after);
    /* Comments and sections that look like keys are never matched. */
    cleanup();
    write_file(g_ini, "; P1Left=x\n#P1Left=y\n[P1Left=z]\n");
    CHECK(vr_ini_merge_keys(g_ini, g_bak, keys, lines, 1) == VR_INI_MERGE_WRITTEN);
    after = read_file(g_ini);
    CHECK(after && strcmp(after, "; P1Left=x\n#P1Left=y\n[P1Left=z]\n[Input]\nP1Left=A\n") == 0);
    free(after);
}

static void test_missing_file(void)
{
    cleanup();
    CHECK(vr_ini_merge_keys(g_ini, g_bak, k_keys, k_lines, 6) == VR_INI_MERGE_MISSING);
    CHECK(!file_exists(g_ini));
    CHECK(!file_exists(g_bak));
}

int main(void)
{
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir)
        dir = "/tmp";
    snprintf(g_ini, sizeof(g_ini), "%s/vr_ini_merge_test_%d.INI", dir, (int)getpid());
    snprintf(g_bak, sizeof(g_bak), "%s.bak", g_ini);
    snprintf(g_tmp, sizeof(g_tmp), "%s.tmp", g_ini);

    test_replace_keyboard_lines();
    test_insert_missing_into_input_section();
    test_append_section_and_case_and_duplicates();
    test_missing_file();
    cleanup();

    if (g_failures) {
        printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("vr_ini_merge_test: ALL PASS\n");
    return 0;
}
