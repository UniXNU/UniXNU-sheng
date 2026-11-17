// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of what WifiNet.cpp does with the held networks and with a scan request while a network is joined
// (package/WifiScanPolicy.h): a network long unheard is not handed out, and on a settled link a scan goes on the air unless its
// channels were just there.
#include <stdio.h>
#include <stdlib.h>
#include "../WifiScanPolicy.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

static const uint32_t low[] = {2412, 2437, 2462}, high[] = {5180, 5200, 5745};
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static void a_network_long_unheard_is_not_handed_out(void)
{
    CHECK(!sheng_wifi_held_stale(0));        // the joined network, and one heard this instant
    CHECK(!sheng_wifi_held_stale(7000));
    CHECK(!sheng_wifi_held_stale(SHENG_WIFI_HELD_MS));
    CHECK(sheng_wifi_held_stale(SHENG_WIFI_HELD_MS + 1));
    CHECK(sheng_wifi_held_stale(266000));    // listed for hours after its access point was out of range
    CHECK(sheng_wifi_held_stale(0xffffffffu));
}

static void without_a_link_every_scan_goes_on_the_air(void)
{
    static struct sheng_wifi_air air;
    const uint64_t now = 100000;
    CHECK(sheng_wifi_scan_on_air(&air, ShengWifiLinkNone, 0, true, low, COUNT(low), now));
    sheng_wifi_air_mark(&air, low, COUNT(low), now);
    CHECK(sheng_wifi_scan_on_air(&air, ShengWifiLinkNone, 0, true, low, COUNT(low), now + 1));
    CHECK(sheng_wifi_scan_on_air(&air, ShengWifiLinkNone, 0, false, NULL, 0, now + 1));
}

static void a_join_under_way_and_a_new_link_keep_the_channel(void)
{
    static struct sheng_wifi_air air;
    const uint64_t now = 100000;
    CHECK(!sheng_wifi_scan_on_air(&air, ShengWifiLinkJoining, 0, true, low, COUNT(low), now));
    CHECK(!sheng_wifi_scan_on_air(&air, ShengWifiLinkUp, 0, true, low, COUNT(low), now));
    CHECK(!sheng_wifi_scan_on_air(&air, ShengWifiLinkUp, SHENG_WIFI_LINK_SETTLE_MS - 1, true, low, COUNT(low), now));
    CHECK(sheng_wifi_scan_on_air(&air, ShengWifiLinkUp, SHENG_WIFI_LINK_SETTLE_MS, true, low, COUNT(low), now));
    // Nothing is held to answer with: the scan runs, as it did.
    CHECK(sheng_wifi_scan_on_air(&air, ShengWifiLinkJoining, 0, false, low, COUNT(low), now));
    CHECK(sheng_wifi_scan_on_air(&air, ShengWifiLinkUp, 0, false, NULL, 0, now));
}

static void on_a_settled_link_channels_just_on_the_air_are_not_scanned_again(void)
{
    static struct sheng_wifi_air air;
    const uint64_t settled = SHENG_WIFI_LINK_SETTLE_MS, t = 500000;
    CHECK(sheng_wifi_scan_on_air(&air, ShengWifiLinkUp, settled, true, low, COUNT(low), t));
    sheng_wifi_air_mark(&air, low, COUNT(low), t);
    // The next client asks for the same batch 7 s later; the other batch has not been on the air.
    CHECK(!sheng_wifi_scan_on_air(&air, ShengWifiLinkUp, settled, true, low, COUNT(low), t + 7000));
    CHECK(sheng_wifi_scan_on_air(&air, ShengWifiLinkUp, settled, true, high, COUNT(high), t + 7000));
    sheng_wifi_air_mark(&air, high, COUNT(high), t + 7000);
    CHECK(!sheng_wifi_scan_on_air(&air, ShengWifiLinkUp, settled, true, low, COUNT(low), t + SHENG_WIFI_RESCAN_MS - 1));
    CHECK(sheng_wifi_scan_on_air(&air, ShengWifiLinkUp, settled, true, low, COUNT(low), t + SHENG_WIFI_RESCAN_MS));
    CHECK(!sheng_wifi_scan_on_air(&air, ShengWifiLinkUp, settled, true, high, COUNT(high), t + SHENG_WIFI_RESCAN_MS));
    // One channel of a request that was not there is enough.
    const uint32_t mixed[] = {5180, 5220};
    CHECK(sheng_wifi_scan_on_air(&air, ShengWifiLinkUp, settled, true, mixed, COUNT(mixed), t + 8000));
}

