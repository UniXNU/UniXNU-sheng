// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the Apple80211 scan surface of WifiNet.cpp (package/WifiScan.h): the card capability bitmap and the multiple-SSID scan
// request (type 86) as IO80211 26C5035r's __getScanData builds it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../WifiScan.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

static int has_bit(const uint8_t caps[SHENG_WIFI_CAPS_BYTES], unsigned bit) {return (caps[bit / 8] >> (bit % 8)) & 1;}

// Apple80211Scan (26C5035r) refuses a scan that names networks (-3903, before any request reaches the driver) unless the cached
// capability byte 2 has bit 6: capability 22. A saved network airportd counts as hidden is only ever looked for that way.
static void caps_allow_named_scans(void)
{
    uint8_t caps[SHENG_WIFI_CAPS_BYTES];
    memset(caps, 0xee, sizeof caps);
    sheng_wifi_caps_fill(caps);
    CHECK(has_bit(caps, SHENG_WIFI_CAP_NAMED_SCAN));
    CHECK(caps[2] & 0x40);
    // the scan-done event the framework waits for (byte 5 bit 6), and the rest as before
    CHECK(has_bit(caps, SHENG_WIFI_CAP_SCAN_DONE_EVENT));
    CHECK(caps[5] & 0x40);
    static const unsigned before[] = {2, 3, 6, 9, 10, 14, 17, 19, 46};
    unsigned set = 0;
    for (unsigned b = 0; b < SHENG_WIFI_CAPS_BYTES * 8; ++b) set += (unsigned)has_bit(caps, b);
    for (unsigned i = 0; i < sizeof before / sizeof before[0]; ++i) CHECK(has_bit(caps, before[i]));
    CHECK(set == sizeof before / sizeof before[0] + 1);
}

// The request __getScanData fills: 0x8 the number of SSID entries, one more than the names given (the last, zeroed, is the wildcard);
// 0xc the entries, 40 bytes each {version 1, length, 32 bytes}; 0x200 the scan type.
static void put_u32(uint8_t *at, uint32_t v) {memcpy(at, &v, 4);}
static void request(uint8_t *req, const char *const *names, unsigned count, int wildcard, uint32_t scan_type)
{
    memset(req, 0, SHENG_WIFI_SCAN_MULTIPLE_BYTES);
    put_u32(req + 0, 1);
    for (unsigned i = 0; i < count; ++i) {
        uint8_t *entry = req + SHENG_WIFI_SCAN_SSIDS + i * SHENG_WIFI_SCAN_SSID_ENTRY;
        put_u32(entry, 1);
        put_u32(entry + 4, (uint32_t)strlen(names[i]));
        memcpy(entry + 8, names[i], strlen(names[i]));
    }
    put_u32(req + SHENG_WIFI_SCAN_SSID_COUNT, count + (wildcard ? 1u : 0u));
    put_u32(req + SHENG_WIFI_SCAN_TYPE, scan_type);
}

static void named_with_wildcard_is_broadcast(void)
{
    static uint8_t req[SHENG_WIFI_SCAN_MULTIPLE_BYTES];
    static const char *const names[] = {"first-network", "second"};
    request(req, names, 2, 1, 1);
    struct sheng_wifi_scan_multiple s;
    CHECK(sheng_wifi_scan_multiple_parse(req, sizeof req, &s) == 0);
    CHECK(s.named == 2 && s.wildcard == 1 && s.scan_type == 1);
    CHECK(s.length[0] == 13 && !memcmp(s.ssid[0], "first-network", 13));
    CHECK(s.length[1] == 6 && !memcmp(s.ssid[1], "second", 6));
    // the radio probes for every network: a network that broadcasts answers whatever names were asked for
    const uint8_t *ssid = (const uint8_t *)"x"; uint32_t length = 99;
    sheng_wifi_scan_multiple_target(&s, &ssid, &length);
    CHECK(ssid == NULL && length == 0);
}

static void one_name_without_wildcard_is_directed(void)
{
    static uint8_t req[SHENG_WIFI_SCAN_MULTIPLE_BYTES];
    static const char *const names[] = {"hidden-only"};
    request(req, names, 1, 0, 1);
    struct sheng_wifi_scan_multiple s;
    CHECK(sheng_wifi_scan_multiple_parse(req, sizeof req, &s) == 0);
    CHECK(s.named == 1 && s.wildcard == 0);
    const uint8_t *ssid = NULL; uint32_t length = 0;
    sheng_wifi_scan_multiple_target(&s, &ssid, &length);
    CHECK(length == 11 && ssid == s.ssid[0]);
}

