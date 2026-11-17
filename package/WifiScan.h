// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Apple80211 scan surface of WifiNet.cpp, as IO80211 26C5035r uses it. Host-tested by tests/wifi_scan_host.c.
// - The card capabilities (request 12): a bitmap, bit n in byte n / 8. Apple80211Scan keeps it at its context + 0x58 and tests two bits
//   before a scan: 22 (byte 2 bit 6), without which a scan that names networks fails at once with -3903 and never reaches the driver,
//   and 46 (byte 5 bit 6), with which the framework waits for the scan-done event (up to 20 s) before it reads the results.
//   airportd looks for a saved network it counts as hidden only by name, so without 22 such a network is never found and never joined.
// - The scan that names networks (request 86, 0x14e8 bytes, filled by __getScanData): 0x8 the number of SSID entries, 0xc the entries
//   (40 bytes each: version, length, 32 bytes), at most 9 names and one more, zeroed entry, the wildcard that asks every network to
//   answer; 0x19c/0x1a0 the BSSIDs; 0x200 the scan type (3: the results already held); 0x210/0x214 the channels.
#ifndef SHENG_WIFI_SCAN_H
#define SHENG_WIFI_SCAN_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SHENG_WIFI_CAPS_BYTES 12u
// AES, AES-CCM, power management, short slot and preamble, WPA2, WME, short guard interval at 20 MHz, scans that name networks, and
// the end of a scan reported by the scan-done event.
#define SHENG_WIFI_CAP_NAMED_SCAN 22u
#define SHENG_WIFI_CAP_SCAN_DONE_EVENT 46u

static inline void sheng_wifi_caps_fill(uint8_t caps[SHENG_WIFI_CAPS_BYTES])
{
    static const uint8_t has[] = {2, 3, 6, 9, 10, 14, 17, 19, SHENG_WIFI_CAP_NAMED_SCAN, SHENG_WIFI_CAP_SCAN_DONE_EVENT};
    memset(caps, 0, SHENG_WIFI_CAPS_BYTES);
    for (size_t i = 0; i < sizeof has; ++i) caps[has[i] / 8] |= (uint8_t)(1u << (has[i] % 8));
}

#define SHENG_WIFI_SCAN_MULTIPLE_BYTES 0x14e8u
#define SHENG_WIFI_SCAN_SSID_COUNT 0x8u
#define SHENG_WIFI_SCAN_SSIDS 0xcu
#define SHENG_WIFI_SCAN_SSID_ENTRY 40u
#define SHENG_WIFI_SCAN_SSID_MAX 10u   // 9 names and the wildcard
#define SHENG_WIFI_SCAN_TYPE 0x200u
#define SHENG_WIFI_SCAN_TYPE_CACHED 3u

struct sheng_wifi_scan_multiple {
    uint32_t named;      // entries with a name, in ssid[0..named)
    uint32_t wildcard;   // an entry without a name was given
    uint32_t scan_type;
    uint8_t length[SHENG_WIFI_SCAN_SSID_MAX];
    uint8_t ssid[SHENG_WIFI_SCAN_SSID_MAX][32];
};

static inline uint32_t sheng_wifi_scan_u32(const uint8_t *at) {uint32_t v; memcpy(&v, at, 4); return v;}

// 0 and *out filled, or -1 for a request shorter than its scan type, with more than SHENG_WIFI_SCAN_SSID_MAX entries or a name longer
// than 32 bytes.
static inline int sheng_wifi_scan_multiple_parse(const uint8_t *req, size_t size, struct sheng_wifi_scan_multiple *out)
{
    memset(out, 0, sizeof *out);
    if (!req || size < SHENG_WIFI_SCAN_TYPE + 4u) return -1;
    const uint32_t count = sheng_wifi_scan_u32(req + SHENG_WIFI_SCAN_SSID_COUNT);
    if (count > SHENG_WIFI_SCAN_SSID_MAX) return -1;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *entry = req + SHENG_WIFI_SCAN_SSIDS + i * SHENG_WIFI_SCAN_SSID_ENTRY;
        const uint32_t length = sheng_wifi_scan_u32(entry + 4);
        if (length > 32) return -1;
        if (!length) {out->wildcard = 1; continue;}
        out->length[out->named] = (uint8_t)length;
        memcpy(out->ssid[out->named], entry + 8, length);
        ++out->named;
    }
    out->scan_type = sheng_wifi_scan_u32(req + SHENG_WIFI_SCAN_TYPE);
    return 0;
}

