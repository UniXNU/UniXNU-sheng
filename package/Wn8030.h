/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The Nanosic WN8030 keyboard controller of the sheng keyboard cover: I2C client 0x4c on QUP2 SE1. Its boot ROM takes a RAM image over I2C,
 * then the firmware raises GPIO 62 for every 68-byte frame it has; the host reads the frame with register 0x4c. Frames carry the keyboard
 * (report 5), consumer (report 6) and touchpad (report 0x19) input reports of the cover, and vendor messages (attach state, the cover's
 * accelerometer, authentication). Host commands are 66-byte writes to register 0x5c. This is the message codec only: no I/O, no waits. */
#ifndef WN8030_H
#define WN8030_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define WN8030_ADDRESS 0x4cu
#define WN8030_REG_WRITE 0x5cu
#define WN8030_REG_READ 0x4cu
#define WN8030_FRAME_BYTES 68u
#define WN8030_COMMAND_BYTES 66u        /* after the register byte */
#define WN8030_BOOT_ID 0xc8u
/* Boot ROM commands, written as {0x5c, command}; the state is read back as register 0x5c after {0x5c, 0x05}. */
#define WN8030_ROM_ID 0x04u
#define WN8030_ROM_STATE 0x05u
#define WN8030_ROM_VERIFY 0x08u
#define WN8030_ROM_BOOT 0x09u
enum {Wn8030RomIdle = 0, Wn8030RomHeader = 1, Wn8030RomVerified = 3};

/* The RAM image: a 64-byte header at 0x7fc0 ("NANO IC"), the code from 0x8000. */
#define WN8030_IMAGE_HEADER 0x7fc0u
#define WN8030_IMAGE_CODE 0x8000u
#define WN8030_IMAGE_MIN 0xa000u
#define WN8030_HEADER_COMMAND_BYTES 65u
#define WN8030_DATA_COMMAND_BYTES 512u
#define WN8030_DATA_CHUNK 503u

/* Control commands (66-byte frames {0x32, 0, 0x4e, 0x31, 0x80, 0x38, command, length, payload, checksum}). */
#define WN8030_CMD_TOUCHPAD 0x21u       /* 1 enables the touchpad reports */
#define WN8030_CMD_BACKLIGHT 0x23u      /* 0..100 */
#define WN8030_CMD_KEYBOARD_POWER 0x25u /* 1 keeps the cover's own controller awake */
#define WN8030_CMD_INDICATOR 0x2eu      /* one of the indicator values below */
#define WN8030_LED_CAPS_ON 0xfdu
#define WN8030_LED_CAPS_OFF 0xfcu
#define WN8030_LED_MIC_ON 0xf7u
#define WN8030_LED_MIC_OFF 0xf3u
#define WN8030_BACKLIGHT_MAX 100u

/* Input report sizes, report ID included. */
#define WN8030_KEYBOARD_REPORT 9u
#define WN8030_CONSUMER_REPORT 3u
#define WN8030_TOUCHPAD_REPORT 21u
/* Touchpad report: buttons, three fingers of {flags (bit 0 tip, 1 in range, 2 confidence), contact id, X, Y (little-endian)}, count. */
#define WN8030_TOUCHPAD_FINGERS 3u
#define WN8030_TOUCHPAD_X_MAX 3047u
#define WN8030_TOUCHPAD_Y_MAX 2031u
#define WN8030_KEYBOARD_ID 0x05u
#define WN8030_CONSUMER_ID 0x06u
#define WN8030_TOUCHPAD_ID 0x19u

enum {
    Wn8030Invalid = 0,      /* no frame (bad magic or empty) */
    Wn8030Keyboard,         /* report points at WN8030_KEYBOARD_REPORT bytes */
    Wn8030Consumer,
    Wn8030Touchpad,
    Wn8030Attach,           /* attached: 1 the cover is on, 0 it is off, -1 a state change in progress */
    Wn8030Accel,            /* the cover's accelerometer: x, y, z in 1/256 g, axes of the tablet */
    Wn8030AuthRequest,      /* the cover asks for authentication */
    Wn8030AuthInit,
    Wn8030AuthChallenge,
    Wn8030Other
};

typedef struct {
    int kind;
    const uint8_t *report;
    uint32_t length;
    int attached;
    int16_t x, y, z;
} Wn8030Frame;

uint8_t wn8030_sum8(const uint8_t *bytes, uint32_t length);
/* The sum of little-endian 32-bit words over length bytes (bytes past the end read as zero). */
uint32_t wn8030_sum32(const uint8_t *bytes, uint32_t length);

/* A control command with its register byte: out[0] = 0x5c, then the 66-byte frame. Returns the bytes to write (67), 0 for a payload
 * over 57 bytes. */
uint32_t wn8030_control(uint8_t out[1 + WN8030_COMMAND_BYTES], uint8_t command, const uint8_t *payload, uint8_t length);

/* Checks a RAM image (size, header magic, code inside the file, the header's byte sum of the code); returns 0 and the code size, or a
 * negative step: -1 size, -2 magic, -3 code size, -4 code checksum. */
int wn8030_image_check(const uint8_t *image, uint32_t size, uint32_t *code_size);
/* The header command with its register byte (66 bytes). */
uint32_t wn8030_header_command(uint8_t out[1 + WN8030_HEADER_COMMAND_BYTES], const uint8_t *image, uint32_t code_size);
/* The data command for the code at offset with its register byte (513 bytes); *next is the offset of the following chunk. */
uint32_t wn8030_data_command(uint8_t out[1 + WN8030_DATA_COMMAND_BYTES], const uint8_t *image, uint32_t code_size, uint32_t offset,
                             uint32_t *next);

/* Classifies one frame read from register 0x4c. */
int wn8030_parse(const uint8_t frame[WN8030_FRAME_BYTES], Wn8030Frame *out);

/* The touchpad report as the host's device takes it: Y counted from the other edge (the controller's Y grows toward the hinge) and a
 * touching contact without confidence (a palm) sent as lifted, since iPadOS's trackpad filter fails on a touching palm that is the last
 * contact of a frame. Returns the number of contacts sent as lifted. */
unsigned wn8030_touchpad_report(const uint8_t in[WN8030_TOUCHPAD_REPORT], uint8_t out[WN8030_TOUCHPAD_REPORT]);

/* The reports that release every key, consumer usage and finger (input gated off). */
void wn8030_release_reports(uint8_t keyboard[WN8030_KEYBOARD_REPORT], uint8_t consumer[WN8030_CONSUMER_REPORT],
                            uint8_t touchpad[WN8030_TOUCHPAD_REPORT]);

#ifdef __cplusplus
}
#endif
#endif
