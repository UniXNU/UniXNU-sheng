/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SHENG_BLUETOOTH_FIRMWARE_H
#define SHENG_BLUETOOTH_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>
/* stage-bluetooth-firmware.py consumes this exact addon-relative prefix. */
#define SHENG_BT_FIRMWARE_PREFIX "firmware/qca"
#define SHENG_BT_FIRMWARE_ROOT "/private/var/ngpu/" SHENG_BT_FIRMWARE_PREFIX
#ifdef __cplusplus
extern "C" {
#endif
int sheng_bt_firmware_read(const char *name, uint8_t **data, size_t *size);
void sheng_bt_firmware_release(uint8_t *data, size_t size);
#ifdef __cplusplus
}
#endif
#endif