// What the radio probes for (nw_wifi_scan takes one name or none): every network when the request has the wildcard or no name, the one
// name when it asks for exactly that one. Several names without the wildcard: every network too, which finds those that broadcast; a
// hidden one answers only a probe with its name, which takes probing for each name (not done).
static inline void sheng_wifi_scan_multiple_target(const struct sheng_wifi_scan_multiple *s, const uint8_t **ssid, uint32_t *length)
{
    if (s->named == 1 && !s->wildcard) {*ssid = s->ssid[0]; *length = s->length[0]; return;}
    *ssid = NULL;
    *length = 0;
}


// The channels a scan asks for (both requests, as __getScanData fills them): request 10 has their count at 0x44 and the list from
// 0x48, request 86 at 0x210 and from 0x214; 12 bytes each {version, channel, flags}, flags 0x8 2.4 GHz and 0x10 5 GHz (WifiNet.cpp
// fill_channel). They go to the radio as MHz (Ath12kWifi nw_wifi_scan_frequencies, at most 32): airportd's auto-join scans name two to
// eighteen, and a scan of every band instead took about 15.7 s on WCN7850, past the 12 s it is waited for, so each one ended empty.
#define SHENG_WIFI_SCAN_ONE_CHANNEL_COUNT 0x44u
#define SHENG_WIFI_SCAN_ONE_CHANNELS 0x48u
#define SHENG_WIFI_SCAN_MULTIPLE_CHANNEL_COUNT 0x210u
#define SHENG_WIFI_SCAN_MULTIPLE_CHANNELS 0x214u
#define SHENG_WIFI_SCAN_CHANNEL_ENTRY 12u
#define SHENG_WIFI_SCAN_CHANNELS_READ 64u   // entries read; a longer list is every band anyway
#define SHENG_WIFI_SCAN_FREQUENCIES 32u     // Ath12kWifi NW_WIFI_SCAN_FREQUENCIES_MAX
#define SHENG_WIFI_CHANNEL_2GHZ 0x8u
#define SHENG_WIFI_CHANNEL_5GHZ 0x10u

// MHz of a channel this driver reports (2.4 and 5 GHz), 0 for another band.
static inline uint32_t sheng_wifi_channel_mhz(uint32_t channel, uint32_t flags)
{
    if (flags & SHENG_WIFI_CHANNEL_2GHZ) return channel == 14 ? 2484 : channel >= 1 && channel <= 13 ? 2407 + 5 * channel : 0;
    if (flags & SHENG_WIFI_CHANNEL_5GHZ) return channel >= 1 && channel <= 196 ? 5000 + 5 * channel : 0;
    return 0;
}

// The distinct frequencies asked for, in order, into mhz; 0 for every band: no list, none this driver reports, more entries than
// SHENG_WIFI_SCAN_CHANNELS_READ or distinct frequencies than SHENG_WIFI_SCAN_FREQUENCIES, or a list past the size copied.
static inline uint32_t sheng_wifi_scan_frequencies(const uint8_t *req, size_t size, uint32_t count_at, uint32_t list_at,
                                                   uint32_t mhz[SHENG_WIFI_SCAN_FREQUENCIES])
{
    if (!req || size < (size_t)count_at + 4u) return 0;
    const uint32_t count = sheng_wifi_scan_u32(req + count_at);
    if (!count || count > SHENG_WIFI_SCAN_CHANNELS_READ || size < (size_t)list_at + (size_t)count * SHENG_WIFI_SCAN_CHANNEL_ENTRY) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *entry = req + list_at + i * SHENG_WIFI_SCAN_CHANNEL_ENTRY;
        const uint32_t f = sheng_wifi_channel_mhz(sheng_wifi_scan_u32(entry + 4), sheng_wifi_scan_u32(entry + 8));
        if (!f) continue;
        uint32_t seen = 0;
        for (uint32_t k = 0; k < n; ++k) seen |= mhz[k] == f;
        if (seen) continue;
        if (n == SHENG_WIFI_SCAN_FREQUENCIES) return 0;
        mhz[n++] = f;
    }
    return n;
}

#endif
