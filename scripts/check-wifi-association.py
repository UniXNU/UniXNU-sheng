#!/usr/bin/env python3
"""Exercise WifiNet's production association decoder with fixed wire fixtures."""
from pathlib import Path
import hashlib
import json
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build/wifi-association'
out.mkdir(parents=True, exist_ok=True)
source = (root / 'package/WifiNet.cpp').read_text()


def section(start, end):
    return source.split(start, 1)[1].split(end, 1)[0]


types = 'struct assoc_parameters {' + section('struct assoc_parameters {', 'struct link_event')
assertions = 'static_assert(sizeof(assoc_parameters)' + section(
    'static_assert(sizeof(assoc_parameters)', 'static_assert(sizeof(channels_list)')
decoder = 'int copy_in(' + section('int copy_in(', 'unsigned gResultsShown;')
join = 'int associate(apple80211req &rq) {' + section('case kAssociate: {', 'case kDisassociate:')
events = section('// events\n', 'struct channel')
link_type = 'struct link_event {' + section('struct link_event {', 'static_assert(sizeof(scan_result)')
notifications = 'void post_event(' + section('void post_event(', '// ---- what the module tells us ----')
callbacks = 'void on_connect_done(' + section('void on_connect_done(', '// What the address configuration exchanges')
power = 'int power(apple80211req &rq) {' + section('case kPower: {', 'case kScanReq:')
auth = 'int auth(apple80211req &rq) { uint32_t w[4]{}; w[0]' + section('case kAuthType: w[0]', 'case kPowerSave:') + '}\n'