static void cached_type_is_kept(void)
{
    static uint8_t req[SHENG_WIFI_SCAN_MULTIPLE_BYTES];
    static const char *const names[] = {"a"};
    request(req, names, 1, 1, SHENG_WIFI_SCAN_TYPE_CACHED);
    struct sheng_wifi_scan_multiple s;
    CHECK(sheng_wifi_scan_multiple_parse(req, sizeof req, &s) == 0);
    CHECK(s.scan_type == SHENG_WIFI_SCAN_TYPE_CACHED);
}

static void malformed_requests_are_refused(void)
{
    static uint8_t req[SHENG_WIFI_SCAN_MULTIPLE_BYTES];
    static const char *const names[] = {"a", "b"};
    struct sheng_wifi_scan_multiple s;
    request(req, names, 2, 1, 1);
    // too short to hold the scan type
    CHECK(sheng_wifi_scan_multiple_parse(req, SHENG_WIFI_SCAN_TYPE + 3, &s) != 0);
    CHECK(sheng_wifi_scan_multiple_parse(NULL, sizeof req, &s) != 0);
    // more entries than the 9 names and the wildcard the framework sends
    put_u32(req + SHENG_WIFI_SCAN_SSID_COUNT, SHENG_WIFI_SCAN_SSID_MAX + 1);
    CHECK(sheng_wifi_scan_multiple_parse(req, sizeof req, &s) != 0);
    // a name longer than 32 bytes
    request(req, names, 2, 1, 1);
    put_u32(req + SHENG_WIFI_SCAN_SSIDS + 4, 33);
    CHECK(sheng_wifi_scan_multiple_parse(req, sizeof req, &s) != 0);
    // no entries at all: a broadcast scan
    request(req, names, 0, 0, 1);
    CHECK(sheng_wifi_scan_multiple_parse(req, sizeof req, &s) == 0);
    CHECK(s.named == 0 && s.wildcard == 0);
    const uint8_t *ssid = (const uint8_t *)"x"; uint32_t length = 5;
    sheng_wifi_scan_multiple_target(&s, &ssid, &length);
    CHECK(ssid == NULL && length == 0);
}

// The channels in a request: {version, channel, flags} at 12 bytes each; flags 0x8 2.4 GHz, 0x10 5 GHz (WifiNet.cpp fill_channel).
static void put_channels(uint8_t *req, uint32_t count_at, uint32_t list_at, const uint32_t (*ch)[2], unsigned n)
{
    put_u32(req + count_at, n);
    for (unsigned i = 0; i < n; ++i) {
        put_u32(req + list_at + i * 12, 1);
        put_u32(req + list_at + i * 12 + 4, ch[i][0]);
        put_u32(req + list_at + i * 12 + 8, 0x2 | ch[i][1]);
    }
}

// airportd's auto-join scan: two channels. Scanning every band instead took about 16 s, past the 12 s the scan is waited for.
static void named_scan_channels_become_frequencies(void)
{
    static uint8_t req[SHENG_WIFI_SCAN_MULTIPLE_BYTES];
    static const char *const names[] = {"a", "b"};
    request(req, names, 2, 1, 1);
    static const uint32_t two[][2] = {{36, 0x10}, {6, 0x8}};
    put_channels(req, SHENG_WIFI_SCAN_MULTIPLE_CHANNEL_COUNT, SHENG_WIFI_SCAN_MULTIPLE_CHANNELS, two, 2);
    uint32_t mhz[SHENG_WIFI_SCAN_FREQUENCIES];
    CHECK(sheng_wifi_scan_frequencies(req, sizeof req, SHENG_WIFI_SCAN_MULTIPLE_CHANNEL_COUNT, SHENG_WIFI_SCAN_MULTIPLE_CHANNELS, mhz) == 2);
    CHECK(mhz[0] == 5180 && mhz[1] == 2437);
}

static void single_scan_channels_become_frequencies(void)
{
    static uint8_t req[0x80 + 64 * 12];
    memset(req, 0, sizeof req);
    static const uint32_t nine[][2] = {{36, 0x10}, {40, 0x10}, {44, 0x10}, {48, 0x10}, {149, 0x10}, {153, 0x10}, {157, 0x10}, {161, 0x10}, {165, 0x10}};
    put_channels(req, SHENG_WIFI_SCAN_ONE_CHANNEL_COUNT, SHENG_WIFI_SCAN_ONE_CHANNELS, nine, 9);
    uint32_t mhz[SHENG_WIFI_SCAN_FREQUENCIES];
    CHECK(sheng_wifi_scan_frequencies(req, sizeof req, SHENG_WIFI_SCAN_ONE_CHANNEL_COUNT, SHENG_WIFI_SCAN_ONE_CHANNELS, mhz) == 9);
    CHECK(mhz[0] == 5180 && mhz[4] == 5745 && mhz[8] == 5825);
    static const uint32_t edge[][2] = {{1, 0x8}, {13, 0x8}, {14, 0x8}};
    put_channels(req, SHENG_WIFI_SCAN_ONE_CHANNEL_COUNT, SHENG_WIFI_SCAN_ONE_CHANNELS, edge, 3);
    CHECK(sheng_wifi_scan_frequencies(req, sizeof req, SHENG_WIFI_SCAN_ONE_CHANNEL_COUNT, SHENG_WIFI_SCAN_ONE_CHANNELS, mhz) == 3);
    CHECK(mhz[0] == 2412 && mhz[1] == 2472 && mhz[2] == 2484);
}

