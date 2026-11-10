/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Wn8030.h"

static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static void put_le16(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static void put_le32(uint8_t *p, uint32_t v) {put_le16(p, v); put_le16(p + 2, v >> 16);}
static void put_be32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;}
static void zero(uint8_t *p, uint32_t n) {for (uint32_t i = 0; i < n; ++i) p[i] = 0;}
/* A 12-bit two's complement value. */
static int16_t sext12(uint32_t v) {v &= 0xfffu; return (int16_t)(v & 0x800u ? (int32_t)v - 0x1000 : (int32_t)v);}

uint8_t wn8030_sum8(const uint8_t *bytes, uint32_t length)
{
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; ++i) sum = (uint8_t)(sum + bytes[i]);
    return sum;
}

uint32_t wn8030_sum32(const uint8_t *bytes, uint32_t length)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < length; i += 4) {
        uint32_t word = 0;
        for (uint32_t k = 0; k < 4 && i + k < length; ++k) word |= (uint32_t)bytes[i + k] << (8 * k);
        sum += word;
    }
    return sum;
}

uint32_t wn8030_control(uint8_t out[1 + WN8030_COMMAND_BYTES], uint8_t command, const uint8_t *payload, uint8_t length)
{
    if (length > WN8030_COMMAND_BYTES - 9u || (length && !payload)) return 0;
    uint8_t *f = out + 1;
    out[0] = WN8030_REG_WRITE;
    zero(f, WN8030_COMMAND_BYTES);
    f[0] = 0x32; f[2] = 0x4e; f[3] = 0x31; f[4] = 0x80; f[5] = 0x38;
    f[6] = command; f[7] = length;
    for (uint32_t i = 0; i < length; ++i) f[8 + i] = payload[i];
    f[8 + length] = wn8030_sum8(f + 2, 6u + length);
    return 1 + WN8030_COMMAND_BYTES;
}

int wn8030_image_check(const uint8_t *image, uint32_t size, uint32_t *code_size)
{
    static const char magic[7] = {'N', 'A', 'N', 'O', ' ', 'I', 'C'};
    if (!image || size < WN8030_IMAGE_MIN) return -1;
    const uint8_t *h = image + WN8030_IMAGE_HEADER;
    for (unsigned i = 0; i < sizeof magic; ++i) if (h[i] != (uint8_t)magic[i]) return -2;
    const uint32_t code = le32(h + 48);
    if (!code || code > size - WN8030_IMAGE_CODE) return -3;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < code; ++i) sum += image[WN8030_IMAGE_CODE + i];
    if (sum != le32(h + 52)) return -4;
    if (code_size) *code_size = code;
    return 0;
}

uint32_t wn8030_header_command(uint8_t out[1 + WN8030_HEADER_COMMAND_BYTES], const uint8_t *image, uint32_t code_size)
{
    const uint8_t *h = image + WN8030_IMAGE_HEADER;
    uint8_t *c = out + 1;
    out[0] = WN8030_REG_WRITE;
    zero(c, WN8030_HEADER_COMMAND_BYTES);
    c[0] = 0x06;
    for (unsigned i = 0; i < 7; ++i) c[1 + i] = h[i];
    put_le16(c + 8, 0x8140);                  /* the chip id the boot ROM expects */
    put_le32(c + 10, 0x2345);                 /* code source */
    put_le32(c + 14, 0x40);                   /* code destination */
    put_le32(c + 18, code_size);
    for (unsigned i = 0; i < 4; ++i) c[22 + i] = h[21 + i];   /* entry point */
    for (unsigned i = 0; i < 4; ++i) c[26 + i] = h[25 + i];   /* product and vendor id */
    c[30] = h[29]; c[31] = h[30];             /* version */
    put_le32(c + 53, wn8030_sum32(image + WN8030_IMAGE_CODE, code_size));
    c[64] = wn8030_sum8(c + 1, 63);
    return 1 + WN8030_HEADER_COMMAND_BYTES;
}

