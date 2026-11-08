// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the WN8030 keyboard controller codec (package/Wn8030.c). With a firmware path argument, the image's header and data
// commands are also printed as hex lines for scripts/check-wn8030-host.py to compare with its own encoding.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../Wn8030.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

static void hex(const char *tag, const uint8_t *p, uint32_t n)
{
    printf("%s ", tag);
    for (uint32_t i = 0; i < n; ++i) printf("%02x", p[i]);
    printf("\n");
}

static uint8_t *load(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc((size_t)n);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {fclose(f); free(p); return NULL;}
    fclose(f);
    *size = (uint32_t)n;
    return p;
}

int main(int argc, char **argv)
{
    uint8_t out[1 + WN8030_DATA_COMMAND_BYTES];

    /* Sums. */
    const uint8_t s[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK(wn8030_sum8(s, 9) == 45 && wn8030_sum32(s, 8) == 0x04030201u + 0x08070605u && wn8030_sum32(s, 9) == 0x04030201u + 0x08070605u + 9u);

    /* Control commands: register byte, the 66-byte frame, the checksum over bytes 2 .. 7 + length of the frame. */
    const uint8_t on = 1;
    CHECK(wn8030_control(out, WN8030_CMD_TOUCHPAD, &on, 1) == 67);
    const uint8_t touchpad[] = {0x5c, 0x32, 0x00, 0x4e, 0x31, 0x80, 0x38, 0x21, 0x01, 0x01, 0x5a};
    CHECK(memcmp(out, touchpad, sizeof touchpad) == 0);
    for (unsigned i = sizeof touchpad; i < 67; ++i) CHECK(out[i] == 0);
    const uint8_t caps = WN8030_LED_CAPS_ON;
    CHECK(wn8030_control(out, WN8030_CMD_INDICATOR, &caps, 1) == 67 && out[7] == 0x2e && out[9] == 0xfd &&
          out[10] == (uint8_t)(0x4e + 0x31 + 0x80 + 0x38 + 0x2e + 0x01 + 0xfd));
    uint8_t long_payload[58] = {0};
    CHECK(wn8030_control(out, 0x40, long_payload, 57) == 67 && wn8030_control(out, 0x40, long_payload, 58) == 0);
    CHECK(wn8030_control(out, 0x40, NULL, 1) == 0 && wn8030_control(out, WN8030_CMD_KEYBOARD_POWER, &on, 1) == 67 && out[7] == 0x25);

    /* A synthetic image: magic, a code size, the byte sum of the code. */
    static uint8_t image[0xa400];
    memset(image, 0, sizeof image);
    memcpy(image + WN8030_IMAGE_HEADER, "NANO IC", 7);
    const uint32_t code = 0x2000 + 17;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < code; ++i) {image[WN8030_IMAGE_CODE + i] = (uint8_t)(i * 13 + 1); sum += image[WN8030_IMAGE_CODE + i];}
    uint8_t *h = image + WN8030_IMAGE_HEADER;
    h[48] = (uint8_t)code; h[49] = (uint8_t)(code >> 8);
    h[52] = (uint8_t)sum; h[53] = (uint8_t)(sum >> 8); h[54] = (uint8_t)(sum >> 16); h[55] = (uint8_t)(sum >> 24);
    h[21] = 0x44; h[24] = 0x1f; h[25] = 0x4c; h[26] = 0x12; h[27] = 0x06; h[28] = 0x32; h[29] = 37; h[30] = 1;
    uint32_t size = 0;
    CHECK(wn8030_image_check(image, sizeof image, &size) == 0 && size == code);
    CHECK(wn8030_image_check(image, WN8030_IMAGE_MIN - 1, NULL) == -1);
    image[WN8030_IMAGE_HEADER] = 'X'; CHECK(wn8030_image_check(image, sizeof image, NULL) == -2); image[WN8030_IMAGE_HEADER] = 'N';
    h[50] = 1; CHECK(wn8030_image_check(image, sizeof image, NULL) == -3); h[50] = 0;
    image[WN8030_IMAGE_CODE] ^= 1; CHECK(wn8030_image_check(image, sizeof image, NULL) == -4); image[WN8030_IMAGE_CODE] ^= 1;

    CHECK(wn8030_header_command(out, image, code) == 66 && out[0] == 0x5c && out[1] == 0x06 && memcmp(out + 2, "NANO IC", 7) == 0);
    CHECK(out[9] == 0x40 && out[10] == 0x81 && out[11] == 0x45 && out[12] == 0x23 && out[15] == 0x40 && out[19] == (uint8_t)code);
    CHECK(out[23] == 0x44 && out[26] == 0x1f && out[27] == 0x4c && out[29] == 0x06 && out[31] == 37 && out[32] == 1);
    CHECK(out[65] == wn8030_sum8(out + 2, 63));
    const uint32_t sum32 = wn8030_sum32(image + WN8030_IMAGE_CODE, code);
    CHECK(out[54] == (uint8_t)sum32 && out[57] == (uint8_t)(sum32 >> 24));

    /* Data commands: 503 bytes per chunk, offset and size big-endian, the last chunk short, nothing past the end. */
    uint32_t offset = 0, next = 0, chunks = 0, sent = 0;
    while (wn8030_data_command(out, image, code, offset, &next) == 513) {
        const uint32_t n = (uint32_t)out[6] << 24 | (uint32_t)out[7] << 16 | (uint32_t)out[8] << 8 | out[9];
        CHECK(out[0] == 0x5c && out[1] == 0x07 && ((uint32_t)out[2] << 24 | (uint32_t)out[3] << 16 | (uint32_t)out[4] << 8 | out[5]) == offset);
        CHECK(n == (code - offset < 503 ? code - offset : 503) && memcmp(out + 10, image + WN8030_IMAGE_CODE + offset, n) == 0);
        for (uint32_t i = 10 + n; i < 513; ++i) CHECK(out[i] == 0);
        sent += n; offset = next; ++chunks;
    }
    CHECK(sent == code && chunks == (code + 502) / 503 && wn8030_data_command(out, image, code, code, &next) == 0);

    /* Frames. */
    uint8_t frame[WN8030_FRAME_BYTES] = {0};
    Wn8030Frame f;
    CHECK(wn8030_parse(frame, &f) == Wn8030Invalid);
    frame[0] = 0x57; CHECK(wn8030_parse(frame, &f) == Wn8030Invalid);   /* empty */
    frame[2] = 0x0a; frame[3] = 0x05; frame[5] = 0x04;
    CHECK(wn8030_parse(frame, &f) == Wn8030Keyboard && f.report == frame + 3 && f.length == 9 && f.report[2] == 0x04);
    frame[3] = 0x06; CHECK(wn8030_parse(frame, &f) == Wn8030Consumer && f.length == 3);
    frame[3] = 0x19; CHECK(wn8030_parse(frame, &f) == Wn8030Touchpad && f.length == 21);
    frame[3] = 0x33; CHECK(wn8030_parse(frame, &f) == Wn8030Other && !f.report);
    /* Attach state. */
    memset(frame, 0, sizeof frame);
    frame[0] = 0x57; frame[2] = 0x10; frame[3] = 0x24; frame[5] = 0x38; frame[6] = 0x80; frame[7] = 0xa2;
    frame[12] = 0x03; CHECK(wn8030_parse(frame, &f) == Wn8030Attach && f.attached == 1 && !f.report);
    frame[12] = 0x00; CHECK(wn8030_parse(frame, &f) == Wn8030Attach && f.attached == 0);
    frame[12] = 0x01; CHECK(wn8030_parse(frame, &f) == Wn8030Attach && f.attached == -1);
    frame[12] = 0xf3; CHECK(wn8030_parse(frame, &f) == Wn8030Attach && f.attached == 1);
    frame[6] = 0x81; CHECK(wn8030_parse(frame, &f) == Wn8030Other); frame[6] = 0x80;
    frame[7] = 0x24; CHECK(wn8030_parse(frame, &f) == Wn8030AuthRequest);
    frame[7] = 0x31; CHECK(wn8030_parse(frame, &f) == Wn8030AuthInit);
    frame[7] = 0x32; CHECK(wn8030_parse(frame, &f) == Wn8030AuthChallenge);
    /* Accelerometer: x = +256 (1 g), y = -1, z = -2048 in the cover's axes; the tablet's y and z are the negated ones. */
    frame[7] = 0x64; frame[8] = 0x06;
    frame[9] = 0x00; frame[10] = 0x10;      /* 0x100 */
    frame[11] = 0xf0; frame[12] = 0xff;     /* 0xfff = -1 */
    frame[13] = 0x00; frame[14] = 0x80;     /* 0x800 = -2048 */
    CHECK(wn8030_parse(frame, &f) == Wn8030Accel && f.x == 256 && f.y == 1 && f.z == 2048);
    frame[8] = 0x05; CHECK(wn8030_parse(frame, &f) == Wn8030Other);

    uint8_t kb[9], cs[3], tp[21];
    memset(kb, 0xff, sizeof kb); memset(cs, 0xff, sizeof cs); memset(tp, 0xff, sizeof tp);
    wn8030_release_reports(kb, cs, tp);
    CHECK(kb[0] == 5 && cs[0] == 6 && tp[0] == 0x19);
    for (unsigned i = 1; i < 9; ++i) CHECK(!kb[i]);
    for (unsigned i = 1; i < 21; ++i) CHECK(!tp[i]);
    CHECK(!cs[1] && !cs[2]);

    /* Touchpad reports: Y from the other edge, X, ids, buttons and count unchanged; a touching palm goes out lifted. */
    {
        uint8_t in[21] = {0x19, 0x01}, out[21];
        const struct {uint8_t flags, id; uint16_t x, y;} fingers[3] = {{0x07, 3, 100, 0}, {0x03, 4, 3047, 2031}, {0x00, 0, 5, 3000}};
        for (unsigned f = 0; f < 3; ++f) {
            uint8_t *p = in + 2 + 6 * f;
            p[0] = fingers[f].flags; p[1] = fingers[f].id;
            p[2] = (uint8_t)fingers[f].x; p[3] = (uint8_t)(fingers[f].x >> 8);
            p[4] = (uint8_t)fingers[f].y; p[5] = (uint8_t)(fingers[f].y >> 8);
        }
        in[20] = 2;
        CHECK(wn8030_touchpad_report(in, out) == 1);
        CHECK(out[0] == 0x19 && out[1] == 0x01 && out[20] == 2);
        CHECK(out[2] == 0x07 && out[3] == 3 && (out[4] | out[5] << 8) == 100 && (out[6] | out[7] << 8) == 2031);
        CHECK(out[8] == 0x00 && out[9] == 4 && (out[10] | out[11] << 8) == 3047 && (out[12] | out[13] << 8) == 0);   /* palm: lifted */
        CHECK(out[14] == 0x00 && (out[18] | out[19] << 8) == 0);   /* an idle slot past the range clamps to the edge */
        in[8] = 0x06;   /* in range, confident, not touching: unchanged */
        CHECK(wn8030_touchpad_report(in, out) == 0 && out[8] == 0x06);
        for (unsigned y = 0; y <= 2031; ++y) {
            in[6] = (uint8_t)y; in[7] = (uint8_t)(y >> 8);
            wn8030_touchpad_report(in, out);
            CHECK((unsigned)(out[6] | out[7] << 8) == 2031 - y);
        }
    }

    /* The pinned image, when given. */
    if (argc > 1) {
        uint32_t bytes = 0;
        uint8_t *fw = load(argv[1], &bytes);
        CHECK(fw != NULL);
        uint32_t fw_code = 0;
        CHECK(wn8030_image_check(fw, bytes, &fw_code) == 0);
        CHECK(wn8030_header_command(out, fw, fw_code) == 66);
        hex("HEADER", out, 66);
        uint32_t at = 0, after = 0, count = 0;
        while (wn8030_data_command(out, fw, fw_code, at, &after) == 513) {
            if (count == 0 || after == fw_code) hex(count == 0 ? "FIRST" : "LAST", out, 513);
            at = after; ++count;
        }
        printf("CHUNKS %u CODE %u\n", count, fw_code);
        free(fw);
    }
    printf("PASS WN8030: %u checks\n", checks);
    return 0;
}
