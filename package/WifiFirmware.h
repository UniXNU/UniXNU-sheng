/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SHENG_WIFI_FIRMWARE_H
#define SHENG_WIFI_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>
/* build-wifi-userspace.py reads this same prefix when staging the addon. */
#define SHENG_WIFI_FIRMWARE_PREFIX "firmware/ath12k/WCN7850/hw2.0"
#define SHENG_WIFI_FIRMWARE_ROOT "/private/var/ngpu/" SHENG_WIFI_FIRMWARE_PREFIX
#ifdef __cplusplus
extern "C" {
#endif
int sheng_wifi_firmware_read(const char *name, uint8_t **data, size_t *size);
#ifdef __cplusplus
}
#endif
#endif