uint32_t wn8030_data_command(uint8_t out[1 + WN8030_DATA_COMMAND_BYTES], const uint8_t *image, uint32_t code_size, uint32_t offset,
                             uint32_t *next)
{
    if (offset >= code_size) return 0;
    const uint32_t n = code_size - offset < WN8030_DATA_CHUNK ? code_size - offset : WN8030_DATA_CHUNK;
    uint8_t *c = out + 1;
    out[0] = WN8030_REG_WRITE;
    zero(c, WN8030_DATA_COMMAND_BYTES);
    c[0] = 0x07;
    put_be32(c + 1, offset);
    put_be32(c + 5, n);
    for (uint32_t i = 0; i < n; ++i) c[9 + i] = image[WN8030_IMAGE_CODE + offset + i];
    if (next) *next = offset + n;
    return 1 + WN8030_DATA_COMMAND_BYTES;
}

int wn8030_parse(const uint8_t frame[WN8030_FRAME_BYTES], Wn8030Frame *out)
{
    zero((uint8_t *)out, sizeof *out);
    out->attached = -1;
    if (frame[0] != 0x57 || !frame[2]) return out->kind = Wn8030Invalid;
    out->report = frame + 3;
    switch (frame[3]) {
    case WN8030_KEYBOARD_ID: out->length = WN8030_KEYBOARD_REPORT; return out->kind = Wn8030Keyboard;
    case WN8030_CONSUMER_ID: out->length = WN8030_CONSUMER_REPORT; return out->kind = Wn8030Consumer;
    case WN8030_TOUCHPAD_ID: out->length = WN8030_TOUCHPAD_REPORT; return out->kind = Wn8030Touchpad;
    case 0x22: case 0x23: case 0x24: break;
    default: out->report = 0; return out->kind = Wn8030Other;
    }
    out->report = 0;
    if (frame[5] != 0x38 || frame[6] != 0x80) return out->kind = Wn8030Other;
    switch (frame[7]) {
    case 0x64:
        if (frame[8] != 0x06) return out->kind = Wn8030Other;
        /* 12-bit samples left-aligned in little-endian 16-bit words; the cover's y and z point the other way than the tablet's. */
        out->x = sext12((uint32_t)frame[10] << 4 | frame[9] >> 4);
        out->y = (int16_t)-sext12((uint32_t)frame[12] << 4 | frame[11] >> 4);
        out->z = (int16_t)-sext12((uint32_t)frame[14] << 4 | frame[13] >> 4);
        return out->kind = Wn8030Accel;
    case 0xa2:
        out->attached = (frame[12] & 3) == 3 ? 1 : (frame[12] & 3) == 0 ? 0 : -1;
        return out->kind = Wn8030Attach;
    case 0x24: return out->kind = Wn8030AuthRequest;
    case 0x31: return out->kind = Wn8030AuthInit;
    case 0x32: return out->kind = Wn8030AuthChallenge;
    default: return out->kind = Wn8030Other;
    }
}

void wn8030_release_reports(uint8_t keyboard[WN8030_KEYBOARD_REPORT], uint8_t consumer[WN8030_CONSUMER_REPORT],
                            uint8_t touchpad[WN8030_TOUCHPAD_REPORT])
{
    zero(keyboard, WN8030_KEYBOARD_REPORT); keyboard[0] = WN8030_KEYBOARD_ID;
    zero(consumer, WN8030_CONSUMER_REPORT); consumer[0] = WN8030_CONSUMER_ID;
    zero(touchpad, WN8030_TOUCHPAD_REPORT); touchpad[0] = WN8030_TOUCHPAD_ID;
}

