// SPDX-License-Identifier: BSD-2-Clause-Patent
// IO80211 26C5035r scan results use a 0x8e8-byte record, 2048 information-element bytes at 0x8c,
// and a continuous-clock timestamp at 0x8d0. CoreWiFi uses the timestamp to age results.
// Short requests receive the record prefix and the information elements that fit.
#ifndef SHENG_WIFI_SCAN_RESULT_H
#define SHENG_WIFI_SCAN_RESULT_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SHENG_WIFI_SCAN_RESULT_BYTES 0x8e8u
#define SHENG_WIFI_SCAN_ELEMENTS 0x8cu          // where the elements start
#define SHENG_WIFI_SCAN_ELEMENTS_BYTES 2048u
#define SHENG_WIFI_SCAN_TIMESTAMP 0x8d0u

struct sheng_wifi_channel { uint32_t version, channel, flags; };
struct sheng_wifi_scan_result {
    uint32_t version; struct sheng_wifi_channel ch; uint16_t noise_known; int16_t noise, snr, rssi; uint16_t beacon_interval, capability;
    uint8_t bssid[6]; uint8_t rate_count, unused0; uint32_t rates[15]; uint8_t ssid_length, ssid[32], unused1[3]; uint32_t age_ms;
    uint8_t flags, unused2; uint16_t elements_length; uint8_t elements[SHENG_WIFI_SCAN_ELEMENTS_BYTES];
    uint8_t unused3[68];
    uint64_t timestamp;
    uint8_t unused4[16];
};

// When a network heard age_ms ago was heard; now_ns is the clock that counts sleep, read at the hand-out.
static inline uint64_t sheng_wifi_scan_timestamp(uint64_t now_ns, uint32_t age_ms)
{
    const uint64_t age_ns = (uint64_t)age_ms * 1000000u;
    return now_ns > age_ns ? now_ns - age_ns : 0;
}

// The elements a request of request_bytes has room for.
static inline uint16_t sheng_wifi_scan_elements_room(uint32_t request_bytes)
{
    if (request_bytes <= SHENG_WIFI_SCAN_ELEMENTS) return 0;
    const uint32_t room = request_bytes - SHENG_WIFI_SCAN_ELEMENTS;
    return (uint16_t)(room < SHENG_WIFI_SCAN_ELEMENTS_BYTES ? room : SHENG_WIFI_SCAN_ELEMENTS_BYTES);
}

// The part of a record that depends on the hand-out: the age and the time heard, and the elements the request has room for.
static inline void sheng_wifi_scan_result_stamp(struct sheng_wifi_scan_result *r, const uint8_t *elements, size_t length, uint32_t age_ms,
                                                uint64_t now_ns, uint32_t request_bytes)
{
    const uint16_t room = sheng_wifi_scan_elements_room(request_bytes);
    r->age_ms = age_ms;
    r->timestamp = sheng_wifi_scan_timestamp(now_ns, age_ms);
    r->elements_length = (uint16_t)(length < room ? length : room);
    memcpy(r->elements, elements, r->elements_length);
}
#endif
