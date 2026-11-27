// SPDX-License-Identifier: BSD-2-Clause-Patent
// What WifiNet.cpp does with the networks the Wi-Fi module holds and with a scan request while a network is joined. Host-tested by
// tests/wifi_scan_policy_host.c.
// - The module holds the networks of its last scan, each with the time since it was last heard (nw_wifi_bss_get). The system's list
//   of networks shows what a scan hands it, of any age, so a held network not heard for SHENG_WIFI_HELD_MS is not handed out: one
//   whose access point went out of range leaves the list. The joined network reads an age of 0 while its link is up.
// - While a join is under way, and for SHENG_WIFI_LINK_SETTLE_MS after the link came up, a scan is answered from the held networks:
//   off the channel the access point's handshake frames and the DHCP answers are missed.
// - On a settled link a scan goes on the air; the firmware returns to the access point's channel between the channels it visits.
//   The system asks for the channels in batches and several of its clients ask in turn, about every 7 s in all, so a request whose
//   channels were all on the air within SHENG_WIFI_RESCAN_MS is answered from the held networks.
#ifndef SHENG_WIFI_SCAN_POLICY_H
#define SHENG_WIFI_SCAN_POLICY_H
#include <stddef.h>
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#define SHENG_WIFI_HELD_MS 60000u
#define SHENG_WIFI_LINK_SETTLE_MS 15000u
#define SHENG_WIFI_RESCAN_MS 60000u
#define SHENG_WIFI_AIR_CHANNELS 64u

// When each channel was last scanned on the air, in ms of a clock that starts above 0; 0: not yet. Zero-initialised.
struct sheng_wifi_air {
    uint64_t every_ms;                           // a scan that named no channel: every channel
    uint32_t mhz[SHENG_WIFI_AIR_CHANNELS];
    uint64_t at_ms[SHENG_WIFI_AIR_CHANNELS];
};
enum sheng_wifi_link {ShengWifiLinkNone, ShengWifiLinkJoining, ShengWifiLinkUp};

static inline bool sheng_wifi_held_stale(uint32_t age_ms) {return age_ms > SHENG_WIFI_HELD_MS;}

// A scan of these channels (count 0: of every channel) went on the air at now_ms. A channel beyond the places takes the oldest one.
static inline void sheng_wifi_air_mark(struct sheng_wifi_air *air, const uint32_t *mhz, size_t count, uint64_t now_ms)
{
    if (!count) {
        air->every_ms = now_ms;
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        size_t place = 0;
        for (size_t slot = 0; slot < SHENG_WIFI_AIR_CHANNELS; ++slot) {
            if (air->mhz[slot] == mhz[i]) {
                place = slot;
                break;
            }
            if (air->at_ms[slot] < air->at_ms[place]) place = slot;   // an empty place is the oldest
        }
        air->mhz[place] = mhz[i];
        air->at_ms[place] = now_ms;
    }
}

static inline bool sheng_wifi_air_recent(uint64_t at_ms, uint64_t now_ms)
{
    return at_ms && now_ms >= at_ms && now_ms - at_ms < SHENG_WIFI_RESCAN_MS;
}

// Whether a scan request goes on the air. link_ms: how long the link has been up; held: there are held networks to answer with;
// mhz, count: the channels asked for (count 0: every channel).
static inline bool sheng_wifi_scan_on_air(const struct sheng_wifi_air *air, enum sheng_wifi_link link, uint64_t link_ms, bool held,
                                          const uint32_t *mhz, size_t count, uint64_t now_ms)
{
    if (link == ShengWifiLinkNone || !held) return true;
    if (link == ShengWifiLinkJoining || link_ms < SHENG_WIFI_LINK_SETTLE_MS) return false;
    if (sheng_wifi_air_recent(air->every_ms, now_ms)) return false;
    if (!count) return true;
    for (size_t i = 0; i < count; ++i) {
        size_t slot = 0;
        while (slot < SHENG_WIFI_AIR_CHANNELS && air->mhz[slot] != mhz[i]) ++slot;
        if (slot == SHENG_WIFI_AIR_CHANNELS || !sheng_wifi_air_recent(air->at_ms[slot], now_ms)) return true;
    }
    return false;
}
#endif