prefix = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <vector>
#include "nw_abi.h"
using test_user_addr_t = uintptr_t;
#define user_addr_t test_user_addr_t
struct apple80211req { uint32_t req_len; user_addr_t req_data; };
static bool copy_fault, gUserJoin, gKeyed, had_bssid, had_key;
static bool gPower = true, gLinkSuppressed;
static int gAssocResult, calls, connect_error;
static int disconnects;
static bool disconnect_queued, hold_disconnect;
static unsigned sleep_ms;
static nw_wifi_status physical;
static void nw_wifi_status_get_mock(nw_wifi_status *st) { *st = physical; }
static void nw_wifi_disconnect_mock() { ++disconnects; disconnect_queued = true; }
static void sheng_wifi_wake() {}
static void IOSleep(unsigned ms) {
    sleep_ms += ms;
    if (disconnect_queued && !hold_disconnect) { physical.state = NW_STA_IDLE; disconnect_queued = false; }
}
#define nw_wifi_status_get nw_wifi_status_get_mock
#define nw_wifi_disconnect nw_wifi_disconnect_mock
// Stand-ins for the BSD event sink; production event serialization runs below.
static int gIf = 1;
static constexpr unsigned IFF_RUNNING = 0x40;
static constexpr unsigned KEV_VENDOR_APPLE = 1, KEV_NETWORK_CLASS = 1, KEV_DL_SUBCLASS = 2;
static constexpr unsigned KEV_DL_LINK_ON = 16, KEV_DL_LINK_OFF = 17, IFNET_FAMILY_ETHERNET = 2;
static unsigned if_flags;
struct net_event_data { uint32_t if_family, if_unit; char if_name[16]; };
struct kev_msg {
    uint32_t vendor_code, kev_class, kev_subclass, event_code;
    struct { uint32_t data_length; void *data_ptr; } dv[5];
};
struct Event { uint32_t cls, code; std::vector<uint8_t> data; };
static std::vector<Event> sent;
static int ifnet_set_flags(int, unsigned flags, unsigned mask) {
    if_flags = (if_flags & ~mask) | (flags & mask); return 0;
}
static int sheng_kev_post(kev_msg *msg) {
    auto p = static_cast<uint8_t *>(msg->dv[0].data_ptr);
    sent.push_back({msg->kev_class, msg->event_code, {p, p + msg->dv[0].data_length}}); return 0;
}
static uint8_t seen_ssid[32], seen_bssid[6];
static char seen_key[64];
static size_t seen_ssid_length, seen_key_length;
static void log(int, const char *, ...) {}
static int copyin(user_addr_t src, void *dst, size_t size) {
    if (copy_fault) return EFAULT;
    memcpy(dst, reinterpret_cast<const void *>(src), size); return 0;
}
static int copy_out(const apple80211req &rq, const void *src, size_t size) {
    memcpy(reinterpret_cast<void *>(rq.req_data), src, rq.req_len < size ? rq.req_len : size); return 0;
}
static int nw_wifi_connect_mock(const uint8_t *ssid, size_t length, const uint8_t *bssid,
                           const char *key, size_t key_length) {
    ++calls; seen_ssid_length = length; seen_key_length = key_length;
    assert(length <= sizeof seen_ssid && key_length <= sizeof seen_key);
    memcpy(seen_ssid, ssid, length); had_bssid = bssid != nullptr; had_key = key != nullptr;
    if (bssid) memcpy(seen_bssid, bssid, 6);
    if (key) memcpy(seen_key, key, key_length);
    return connect_error;
}
#define nw_wifi_connect nw_wifi_connect_mock
'''
fixtures = r'''
static uint8_t wire[656];
static void put32(unsigned offset, uint32_t value) { memcpy(wire + offset, &value, 4); }
static apple80211req fixture(unsigned length, unsigned key_length, unsigned cipher) {
    memset(wire, 0, sizeof wire); calls = 0; copy_fault = false; connect_error = 0;
    gUserJoin = false; gKeyed = false; gPower = true; gLinkSuppressed = false;
    physical = {}; physical.state = NW_STA_IDLE; sent.clear(); disconnects = 0; if_flags = 0;
    disconnect_queued = hold_disconnect = false; sleep_ms = 0;
    // Fixed offsets from the legacy wire contract and 24A446 IO80211 disassembly.
    const bool modern = length == 656;
    put32(0, 1);
    put32(modern ? 0x10 : 0x0c, 8);
    memcpy(wire + (modern ? 0x14 : 0x10), "test-net", 8);
    memcpy(wire + (modern ? 0x34 : 0x30), "\x02\x11\x22\x33\x44\x55", 6);
    put32(modern ? 0x3c : 0x38, 1);
    put32(modern ? 0x40 : 0x3c, key_length);
    put32(modern ? 0x44 : 0x40, cipher);
    for (unsigned i = 0; i < 64; ++i) wire[(modern ? 0x4c : 0x48) + i] = uint8_t(i);
    return {length, reinterpret_cast<user_addr_t>(wire)};
}
int main() {
    for (unsigned length : {592u, 656u}) {
        const unsigned shift = length == 656 ? 4 : 0;
        auto rq = fixture(length, 32, 6);
        assert(associate(rq) == 0 && calls == 1 && gUserJoin && gKeyed);
        assert(seen_ssid_length == 8 && !memcmp(seen_ssid, "test-net", 8));
        assert(had_bssid && !memcmp(seen_bssid, "\x02\x11\x22\x33\x44\x55", 6));
        assert(had_key && seen_key_length == 64);
        assert(!memcmp(seen_key, "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", 64));

        for (unsigned n : {8u, 63u}) {
            rq = fixture(length, n, 10); memset(wire + 0x48 + shift, 'p', n);
            assert(associate(rq) == 0 && calls == 1 && seen_key_length == n);
            assert(!memcmp(seen_key, wire + 0x48 + shift, n));
        }
        rq = fixture(length, 0, 0); memset(wire + 0x30 + shift, 0, 6);
        assert(associate(rq) == 0 && calls == 1 && !had_key && !had_bssid && !gKeyed);
        for (unsigned n : {1u, 7u, 64u, 65u}) {
            rq = fixture(length, n, 10); assert(associate(rq) == EINVAL && calls == 0);
        }
        rq = fixture(length, 31, 6); assert(associate(rq) == EINVAL && calls == 0);
        rq = fixture(length, 32, 32); assert(associate(rq) == EINVAL && calls == 0);
        for (unsigned n : {0u, 33u}) {
            rq = fixture(length, 32, 6); put32(0x0c + shift, n);
            assert(associate(rq) == EINVAL && calls == 0);
        }
        rq = fixture(length, 32, 6); put32(0, 2);
        assert(associate(rq) == EINVAL && calls == 0);
        rq = fixture(length, 32, 6); rq.req_data = 0;
        assert(associate(rq) == EINVAL && calls == 0);
        rq = fixture(length, 32, 6); copy_fault = true;
        assert(associate(rq) == EFAULT && calls == 0);
        rq = fixture(length, 32, 6); connect_error = -1;
        assert(associate(rq) == EINVAL && calls == 1);
    }
    for (unsigned n : {0u, 136u, 591u, 593u, 655u, 657u}) {
        auto rq = fixture(656, 32, 6); rq.req_len = n;
        assert(associate(rq) == EINVAL && calls == 0);
    }
    auto rq = fixture(656, 32, 6);
    assert(associate(rq) == 0);
    physical.state = NW_STA_UP; physical.ssid_len = 8;
    memcpy(physical.ssid, "test-net", 8);
    memcpy(physical.bssid, "\x02\x11\x22\x33\x44\x55", 6);
    on_connect_done(NW_RES_OK);
    assert(gAssocResult == 1 && (if_flags & IFF_RUNNING));
    assert(sent.size() == 5 && sent[1].cls == 6 && sent[1].code == 9);
    assert(sent[1].data.size() == 16 && !memcmp(sent[1].data.data(), "en0\0", 4));
    int32_t result = -1;
    // Repeating the completed join must signal completion without queueing radio work.
    sent.clear(); const int queued = calls;
    assert(associate(rq) == 0 && calls == queued && gAssocResult == 1);
    assert(sent.size() == 5 && sent[1].code == 9 && sent[1].data.size() == 16);
    // Switching targets drains the previous connection before queuing another join.
    wire[0x14] = 'X'; sent.clear();
    assert(associate(rq) == 0 && calls == queued + 1 && disconnects == 1 && sleep_ms == 10);
    assert(physical.state == NW_STA_IDLE && !gLinkSuppressed && seen_ssid[0] == 'X');
    wire[0x14] = 't'; physical.state = NW_STA_UP; wire[0x34] ^= 1; hold_disconnect = true;
    const unsigned before_sleep = sleep_ms;
    assert(associate(rq) == ETIMEDOUT && calls == queued + 1 && sleep_ms - before_sleep == 2000);
    wire[0x34] ^= 1; hold_disconnect = false; disconnect_queued = false;
    gLinkSuppressed = false; calls = queued; sent.clear(); disconnects = 0;
    physical.state = NW_STA_KEYS; gAssocResult = 2;
    assert(associate(rq) == 0 && calls == queued && sent.empty() && gAssocResult == 2);
    // Power-off publishes a voluntary link-down and hides the asynchronous old snapshot.
    physical.state = NW_STA_UP;
    uint32_t off[6] = {1, 1, 0}; apple80211req pr = {sizeof off, reinterpret_cast<user_addr_t>(off)};
    assert(power(pr) == 0 && disconnects == 1 && !gPower && gLinkSuppressed);
    assert(!(if_flags & IFF_RUNNING) && sent.size() == 5);
    assert(sent[0].cls == 1 && sent[0].code == KEV_DL_LINK_OFF);
    assert(sent[1].cls == 6 && sent[1].code == 4 && sent[1].data.size() == 48);
    assert(sent[1].data[16] == 1 && sent[1].data[32] == 0);
    assert(sent[2].code == 2 && sent[3].code == 3 && sent[4].code == 1);
    nw_wifi_status visible; visible_status(visible);
    assert(visible.state == NW_STA_IDLE && !visible.ssid_len && !visible.bssid[0]);
    sent.clear(); on_connect_done(NW_RES_OK);
    assert(sent.empty() && !(if_flags & IFF_RUNNING));
    assert(associate(rq) == ENETDOWN && calls == queued);
    off[2] = 1; assert(power(pr) == 0 && gPower);
    visible_status(visible); assert(visible.state == NW_STA_IDLE);
    assert(associate(rq) == 0 && calls == queued + 1 && !gLinkSuppressed); // drain, then rejoin
    physical.state = NW_STA_UP; on_connect_done(NW_RES_OK);
    sent.clear(); disconnect_user(); assert(!(if_flags & IFF_RUNNING) && sent.size() == 4);
    gLinkSuppressed = false; sent.clear(); on_link_lost(16);
    assert(sent.size() == 4 && sent[1].data[32] == 1 && gAssocResult == 2);
    memcpy(&result, sent[1].data.data() + 36, 4); assert(result == 16);
    uint32_t authentication[4] = {}; apple80211req ar = {sizeof authentication, reinterpret_cast<user_addr_t>(authentication)};
    gKeyed = true; assert(auth(ar) == 0);
    assert(authentication[0] == 1 && authentication[1] == 1 && authentication[2] == 0x10 && authentication[3] == 0);
    ar.req_len = 12; authentication[3] = 123; gKeyed = false;
    assert(auth(ar) == 0 && authentication[2] == 0 && authentication[3] == 123);
    puts("PASS: wire layouts, credentials, duplicate/switch joins, teardown timeout, completion and power-off/leave");
}
'''
unit = prefix + '#include <initializer_list>\n' + events + types + link_type + assertions + decoder + notifications + callbacks + power + auth + join + fixtures
(out / 'test.cpp').write_text(unit)
subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                '-fsanitize=address,undefined', '-I', str(root / 'drivers/Ath12kWifi/include'),
                str(out / 'test.cpp'), '-o', str(out / 'test')], check=True)
subprocess.run([str(out / 'test')], check=True)
(out / 'report.json').write_text(json.dumps({
    'passed': True, 'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
    'copyin_and_station_substituted': True, 'hardware_verified': False,
}, indent=2) + '\n')