// Repeated channels once; a band this driver does not report (6 GHz) left out; none left, or a list that does not fit, is every band.
static void channel_lists_are_cleaned(void)
{
    static uint8_t req[0x80 + 64 * 12];
    uint32_t mhz[SHENG_WIFI_SCAN_FREQUENCIES];
    memset(req, 0, sizeof req);
    static const uint32_t repeated[][2] = {{6, 0x8}, {6, 0x8}, {36, 0x10}, {37, 0x2000}};
    put_channels(req, SHENG_WIFI_SCAN_ONE_CHANNEL_COUNT, SHENG_WIFI_SCAN_ONE_CHANNELS, repeated, 4);
    CHECK(sheng_wifi_scan_frequencies(req, sizeof req, SHENG_WIFI_SCAN_ONE_CHANNEL_COUNT, SHENG_WIFI_SCAN_ONE_CHANNELS, mhz) == 2);
    CHECK(mhz[0] == 2437 && mhz[1] == 5180);
    static const uint32_t six[][2] = {{37, 0x2000}};
    put_channels(req, SHENG_WIFI_SCAN_ONE_CHANNEL_COUNT, SHENG_WIFI_SCAN_ONE_CHANNELS, six, 1);
    CHECK(sheng_wifi_scan_frequencies(req, sizeof req, SHENG_WIFI_SCAN_ONE_CHANNEL_COUNT, SHENG_WIFI_SCAN_ONE_CHANNELS, mhz) == 0);
    put_u32(req + SHENG_WIFI_SCAN_ONE_CHANNEL_COUNT, 0);
    CHECK(sheng_wifi_scan_frequencies(req, sizeof req, SHENG_WIFI_SCAN_ONE_CHANNEL_COUNT, SHENG_WIFI_SCAN_ONE_CHANNELS, mhz) == 0);
    // more entries than are read, a list past the bytes copied, more distinct channels than the kernel takes
    put_u32(req + SHENG_WIFI_SCAN_ONE_CHANNEL_COUNT, SHENG_WIFI_SCAN_CHANNELS_READ + 1);
    CHECK(sheng_wifi_scan_frequencies(req, sizeof req, SHENG_WIFI_SCAN_ONE_CHANNEL_COUNT, SHENG_WIFI_SCAN_ONE_CHANNELS, mhz) == 0);
    static const uint32_t two[][2] = {{1, 0x8}, {2, 0x8}};
    put_channels(req, SHENG_WIFI_SCAN_ONE_CHANNEL_COUNT, SHENG_WIFI_SCAN_ONE_CHANNELS, two, 2);
    CHECK(sheng_wifi_scan_frequencies(req, SHENG_WIFI_SCAN_ONE_CHANNELS + 12, SHENG_WIFI_SCAN_ONE_CHANNEL_COUNT, SHENG_WIFI_SCAN_ONE_CHANNELS, mhz) == 0);
    CHECK(sheng_wifi_scan_frequencies(NULL, sizeof req, SHENG_WIFI_SCAN_ONE_CHANNEL_COUNT, SHENG_WIFI_SCAN_ONE_CHANNELS, mhz) == 0);
    uint32_t many[SHENG_WIFI_SCAN_FREQUENCIES + 1][2];
    for (unsigned i = 0; i < SHENG_WIFI_SCAN_FREQUENCIES + 1; ++i) {many[i][0] = 36 + 4 * i; many[i][1] = 0x10;}
    put_channels(req, SHENG_WIFI_SCAN_ONE_CHANNEL_COUNT, SHENG_WIFI_SCAN_ONE_CHANNELS, (const uint32_t (*)[2])many, SHENG_WIFI_SCAN_FREQUENCIES + 1);
    CHECK(sheng_wifi_scan_frequencies(req, sizeof req, SHENG_WIFI_SCAN_ONE_CHANNEL_COUNT, SHENG_WIFI_SCAN_ONE_CHANNELS, mhz) == 0);
}

int main(void)
{
    caps_allow_named_scans();
    named_with_wildcard_is_broadcast();
    one_name_without_wildcard_is_directed();
    cached_type_is_kept();
    malformed_requests_are_refused();
    named_scan_channels_become_frequencies();
    single_scan_channels_become_frequencies();
    channel_lists_are_cleaned();
    printf("checks %u\n", checks);
    return 0;
}