unsigned wn8030_touchpad_report(const uint8_t in[WN8030_TOUCHPAD_REPORT], uint8_t out[WN8030_TOUCHPAD_REPORT])
{
    unsigned lifted = 0;
    for (uint32_t i = 0; i < WN8030_TOUCHPAD_REPORT; ++i) out[i] = in[i];
    for (uint32_t f = 0; f < WN8030_TOUCHPAD_FINGERS; ++f) {
        uint8_t *p = out + 2 + 6 * f;
        uint32_t y = (uint32_t)p[4] | (uint32_t)p[5] << 8;
        if (y > WN8030_TOUCHPAD_Y_MAX) y = WN8030_TOUCHPAD_Y_MAX;
        put_le16(p + 4, WN8030_TOUCHPAD_Y_MAX - y);
        if ((p[0] & 1u) && !(p[0] & 4u)) {p[0] = (uint8_t)(p[0] & ~3u); ++lifted;}
    }
    return lifted;
}

/* 3048 counts span 9.35 cm (828 per inch); half of that is a common pointer resolution, and 48 counts (1.5 mm) make one scroll step. */
#define WN8030_POINTER_DIVIDE 2
#define WN8030_SCROLL_COUNTS 48
static int32_t pointer_take(int32_t *pending, int32_t unit, int32_t limit)
{
    int32_t steps = *pending / unit;
    if (steps > limit) steps = limit;
    if (steps < -limit) steps = -limit;
    *pending -= steps * unit;
    return steps;
}
unsigned wn8030_pointer_report(struct wn8030_pointer *state, const uint8_t touchpad[WN8030_TOUCHPAD_REPORT],
                               uint8_t out[WN8030_POINTER_REPORT])
{
    uint32_t fingers = 0, contact = 0;
    int32_t x = 0, y = 0;
    for (uint32_t f = 0; f < WN8030_TOUCHPAD_FINGERS; ++f) {
        const uint8_t *p = touchpad + 2 + 6 * f;
        if (!(p[0] & 1u)) continue;
        if (!fingers) contact = p[1];
        x += (int32_t)((uint32_t)p[2] | (uint32_t)p[3] << 8);
        y += (int32_t)((uint32_t)p[4] | (uint32_t)p[5] << 8);
        ++fingers;
    }
    if (fingers) {x /= (int32_t)fingers; y /= (int32_t)fingers;}
    /* A finger that came, went or changed places starts from where it is: no jump. */
    if (fingers && fingers == state->fingers && (fingers > 1 || contact == state->contact)) {
        /* In the report as it arrives here Y grows away from the user (seen on the tablet: with Y taken as growing downwards the
         * pointer went down when the finger went up), so the pointer's Y is its negative and moving up scrolls up. */
        const int32_t dx = x - state->x, dy = state->y - y;
        if (fingers == 1) {state->move_x += dx; state->move_y += dy;}
        else {state->scroll_x += dx; state->scroll_y -= dy;}
    }
    if (fingers != state->fingers) state->move_x = state->move_y = state->scroll_x = state->scroll_y = 0;
    state->fingers = (uint8_t)fingers; state->contact = (uint8_t)contact; state->x = x; state->y = y;
    uint8_t buttons = 0;
    if (touchpad[1] & 1u) buttons |= fingers >= 2 ? 2u : 1u;
    if (touchpad[1] & 2u) buttons |= 2u;
    const int32_t mx = pointer_take(&state->move_x, WN8030_POINTER_DIVIDE, 32767), my = pointer_take(&state->move_y, WN8030_POINTER_DIVIDE, 32767);
    const int32_t sy = pointer_take(&state->scroll_y, WN8030_SCROLL_COUNTS, 127), sx = pointer_take(&state->scroll_x, WN8030_SCROLL_COUNTS, 127);
    if (!mx && !my && !sx && !sy && buttons == state->buttons) return 0;
    state->buttons = buttons;
    out[0] = WN8030_POINTER_ID; out[1] = buttons;
    put_le16(out + 2, (uint16_t)(int16_t)mx); put_le16(out + 4, (uint16_t)(int16_t)my);
    out[6] = (uint8_t)(int8_t)sy; out[7] = (uint8_t)(int8_t)sx;
    return WN8030_POINTER_REPORT;
}

