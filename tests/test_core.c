/* Host tests for the parts that touch no device.
 *
 * The central one pins the refactor down: the wired path now decodes a GIP
 * report into a pad_state and translates that, where it used to translate the
 * raw report directly. The kernel report must come out byte for byte the same,
 * so the old translation is kept here verbatim as the reference.
 */
#include "gip/gip_proto.h"
#include "vpad.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...) do {                                   \
    if (!(cond)) {                                              \
        g_failures++;                                           \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);    \
        fprintf(stderr, __VA_ARGS__);                           \
        fprintf(stderr, "\n");                                  \
    }                                                           \
} while (0)

/* ---- the translation as it was before the refactor (vpad.c, resident) -- */

#define VP_BUTTONS 0x0C
#define VP_STICKS  0x10
#define VP_L2      0x14
#define VP_R2      0x15
#define VP_ACTIVE  0x17
#define VP_MOTION  0x78
#define TRIGGER_DIGITAL 128

static int16_t ref_le16(const unsigned char *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint8_t ref_axis(int16_t v)     { return (uint8_t)(((int)v + 32768) >> 8); }
static uint8_t ref_axis_inv(int16_t v) { return (uint8_t)((32767 - (int)v) >> 8); }

static void ref_translate(const unsigned char *gip_report, unsigned char *report)
{
    const unsigned char *d = gip_report + 4;
    unsigned m  = (unsigned)d[0] | ((unsigned)d[1] << 8);
    unsigned lt = (unsigned)d[2] | ((unsigned)d[3] << 8);
    unsigned rt = (unsigned)d[4] | ((unsigned)d[5] << 8);
    uint32_t b = 0;
    float ident = 1.0f;

    memset(report, 0, VPAD_REPORT_SIZE);

    if (m & 0x0010) b |= 0x004000;   /* A     -> cross    */
    if (m & 0x0020) b |= 0x002000;   /* B     -> circle   */
    if (m & 0x0040) b |= 0x008000;   /* X     -> square   */
    if (m & 0x0080) b |= 0x001000;   /* Y     -> triangle */
    if (m & 0x0100) b |= 0x000010;   /* up    */
    if (m & 0x0200) b |= 0x000040;   /* down  */
    if (m & 0x0400) b |= 0x000080;   /* left  */
    if (m & 0x0800) b |= 0x000020;   /* right */
    if (m & 0x1000) b |= 0x000400;   /* LB    -> L1 */
    if (m & 0x2000) b |= 0x000800;   /* RB    -> R1 */
    if (m & 0x4000) b |= 0x000002;   /* LS    -> L3 */
    if (m & 0x8000) b |= 0x000004;   /* RS    -> R3 */
    if (m & 0x0004) b |= 0x000008;   /* menu  -> options  */
    if (m & 0x0008) b |= 0x100000;   /* view  -> touchpad */

    if (lt > TRIGGER_DIGITAL) b |= 0x000100;
    if (rt > TRIGGER_DIGITAL) b |= 0x000200;

    memcpy(report + VP_BUTTONS, &b, sizeof b);

    report[VP_STICKS + 0] = ref_axis(ref_le16(d + 6));
    report[VP_STICKS + 1] = ref_axis_inv(ref_le16(d + 8));
    report[VP_STICKS + 2] = ref_axis(ref_le16(d + 10));
    report[VP_STICKS + 3] = ref_axis_inv(ref_le16(d + 12));

    report[VP_L2] = (uint8_t)(lt >> 2);
    report[VP_R2] = (uint8_t)(rt >> 2);
    report[VP_ACTIVE] = 1;

    memcpy(report + VP_MOTION + 12, &ident, sizeof ident);
}

/* ---- tests ------------------------------------------------------------- */

static void make_report(unsigned char *r, uint16_t buttons, uint16_t lt,
                        uint16_t rt, int16_t lx, int16_t ly, int16_t rx,
                        int16_t ry)
{
    uint16_t v[7];
    int i;

    v[0] = buttons; v[1] = lt; v[2] = rt;
    v[3] = (uint16_t)lx; v[4] = (uint16_t)ly;
    v[5] = (uint16_t)rx; v[6] = (uint16_t)ry;

    r[0] = GIP_LL_INPUT_REPORT;
    r[1] = 0x00;
    r[2] = 0x2A;
    r[3] = 14;
    for (i = 0; i < 7; i++) {
        r[4 + 2 * i] = (unsigned char)(v[i] & 0xFF);
        r[5 + 2 * i] = (unsigned char)(v[i] >> 8);
    }
}

static void check_same(const unsigned char *gip, const char *what)
{
    unsigned char want[VPAD_REPORT_SIZE], got[VPAD_REPORT_SIZE];
    pad_state st;

    ref_translate(gip, want);
    CHECK(gip_decode_input(gip, GIP_INPUT_MIN, &st), "%s: not decoded", what);
    vpad_translate(&st, got);
    CHECK(memcmp(want, got, sizeof want) == 0, "%s: report differs", what);
}

static void test_translation_unchanged(void)
{
    unsigned char r[GIP_INPUT_MIN];
    int i;

    /* Edges: everything released, everything pressed, the extremes of every
     * axis, and the triggers either side of the digital threshold. */
    make_report(r, 0, 0, 0, 0, 0, 0, 0);
    check_same(r, "idle");
    make_report(r, 0xFFFF, 1023, 1023, 32767, 32767, 32767, 32767);
    check_same(r, "all max");
    make_report(r, 0xFFFF, 0, 0, -32768, -32768, -32768, -32768);
    check_same(r, "all min");
    make_report(r, 0, 128, 129, 0, 0, 0, 0);
    check_same(r, "trigger threshold");

    srand(1);
    for (i = 0; i < 100000; i++) {
        make_report(r, (uint16_t)rand(), (uint16_t)(rand() % 1024),
                    (uint16_t)(rand() % 1024), (int16_t)rand(), (int16_t)rand(),
                    (int16_t)rand(), (int16_t)rand());
        check_same(r, "random");
        if (g_failures) break;
    }
}

static void test_decode_rejects(void)
{
    unsigned char r[GIP_INPUT_MIN];
    pad_state st;

    make_report(r, 0, 0, 0, 0, 0, 0, 0);
    CHECK(!gip_decode_input(r, GIP_INPUT_MIN - 1, &st), "short report accepted");
    r[0] = GIP_CMD_GUIDE_BUTTON;
    CHECK(!gip_decode_input(r, GIP_INPUT_MIN, &st), "non-input message accepted");
}

static void test_ack(void)
{
    /* A status message (cmd 03) with the ACK option, sequence 7, length 4. */
    const unsigned char msg[] = { 0x03, 0x30, 0x07, 0x04, 1, 2, 3, 4 };
    const unsigned char want[] = { 0x01, 0x20, 0x07, 0x09,
                                   0x00, 0x03, 0x30, 0x04, 0, 0, 0, 0, 0 };
    unsigned char got[32];
    int n;

    CHECK(gip_wants_ack(msg), "ACK option not seen");
    n = gip_build_ack(got, (int)sizeof got, msg);
    CHECK(n == (int)sizeof want && memcmp(got, want, sizeof want) == 0,
          "acknowledgement bytes differ (len %d)", n);
}

static void test_init_sequence(void)
{
    /* Power on, authenticate, light the guide LED -- as gip_init sent them. */
    unsigned char buf[16];
    int n;

    CHECK(gip_init_count == 3, "init sequence has %d messages", gip_init_count);
    n = gip_build(buf, (int)sizeof buf, gip_init_seq[0].cmd, 0,
                  gip_init_seq[0].payload, gip_init_seq[0].len);
    CHECK(n == 5 && buf[0] == 0x05 && buf[1] == 0x20 && buf[3] == 1 && buf[4] == 0,
          "power-on message wrong");
    n = gip_build(buf, (int)sizeof buf, gip_init_seq[2].cmd, 2,
                  gip_init_seq[2].payload, gip_init_seq[2].len);
    CHECK(n == 7 && buf[0] == 0x0A && buf[2] == 2 && buf[4] == 0x00 &&
          buf[5] == 0x01 && buf[6] == 0x14, "LED message wrong");
}

int main(void)
{
    test_translation_unchanged();
    test_decode_rejects();
    test_ack();
    test_init_sequence();

    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
