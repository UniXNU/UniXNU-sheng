// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the Apple80211 scan result (package/WifiScanResult.h): the record IO80211 26C5035r reads, where it takes the time a
// network was last heard from, and what a request with room for less gets.
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include "../WifiScanResult.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

// The record as the system reads it, by offset.
static uint64_t timestamp_at_0x8d0(const struct sheng_wifi_scan_result *r)
{
    uint8_t raw[0x8e8] = {0};
    uint64_t value;
    memcpy(raw, r, sizeof *r < sizeof raw ? sizeof *r : sizeof raw);
    memcpy(&value, raw + 0x8d0, sizeof value);
    return value;
}

static void the_record_is_the_one_io80211_reads(void)
{
    CHECK(sizeof(struct sheng_wifi_scan_result) == 0x8e8);
    CHECK(offsetof(struct sheng_wifi_scan_result, ssid_length) == 0x60);
    CHECK(offsetof(struct sheng_wifi_scan_result, age_ms) == 0x84);
    CHECK(offsetof(struct sheng_wifi_scan_result, elements_length) == 0x8a);
    CHECK(offsetof(struct sheng_wifi_scan_result, elements) == 0x8c);
    CHECK(sizeof(((struct sheng_wifi_scan_result *)0)->elements) == 2048);
}

static void the_time_heard_is_where_corewifi_takes_it(void)
{
    static struct sheng_wifi_scan_result r;
    static const uint8_t elements[] = {0, 4, 'h', 'e', 'l', 'd'};
    // Up for 3767.182 s (sleep counted), heard 2.5 s ago: CoreWiFi's now - TIMESTAMP is 2.5 s, not the 3767 s a zero gives.
    const uint64_t now = 3767182000000ull;
    sheng_wifi_scan_result_stamp(&r, elements, sizeof elements, 2500, now, 0x8e8);
    CHECK(r.age_ms == 2500);
    CHECK(timestamp_at_0x8d0(&r) == now - 2500000000ull);
    CHECK(r.elements_length == sizeof elements && !memcmp(r.elements, elements, sizeof elements));
    // Heard this instant.
    sheng_wifi_scan_result_stamp(&r, elements, sizeof elements, 0, now, 0x8e8);
    CHECK(timestamp_at_0x8d0(&r) == now);
    // An age past the clock's start cannot be: the stamp stays at the start.
    sheng_wifi_scan_result_stamp(&r, elements, sizeof elements, 5000, 4000000000ull, 0x8e8);
    CHECK(timestamp_at_0x8d0(&r) == 0);
    CHECK(sheng_wifi_scan_timestamp(1, 0xffffffffu) == 0);
}

static void a_shorter_request_gets_the_elements_it_has_room_for(void)
{
    static struct sheng_wifi_scan_result r;
    static uint8_t elements[3000];
    for (size_t i = 0; i < sizeof elements; ++i) elements[i] = (uint8_t)(i * 7 + 1);
    CHECK(sheng_wifi_scan_elements_room(0x8e8) == 2048);
    CHECK(sheng_wifi_scan_elements_room(1164) == 1024);   // the record of earlier systems
    CHECK(sheng_wifi_scan_elements_room(0x8c) == 0 && sheng_wifi_scan_elements_room(0) == 0);
    CHECK(sheng_wifi_scan_elements_room(0xffffffffu) == 2048);
    sheng_wifi_scan_result_stamp(&r, elements, sizeof elements, 0, 1, 0x8e8);
    CHECK(r.elements_length == 2048 && !memcmp(r.elements, elements, 2048));
    sheng_wifi_scan_result_stamp(&r, elements, sizeof elements, 0, 1, 1164);
    CHECK(r.elements_length == 1024 && !memcmp(r.elements, elements, 1024));
    sheng_wifi_scan_result_stamp(&r, elements, 300, 0, 1, 1164);
    CHECK(r.elements_length == 300);
}

int main(void)
{
    the_record_is_the_one_io80211_reads();
    the_time_heard_is_where_corewifi_takes_it();
    a_shorter_request_gets_the_elements_it_has_room_for();
    printf("wifi scan result: %u checks\n", checks);
    return 0;
}