static void a_scan_of_every_channel_counts_for_each(void)
{
    static struct sheng_wifi_air air;
    const uint64_t settled = SHENG_WIFI_LINK_SETTLE_MS, t = 900000;
    // A request that names no channel asks for all of them; batches that were on the air do not add up to that.
    sheng_wifi_air_mark(&air, low, COUNT(low), t);
    sheng_wifi_air_mark(&air, high, COUNT(high), t);
    CHECK(sheng_wifi_scan_on_air(&air, ShengWifiLinkUp, settled, true, NULL, 0, t + 1000));
    sheng_wifi_air_mark(&air, NULL, 0, t + 1000);
    CHECK(!sheng_wifi_scan_on_air(&air, ShengWifiLinkUp, settled, true, NULL, 0, t + 2000));
    const uint32_t other[] = {5805};
    CHECK(!sheng_wifi_scan_on_air(&air, ShengWifiLinkUp, settled, true, other, COUNT(other), t + 2000));
    CHECK(sheng_wifi_scan_on_air(&air, ShengWifiLinkUp, settled, true, other, COUNT(other), t + 1000 + SHENG_WIFI_RESCAN_MS));
    CHECK(sheng_wifi_scan_on_air(&air, ShengWifiLinkUp, settled, true, NULL, 0, t + 1000 + SHENG_WIFI_RESCAN_MS));
}

static void more_channels_than_are_remembered_forget_the_oldest(void)
{
    static struct sheng_wifi_air air;
    const uint64_t settled = SHENG_WIFI_LINK_SETTLE_MS, t = 2000000;
    for (uint32_t i = 0; i < SHENG_WIFI_AIR_CHANNELS + 6; ++i) {
        const uint32_t one = 5000 + 5 * i;
        sheng_wifi_air_mark(&air, &one, 1, t + i);
    }
    const uint64_t now = t + 1000;
    for (uint32_t i = 0; i < SHENG_WIFI_AIR_CHANNELS + 6; ++i) {
        const uint32_t one = 5000 + 5 * i;
        // The first six gave their places to the last six: they are scanned again, which costs air time and nothing else.
        CHECK(sheng_wifi_scan_on_air(&air, ShengWifiLinkUp, settled, true, &one, 1, now) == (i < 6));
    }
    // A channel marked again keeps its place and takes the new time.
    const uint32_t again = 5000 + 5 * 10;
    sheng_wifi_air_mark(&air, &again, 1, now + SHENG_WIFI_RESCAN_MS);
    CHECK(!sheng_wifi_scan_on_air(&air, ShengWifiLinkUp, settled, true, &again, 1, now + SHENG_WIFI_RESCAN_MS + 1));
    const uint32_t neighbour = 5000 + 5 * 11;
    CHECK(sheng_wifi_scan_on_air(&air, ShengWifiLinkUp, settled, true, &neighbour, 1, now + SHENG_WIFI_RESCAN_MS + 1));
}

int main(void)
{
    a_network_long_unheard_is_not_handed_out();
    without_a_link_every_scan_goes_on_the_air();
    a_join_under_way_and_a_new_link_keep_the_channel();
    on_a_settled_link_channels_just_on_the_air_are_not_scanned_again();
    a_scan_of_every_channel_counts_for_each();
    more_channels_than_are_remembered_forget_the_oldest();
    printf("wifi scan policy: %u checks\n", checks);
    return 0;
}
