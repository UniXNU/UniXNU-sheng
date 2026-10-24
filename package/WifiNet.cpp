// SPDX-License-Identifier: BSD-2-Clause-Patent
// The WiFi module's network side: the Ethernet interface the system routes through, the registry entries by which the wireless daemon and
// the network configuration find a wireless interface, and the Apple80211 requests (ioctls on that interface) the daemon uses to scan,
// join and read state. Nothing here talks to the radio: it calls the module's station interface (nw_abi.h) and turns what it answers into
// the structures the system's wireless framework reads.
//
// Layouts below are those the framework reads and writes; where a field's meaning is not known it is left zero. Every request is written
// to the log (type, direction, length) and a request that is not handled is refused, so which ones the framework needs shows in the log.
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOUserClient.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSerialize.h>
#include <sys/kern_event.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_media.h>
#include <net/if_ether.h>
#include <net/kpi_interface.h>
extern "C" {
#include <net/bpf.h>
}
#include <net/if_types.h>
#include <netinet/in.h>
#include <sys/mbuf.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <sys/systm.h>
#include <stdarg.h>
#include "nw_abi.h"

extern "C" void native_wifi_logf(int level, const char *fmt, ...);
extern "C" int sheng_wifi_file(const char *name, const uint8_t **data, size_t *size);
extern "C" void sheng_wifi_wake(void);   // WifiProbe.cpp: the WiFi thread sends the queued frame now

#define SIOCSA80211 0x802869c8UL   // _IOW('i', 200, struct apple80211req)
#define SIOCGA80211 0xc02869c9UL   // _IOWR('i', 201, struct apple80211req)

// (the header declares it without C linkage; the symbol is the C one)
extern "C" int sheng_kev_post(struct kev_msg *) __asm__("_kev_post_msg");
extern "C" int boot_kthread_report(const char *name, char *out, unsigned size);   // BootUiTraceProbe.c
extern "C" unsigned boot_cpu_kick(char *out, unsigned size);                      // BootUiTraceProbe.c

namespace {

struct apple80211req {
  char req_if_name[16];
  int32_t req_type;
  int32_t req_val;
  uint32_t req_len;
  user_addr_t req_data __attribute__((aligned(8)));
};

// request types
enum {
  kSsid = 1, kAuthType = 2, kCipherKey = 3, kChannel = 4, kPowerSave = 5, kTxPower = 7, kRate = 8, kBssid = 9, kScanReq = 10, kScanResult = 11,
  kCardCaps = 12, kState = 13, kPhyMode = 14, kOpMode = 15, kRssi = 16, kNoise = 17, kPower = 19, kAssociate = 20, kAssociateResult = 21,
  kDisassociate = 22, kSupportedChannels = 27, kLocale = 28, kDeauth = 29, kDriverVersion = 43, kHardwareVersion = 44, kCountryCode = 51,
  kRadioInfo = 54, kScanReqMultiple = 86, kScanCacheClear = 90, kCurrentNetwork = 103, kChannelsInfo = 207, kHwSupportedChannels = 254,
};
// events
enum { kEvPowerChanged = 1, kEvSsidChanged = 2, kEvBssidChanged = 3, kEvLinkChanged = 4, kEvAssocDone = 9, kEvScanDone = 10 };
// results of an association, as the framework numbers them
enum { kAssocOk = 1, kAssocFailed = 2, kAssocTimeout = 9, kAssocBadKey = 21 };

struct channel { uint32_t version, channel, flags; };
struct channels_list { uint32_t version, count; channel list[401]; uint32_t unused; };           // 4824 bytes
struct scan_result {                                                                              // 1164 bytes
  uint32_t version; channel ch; uint16_t noise_known; int16_t noise, snr, rssi; uint16_t beacon_interval, capability; uint8_t bssid[6];
  uint8_t rate_count, unused0; uint32_t rates[15]; uint8_t ssid_length, ssid[32], unused1[3]; uint32_t age_ms; uint8_t flags, unused2;
  uint16_t elements_length; uint8_t elements[1024];
};
struct assoc_request {                                                                            // 592 bytes
  uint32_t version; uint16_t mode, auth_lower, auth_upper, unused0; uint32_t ssid_length; uint8_t ssid[32]; uint8_t bssid[6], unused1[2];
  uint32_t key_version, key_length, key_cipher; uint16_t key_flags, key_index; uint8_t key[64]; uint8_t rest[0x250 - 0x88];
};
struct link_event { uint8_t down, unused0[3]; int32_t rssi; uint16_t snr, noise; uint8_t unused1[4]; uint8_t involuntary, unused2[3]; uint32_t reason, subreason; uint8_t unused3[4]; };
static_assert(sizeof(scan_result) == 1164 && __builtin_offsetof(scan_result, ssid_length) == 0x60 && __builtin_offsetof(scan_result, age_ms) == 0x84 &&
              __builtin_offsetof(scan_result, elements) == 0x8c, "scan result layout");
static_assert(sizeof(assoc_request) == 592 && __builtin_offsetof(assoc_request, key) == 0x48, "association layout");
static_assert(sizeof(channels_list) == 4824, "channel list layout");
static_assert(sizeof(link_event) == 32, "link event layout");

ifnet_t gIf;
uint8_t gMac[6];
lck_grp_t *gGrp;
lck_mtx_t *gLock;
bool gPower = true;
bool gKeyed;   // the last join used a passphrase
uint32_t gRxFrames, gRxErrors; int gRxLastError;
uint32_t gRxNoBuffer, gRxChains, gRxLongest;   // frames dropped for want of a buffer, frames that took a chain, the longest frame
struct { unsigned count, kicks, cleared, longest; bool kicked; } gStall;   // input thread stalls (check_input)   // frames handed to the stack, refused ones and the last refusal
int gAssocResult = kAssocOk;
unsigned gScanCursor, gScanning;
bool gUserJoin;   // the wireless daemon joined, left or switched the radio itself: the unattended test join steps aside
uint32_t gLogged[256 / 32];

void log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void log(int level, const char *fmt, ...) {
  char line[160]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
  native_wifi_logf(level, "%s", line);
}

// ---- events to the framework: class IEEE 802.11 (6), subclass 1, data starting with the interface name ----
void post_event(uint32_t code, const void *payload, size_t length) {
  uint8_t data[128]; if (length > sizeof data - 16) length = sizeof data - 16;
  memset(data, 0, 16); snprintf(reinterpret_cast<char *>(data), 16, "en0"); if (length) memcpy(data + 16, payload, length);
  struct kev_msg msg; memset(&msg, 0, sizeof msg);
  msg.vendor_code = KEV_VENDOR_APPLE; msg.kev_class = 6; msg.kev_subclass = 1; msg.event_code = code;
  msg.dv[0].data_length = static_cast<uint32_t>(16 + length); msg.dv[0].data_ptr = data;
  sheng_kev_post(&msg);
}
// link state for the network configuration: the interface's flags and the stack's own link events
void post_link(bool up) {
  if (!gIf) return;
  ifnet_set_flags(gIf, up ? IFF_RUNNING : 0, IFF_RUNNING);
  struct { struct net_event_data data; } payload; memset(&payload, 0, sizeof payload);
  payload.data.if_family = IFNET_FAMILY_ETHERNET; payload.data.if_unit = 0; snprintf(payload.data.if_name, sizeof payload.data.if_name, "en");
  struct kev_msg msg; memset(&msg, 0, sizeof msg);
  msg.vendor_code = KEV_VENDOR_APPLE; msg.kev_class = KEV_NETWORK_CLASS; msg.kev_subclass = KEV_DL_SUBCLASS; msg.event_code = up ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF;
  msg.dv[0].data_length = sizeof payload; msg.dv[0].data_ptr = &payload;
  sheng_kev_post(&msg);
}

// ---- what the module tells us ----
void on_scan_done() { __atomic_store_n(&gScanning, 0u, __ATOMIC_RELEASE); gScanCursor = 0; post_event(kEvScanDone, nullptr, 0); }
// A scan answered from the cache finishes after the time a short scan takes: answered at once, the daemon asks again straight away.
thread_call_t gCachedScan;
void cached_scan_done(thread_call_param_t, thread_call_param_t) { on_scan_done(); }
void on_connect_done(int result) {
  gAssocResult = result == NW_RES_OK ? kAssocOk : result == NW_RES_BAD_KEY ? kAssocBadKey : result == NW_RES_TIMEOUT ? kAssocTimeout : kAssocFailed;
  log(6, "wifi-net: join finished: module result %d", result);
  if (result == NW_RES_OK) { post_link(true); }
  post_event(kEvAssocDone, nullptr, 0);
  if (result == NW_RES_OK) {
    post_event(kEvSsidChanged, nullptr, 0); post_event(kEvBssidChanged, nullptr, 0);
    nw_wifi_status st; nw_wifi_status_get(&st);
    link_event ev; memset(&ev, 0, sizeof ev); ev.rssi = st.signal_dbm; ev.noise = static_cast<uint16_t>(-95); ev.snr = static_cast<uint16_t>(st.signal_dbm + 95);
    post_event(kEvLinkChanged, &ev, sizeof ev);
  }
}
void on_link_lost(int reason) {
  log(6, "wifi-net: link lost: reason %d", reason);
  post_link(false);
  link_event ev; memset(&ev, 0, sizeof ev); ev.down = 1; ev.involuntary = 1; ev.reason = static_cast<uint32_t>(reason);
  post_event(kEvLinkChanged, &ev, sizeof ev);
}
// What the address configuration exchanges, as it crosses the interface: DHCP messages (type, transaction, offered address) and ARP, the first
// ones of each direction, and per-kind counts for the traffic line.
struct Kinds { uint32_t arp, ip4, dhcp, ip6, other; } gKindsIn, gKindsOut;
void note_frame(const uint8_t *e, size_t n, bool out) {
  Kinds &k = out ? gKindsOut : gKindsIn; const unsigned type = n >= 14 ? (e[12] << 8 | e[13]) : 0; const char *dir = out ? "out" : "in";
  if (type == 0x0806) {
    k.arp++; static unsigned shown[2];
    if (n >= 42 && shown[out]++ < 12) log(6, "wifi-net: arp %s op %u %u.%u.%u.%u asks %u.%u.%u.%u", dir, e[21], e[28], e[29], e[30], e[31], e[38], e[39], e[40], e[41]);
    return; }
  if (type == 0x86dd) { k.ip6++; return; }
  if (type != 0x0800 || n < 34) { k.other++; return; }
  k.ip4++; const uint8_t *ip = e + 14; const unsigned ihl = (ip[0] & 15) * 4;
  if (ip[9] != 17 || n < 14 + ihl + 8) return;
  const uint8_t *udp = ip + ihl; const unsigned sp = udp[0] << 8 | udp[1], dp = udp[2] << 8 | udp[3];
  if (!((sp == 67 || sp == 68) && (dp == 67 || dp == 68))) return;
  k.dhcp++; const uint8_t *b = udp + 8; const size_t bl = n - (b - e); unsigned msg = 0;
  for (size_t o = 240; bl >= 240 && o + 2 <= bl && b[o] != 255; o += b[o] ? 2 + b[o + 1] : 1) if (b[o] == 53 && o + 2 < bl) { msg = b[o + 2]; break; }
  static unsigned shown[2]; if (shown[out]++ < 24 && bl >= 240)
    log(6, "wifi-net: dhcp %s type %u xid %02x%02x%02x%02x flags %02x%02x to %02x:%02x:%02x:%02x:%02x:%02x yiaddr %u.%u.%u.%u", dir, msg, b[4], b[5], b[6], b[7], b[10], b[11],
        e[0], e[1], e[2], e[3], e[4], e[5], b[16], b[17], b[18], b[19]);
  // An offer or acknowledgement: the network settings the server hands out (mask, router, name servers, lease), to tell a server that
  // gives no IPv4 name server from a client that drops one.
  if (out || (msg != 2 && msg != 5)) return;
  char opts[124]; int at = 0;   // fits the 160-byte log line
  for (size_t o = 240; o + 2 <= bl && b[o] != 255 && at >= 0 && at < static_cast<int>(sizeof opts) - 24; o += b[o] ? 2 + b[o + 1] : 1) {
    if (!b[o]) continue;
    const unsigned code = b[o], len = b[o + 1]; const uint8_t *v = b + o + 2;
    if (o + 2 + len > bl) break;
    if ((code == 1 || code == 3 || code == 6 || code == 54) && len >= 4)
      for (unsigned i = 0; i + 4 <= len && at < static_cast<int>(sizeof opts) - 24; i += 4)
        at += snprintf(opts + at, sizeof opts - at, " %u=%u.%u.%u.%u", code, v[i], v[i + 1], v[i + 2], v[i + 3]);
    else if (code == 51 && len == 4) at += snprintf(opts + at, sizeof opts - at, " 51=%u", v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3]);
    else at += snprintf(opts + at, sizeof opts - at, " %u", code);
  }
  log(6, "wifi-net: dhcp %s type %u options%s", dir, msg, opts);
}
void on_rx(const uint8_t *eth, size_t length) {
  if (!gIf || length < 14) return;
  note_frame(eth, length, false);
  // Two bytes ahead of the Ethernet header put the IP header on a 4-byte boundary. The packet may come back as a chain (a frame larger
  // than one cluster, or no large cluster free): each buffer gets only what fits in it. Setting the whole length on the first one (as
  // before) let the stack read past that buffer's end (i469: a data abort in m_copydata reading across a page).
  enum : size_t { Align = 2 };
  mbuf_t m = nullptr;
  if (mbuf_allocpacket(MBUF_DONTWAIT, length + Align, nullptr, &m) || !m) { gRxNoBuffer++; return; }
  if (length > gRxLongest) gRxLongest = static_cast<uint32_t>(length);
  if (mbuf_next(m)) gRxChains++;
  size_t done = 0; bool first = true;
  for (mbuf_t n = m; n; n = mbuf_next(n)) {
    size_t room = mbuf_maxlen(n) - mbuf_leadingspace(n);
    if (first) { if (room < Align + 14) break; mbuf_setdata(n, static_cast<uint8_t *>(mbuf_datastart(n)) + mbuf_leadingspace(n) + Align, 0); room -= Align; }
    const size_t take = length - done < room ? length - done : room;
    memcpy(mbuf_data(n), eth + done, take); mbuf_setlen(n, take); done += take; first = false;
  }
  if (done != length) { gRxNoBuffer++; mbuf_freem(m); return; }
  mbuf_pkthdr_setlen(m, length); mbuf_pkthdr_setrcvif(m, gIf);
  mbuf_pkthdr_setheader(m, mbuf_data(m)); mbuf_adj(m, 14);   // the stack wants the Ethernet header apart from the payload
  struct ifnet_stat_increment_param inc; memset(&inc, 0, sizeof inc); inc.packets_in = 1; inc.bytes_in = static_cast<uint32_t>(length);
  const errno_t e = ifnet_input(gIf, m, &inc);
  if (e) { gRxErrors++; gRxLastError = e; } else gRxFrames++;
}
const struct nw_wifi_events gEvents = { on_scan_done, on_connect_done, on_link_lost, on_rx };

// ---- the interface ----
errno_t if_output(ifnet_t, mbuf_t m) {
  size_t length = mbuf_pkthdr_len(m);
  if (length >= 14 && length <= 2000) {
    uint8_t frame[2000]; mbuf_copydata(m, 0, length, frame);
    note_frame(frame, length, true);
    if (nw_wifi_tx(frame, length)) log(6, "wifi-net: a frame was dropped (queue full)");
    else {ifnet_stat_increment_out(gIf, 1, static_cast<uint32_t>(length), 0); sheng_wifi_wake();}
  }
  mbuf_freem(m);
  return 0;
}
void if_event_stub(ifnet_t, const struct kev_msg *) {}

// ---- Apple80211 ----
void fill_channel(channel &c, uint32_t mhz) {
  c.version = 1;
  if (mhz >= 4900) { c.channel = (mhz - 5000) / 5; c.flags = 0x2 | 0x10; } else { c.channel = mhz == 2484 ? 14 : (mhz - 2407) / 5; c.flags = 0x2 | 0x8; }
}
void fill_result(scan_result &r, const nw_bss_info &b) {
  memset(&r, 0, sizeof r); r.version = 1; fill_channel(r.ch, b.freq_mhz); r.noise_known = 1; r.noise = -95; r.rssi = b.signal_dbm; r.snr = b.signal_dbm + 95;
  r.beacon_interval = b.beacon_int; r.capability = b.caps; memcpy(r.bssid, b.bssid, 6); r.ssid_length = b.ssid_len; memcpy(r.ssid, b.ssid, 32);
  r.age_ms = b.age_ms; r.elements_length = b.ie_len > sizeof r.elements ? sizeof r.elements : b.ie_len; memcpy(r.elements, b.ie, r.elements_length);
  r.rate_count = 0;
}
int copy_out(const apple80211req &rq, const void *src, size_t size) {
  if (rq.req_len == 0 || !rq.req_data) return EINVAL;
  return copyout(src, rq.req_data, rq.req_len < size ? rq.req_len : size);
}
// A get that gives no buffer takes a single value in req_val; one with a buffer gets {version, value}.
int answer_value(apple80211req &rq, uint32_t value) {
  if (rq.req_len == 0 || !rq.req_data) { rq.req_val = static_cast<int32_t>(value); return 0; }
  const uint32_t w[2] = { 1, value }; return copyout(w, rq.req_data, rq.req_len < sizeof w ? rq.req_len : sizeof w);
}
// The raw bytes, their length in req_len.
int answer_bytes(apple80211req &rq, const void *src, uint32_t size) {
  if (!rq.req_data) return EINVAL;
  const uint32_t n = rq.req_len < size ? rq.req_len : size; rq.req_len = size; return copyout(src, rq.req_data, n);
}
int copy_in(const apple80211req &rq, void *dst, size_t size) {
  if (!rq.req_data) return EINVAL;
  memset(dst, 0, size);
  return copyin(rq.req_data, dst, rq.req_len < size ? rq.req_len : size);
}
unsigned gResultsShown;
// The regulatory country as the access points announce it (802.11d: the Country element, id 7, of the strongest network that has one), else "XX".
void country(char cc[3]) {
  cc[0] = 'X'; cc[1] = 'X'; cc[2] = 0; int best = -200; nw_bss_info b;
  for (int i = 0, n = nw_wifi_bss_count(); i < n; ++i) {
    if (nw_wifi_bss_get(i, &b) || b.signal_dbm <= best) continue;
    for (unsigned o = 0; o + 2 <= b.ie_len && o + 2 + b.ie[o + 1] <= b.ie_len; o += 2 + b.ie[o + 1])
      if (b.ie[o] == 7 && b.ie[o + 1] >= 2 && b.ie[o + 2] >= 'A' && b.ie[o + 2] <= 'Z' && b.ie[o + 3] >= 'A' && b.ie[o + 3] <= 'Z') {
        cc[0] = static_cast<char>(b.ie[o + 2]); cc[1] = static_cast<char>(b.ie[o + 3]); best = b.signal_dbm; break; }
  }
}
int map_state(unsigned s) { switch (s) { case NW_STA_SCANNING: return 1; case NW_STA_AUTH: return 2; case NW_STA_ASSOC: case NW_STA_KEYS: return 3; case NW_STA_UP: return 4; default: return 0; } }

int apple80211(bool set, apple80211req &rq) {
  nw_wifi_status st; nw_wifi_status_get(&st);
  const int type = rq.req_type;
  // (not on the stack: the largest answer is 4824 bytes)
  union Out { uint8_t raw[5120]; channels_list chans; scan_result res; };
  Out *outp = static_cast<Out *>(IOMallocData(sizeof(Out)));
  if (!outp) return ENOMEM;
  memset(outp, 0, sizeof(Out));
  struct Free { Out *p; ~Free() { IOFreeData(p, sizeof(Out)); } } freeOut{outp};
  Out &out = *outp;
  uint32_t *w = reinterpret_cast<uint32_t *>(out.raw);
  if (set) {
    switch (type) {
    case kPower: { uint32_t in[6]; if (copy_in(rq, in, sizeof in)) return EFAULT; gUserJoin = true; gPower = in[2] != 0; log(6, "wifi-net: power set %u", in[2]); if (!gPower) nw_wifi_disconnect(); post_event(kEvPowerChanged, nullptr, 0); return 0; }
    case kScanReq: case kScanReqMultiple: {
      uint8_t buf[0x80]; if (copy_in(rq, buf, sizeof buf)) return EFAULT;
      uint32_t ssidLength; memcpy(&ssidLength, buf + 16, 4); const uint8_t *ssid = buf + 20;
      // joining or joined: a scan takes the radio off channel for seconds and the access point's beacons and handshake frames are missed, so
      // the daemon gets the last results instead
      if (st.state >= NW_STA_AUTH && nw_wifi_bss_count() > 0) {
        if (!gCachedScan) gCachedScan = thread_call_allocate(cached_scan_done, nullptr);
        static unsigned answered; if (++answered <= 8 || answered % 64 == 0) log(6, "wifi-net: scan %u while joined: answered from the cache", answered);
        gScanning = 1; gScanCursor = 0; gResultsShown = 24; uint64_t at; clock_interval_to_deadline(2000, kMillisecondScale, &at); thread_call_enter_delayed(gCachedScan, at); return 0; }
      gScanning = 1; gScanCursor = 0; gResultsShown = 0; nw_wifi_scan(ssidLength && ssidLength <= 32 ? ssid : nullptr, ssidLength <= 32 ? ssidLength : 0); return 0; }
    case kAssociate: {
      assoc_request a; if (copy_in(rq, &a, sizeof a)) return EFAULT;
      if (a.ssid_length > 32) return EINVAL;
      const bool hasBssid = (a.bssid[0] | a.bssid[1] | a.bssid[2] | a.bssid[3] | a.bssid[4] | a.bssid[5]) != 0;
      const bool hasKey = a.key_length != 0 && a.key_length <= 64; char pass[65]; memset(pass, 0, sizeof pass); if (hasKey) memcpy(pass, a.key, a.key_length);
      // cipher 6: the daemon already derived the pairwise master key (32 bytes); the module takes it as 64 hex digits
      unsigned keyLength = a.key_length;
      if (hasKey && a.key_cipher == 6 && a.key_length == 32) { static const char hex[] = "0123456789abcdef"; for (int i = 0; i < 32; i++) { pass[2 * i] = hex[a.key[i] >> 4]; pass[2 * i + 1] = hex[a.key[i] & 15]; } keyLength = 64; }
      gAssocResult = kAssocFailed; gUserJoin = true;
      log(6, "wifi-net: join: ssid length %u, bssid given %d, key length %u cipher %u", a.ssid_length, hasBssid, a.key_length, a.key_cipher);
      gKeyed = hasKey; int r = nw_wifi_connect(a.ssid, a.ssid_length, hasBssid ? a.bssid : nullptr, hasKey ? pass : nullptr, hasKey ? keyLength : 0);
      memset(pass, 0, sizeof pass); memset(&a, 0, sizeof a);
      return r ? EINVAL : 0; }
    case kDisassociate: case kDeauth: gUserJoin = true; log(6, "wifi-net: leave"); nw_wifi_disconnect(); post_link(false); return 0;
    case kScanCacheClear: case kLocale: case kCountryCode: case kPowerSave: case kAuthType: case kCipherKey: return 0;
    default: return EOPNOTSUPP;
    }
  }
  switch (type) {
  case kPower: w[0] = 1; w[1] = 1; w[2] = gPower ? 1 : 0; return copy_out(rq, w, 24);
  case kState: return answer_value(rq, static_cast<uint32_t>(gPower ? map_state(st.state) : 0));
  case kOpMode: return answer_value(rq, 1);   // station
  case kAuthType: w[0] = 1; w[1] = 1; w[2] = gKeyed ? 0x8 : 0; return copy_out(rq, w, 12);   // {version, lower: open system, upper: WPA2 personal or none}
  case kPowerSave: return answer_value(rq, 0);
  case kCardCaps: {   // a bitmap, bit n in byte n / 8: AES, AES-CCM, power management, short slot and preamble, WPA2, WME, short guard interval at 20 MHz
    static const uint8_t has[] = { 2, 3, 6, 9, 10, 14, 17, 19 };
    for (uint8_t b : has) out.raw[b / 8] |= static_cast<uint8_t>(1u << (b % 8));
    return answer_bytes(rq, out.raw, 12); }
  case kPhyMode: w[0] = 1; w[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40; w[2] = st.state == NW_STA_UP ? 0x8 : 0; return copy_out(rq, w, 12);
  case kSsid: if (st.state < NW_STA_ASSOC) return ENOENT; return answer_bytes(rq, st.ssid, st.ssid_len);
  case kBssid: if (st.state < NW_STA_ASSOC) return ENOENT; return answer_bytes(rq, st.bssid, 6);
  case kChannel: if (st.state < NW_STA_AUTH) return ENOENT; w[0] = 1; fill_channel(*reinterpret_cast<channel *>(out.raw + 4), st.freq_mhz); return copy_out(rq, w, 16);
  case kRssi: case kNoise: {   // {version, radios, unit (0: dBm), per chain[4], aggregate, extension per chain[4], its aggregate}
    if (type == kRssi && st.state != NW_STA_UP) return ENOENT;
    const uint32_t v = static_cast<uint32_t>(type == kRssi ? static_cast<int32_t>(st.signal_dbm) : -95);
    w[0] = 1; w[1] = 1; w[2] = 0; for (int i = 3; i < 13; i++) w[i] = v; return copy_out(rq, w, 13 * 4); }
  case kRate: return answer_value(rq, st.state == NW_STA_UP ? 54 : 0);
  case kTxPower: return answer_value(rq, 15);
  case kDriverVersion: case kHardwareVersion: { const char *v = type == kDriverVersion ? "nativewifi 1" : "WCN7850"; strlcpy(reinterpret_cast<char *>(out.raw), v, 256); return copy_out(rq, out.raw, 256); }
  case kCountryCode: { char cc[3]; country(cc); return answer_bytes(rq, cc, 3); }   // the two letters and a NUL (the daemon asks with a 3-byte buffer)
  case kAssociateResult: return answer_value(rq, static_cast<uint32_t>(gAssocResult));
  case kSupportedChannels: case kHwSupportedChannels: {
    out.chans.version = 1; unsigned n = 0;
    static const uint16_t known[] = { 2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 5180, 5200, 5220, 5240, 5745, 5765, 5785, 5805, 5825 };
    for (uint16_t f : known) { fill_channel(*reinterpret_cast<channel *>(&out.chans.list[n]), f); out.chans.list[n].flags |= 0x80; n++; }
    out.chans.count = n; return copy_out(rq, &out.chans, sizeof out.chans); }
  case kScanResult: {
    nw_bss_info b; const int count = nw_wifi_bss_count();
    if (gScanCursor >= static_cast<unsigned>(count) || nw_wifi_bss_get(static_cast<int>(gScanCursor), &b)) { gScanCursor = 0; return EIO; }   // after the last record
    gScanCursor++; fill_result(out.res, b);
    if (gResultsShown < 24) { ++gResultsShown; log(6, "wifi-net: scan result %u: %u MHz, %d dBm, %.*s", gScanCursor, b.freq_mhz, b.signal_dbm, b.ssid_len, reinterpret_cast<const char *>(b.ssid)); }
    return copy_out(rq, &out.res, sizeof out.res); }
  case kCurrentNetwork: {
    if (st.state < NW_STA_ASSOC) return ENOENT;
    nw_bss_info b; const int count = nw_wifi_bss_count();
    for (int i = 0; i < count; i++) if (!nw_wifi_bss_get(i, &b) && !memcmp(b.bssid, st.bssid, 6)) { fill_result(out.res, b); return copy_out(rq, &out.res, sizeof out.res); }
    return ENOENT; }
  default: return EOPNOTSUPP;
  }
}

errno_t if_ioctl(ifnet_t, unsigned long cmd, void *data) {
  switch (cmd) {
  case SIOCGA80211: case SIOCSA80211: {
    apple80211req *rq = static_cast<apple80211req *>(data);
    const uint32_t len = rq->req_len; const int32_t val = rq->req_val;
    int e = apple80211(cmd == SIOCSA80211, *rq);
    // every request with its answer, for the first few hundred; then a type's first one only
    static unsigned shown; const int type = rq->req_type; const bool first = type >= 0 && type < 256 && !(gLogged[type / 32] & (1u << (type % 32)));
    if (type >= 0 && type < 256) gLogged[type / 32] |= 1u << (type % 32);
    if (shown < 300 || first) { ++shown; log(6, "wifi-net: request %u: type %d %s len %u val %d -> %d (len %u val %d)", shown, type, cmd == SIOCSA80211 ? "set" : "get", len, val, e, rq->req_len, rq->req_val); }
    return e;
  }
  case SIOCGIFMEDIA: {
    struct ifmediareq *m = static_cast<struct ifmediareq *>(data);
    nw_wifi_status st; nw_wifi_status_get(&st);
    // the framework lists wireless interfaces by this media type
    m->ifm_current = IFM_IEEE80211 | IFM_AUTO; m->ifm_mask = 0; m->ifm_status = IFM_AVALID | (st.state == NW_STA_UP ? IFM_ACTIVE : 0);
    m->ifm_active = IFM_IEEE80211 | IFM_AUTO; m->ifm_count = 1;
    return 0;
  }
  case SIOCSIFFLAGS: case SIOCSIFMTU: case SIOCADDMULTI: case SIOCDELMULTI: case SIOCSIFADDR: case SIOCSIFMEDIA: return 0;
  default: { static unsigned said; if (said++ < 16) log(6, "wifi-net: interface request %lx not handled", cmd); return EOPNOTSUPP; }
  }
}

// ---- registry entries (see the top of the file in the build: the classes are registered late) ----
}  // namespace

class IO80211Controller : public IOService { OSDeclareDefaultStructors(IO80211Controller); };
OSDefineMetaClassAndStructors(IO80211Controller, IOService);
class IONetworkInterface : public IOService { OSDeclareDefaultStructors(IONetworkInterface); };
OSDefineMetaClassAndStructors(IONetworkInterface, IOService);
// The system's network configuration and wireless daemon tell a wireless interface by its class name.
class IO80211Interface : public IONetworkInterface {
  OSDeclareDefaultStructors(IO80211Interface);
 public:
  IOReturn newUserClient(task_t, void *, UInt32 type, OSDictionary *, IOUserClient **) APPLE_KEXT_OVERRIDE {
    static unsigned said; if (said++ < 4) log(6, "wifi-net: a user client of type %u was asked for: none offered (ioctl path)", static_cast<unsigned>(type));
    return kIOReturnUnsupported;
  }
};
OSDefineMetaClassAndStructors(IO80211Interface, IONetworkInterface);

// The network configuration's interface namer waits for a service of this class, opens it and only then publishes that IOKit is quiet;
// everything that lists interfaces (the wireless daemon among them) waits for that. Interfaces here are named when they are made, so the
// commands it sends (as properties of its connection) are only logged.
class IONetworkStackUserClient : public IOUserClient {
  OSDeclareDefaultStructors(IONetworkStackUserClient);
 public:
  IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE {
    static unsigned said;
    if (said++ < 16) {
      char text[200] = "?";
      if (OSSerialize *s = OSSerialize::withCapacity(512)) { if (properties && properties->serialize(s)) strlcpy(text, s->text(), sizeof text); s->release(); }
      log(6, "wifi-net: network stack command %s", text);
    }
    return kIOReturnSuccess;
  }
  IOReturn clientClose() APPLE_KEXT_OVERRIDE { terminate(); return kIOReturnSuccess; }
};
OSDefineMetaClassAndStructors(IONetworkStackUserClient, IOUserClient);
class IONetworkStack : public IOService {
  OSDeclareDefaultStructors(IONetworkStack);
 public:
  IOReturn newUserClient(task_t task, void *security, UInt32 type, OSDictionary *, IOUserClient **handler) APPLE_KEXT_OVERRIDE {
    IONetworkStackUserClient *client = OSTypeAlloc(IONetworkStackUserClient);
    if (!client || !client->initWithTask(task, security, type)) { OSSafeReleaseNULL(client); return kIOReturnNoMemory; }
    if (!client->attach(this)) { client->release(); return kIOReturnError; }
    if (!client->start(this)) { client->detach(this); client->release(); return kIOReturnError; }
    log(6, "wifi-net: the network stack was opened (type %u)", static_cast<unsigned>(type));
    *handler = client; return kIOReturnSuccess;
  }
};
OSDefineMetaClassAndStructors(IONetworkStack, IOService);

namespace {

IOService *publish(IOService *service, IOService *provider, const char *name) {
  if (!service || !service->attach(provider)) { OSSafeReleaseNULL(service); return nullptr; }
  service->setName(name);
  if (!service->start(provider)) { service->detach(provider); service->release(); return nullptr; }
  service->registerService();
  return service;
}
void publish_registry(const char *ifname) {
  IOService *platform = IOService::getPlatform();
  if (IONetworkStack *stack = OSTypeAlloc(IONetworkStack)) {
    if (!stack->init() || !publish(stack, platform, "IONetworkStack")) log(3, "wifi-net: the network stack service did not start"); else stack->release(); }
  IO80211Controller *controller = OSTypeAlloc(IO80211Controller);
  IO80211Interface *wlan = OSTypeAlloc(IO80211Interface);
  if (!platform || !controller || !wlan || !controller->init() || !wlan->init()) { OSSafeReleaseNULL(controller); OSSafeReleaseNULL(wlan); log(3, "wifi-net: registry objects not made"); return; }
  if (OSData *mac = OSData::withBytes(gMac, 6)) { controller->setProperty("IOMACAddress", mac); mac->release(); }
  controller->setProperty("IOBuiltin", true);
  controller = static_cast<IO80211Controller *>(publish(controller, platform, "ShengWLANController"));
  if (!controller) { wlan->release(); log(3, "wifi-net: the controller did not start"); return; }
  wlan->setProperty("IOInterfaceName", ifname); wlan->setProperty("BSD Name", ifname); wlan->setProperty("IOInterfaceNamePrefix", "en");
  if (OSNumber *n = OSNumber::withNumber(static_cast<unsigned long long>(IFT_ETHER), 32)) { wlan->setProperty("IOInterfaceType", n); n->release(); }
  if (OSNumber *n = OSNumber::withNumber(0ULL, 32)) { wlan->setProperty("IOInterfaceUnit", n); n->release(); }
  wlan->setProperty("IOBuiltin", true); wlan->setProperty("IOPrimaryInterface", true); wlan->setProperty("IO80211InterfaceRole", "Infrastructure");
  if (!publish(wlan, controller, "ShengWLAN")) log(3, "wifi-net: the interface service did not start"); else wlan->release();
  controller->release();
}

// en0's IPv4 addresses and packet counts, for the log.
void log_traffic() {
  if (!gIf) return;
  char text[64] = "none"; ifaddr_t *list = nullptr;
  if (!ifnet_get_address_list_family(gIf, &list, AF_INET) && list) {
    if (list[0]) { struct sockaddr_in a; memset(&a, 0, sizeof a);
      if (!ifaddr_address(list[0], reinterpret_cast<struct sockaddr *>(&a), sizeof a)) { const uint8_t *b = reinterpret_cast<const uint8_t *>(&a.sin_addr); snprintf(text, sizeof text, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]); } }
    ifnet_free_address_list(list); }
  struct ifnet_stats_param stats; memset(&stats, 0, sizeof stats); ifnet_stat(gIf, &stats);
  char tx[160]; nw_wifi_tx_reasons(tx, sizeof tx);
  log(6, "wifi-net: en0 address %s, packets in %llu out %llu; frames to the stack %u, refused %u (last %d)", text, static_cast<unsigned long long>(stats.packets_in),
      static_cast<unsigned long long>(stats.packets_out), gRxFrames, gRxErrors, gRxLastError);
  log(6, "wifi-net: tx %s", tx);
  log(6, "wifi-net: in arp %u ip4 %u dhcp %u ip6 %u other %u", gKindsIn.arp, gKindsIn.ip4, gKindsIn.dhcp, gKindsIn.ip6, gKindsIn.other);
  log(6, "wifi-net: out arp %u ip4 %u dhcp %u ip6 %u other %u", gKindsOut.arp, gKindsOut.ip4, gKindsOut.dhcp, gKindsOut.ip6, gKindsOut.other);
  log(6, "wifi-net: input stalls %u, kicks %u, cleared after a kick %u, longest %u s", gStall.count, gStall.kicks, gStall.cleared, gStall.longest);
  log(6, "wifi-net: rx no buffer %u, chained %u, longest frame %u", gRxNoBuffer, gRxChains, gRxLongest);
}

// The interface's input thread made runnable and then never run while frames kept coming (i468: from 231 s on, a priority 81 thread with no
// stack, so the DHCP answers never reached the client and the lease was lost). Checked every second: on a stall where that thread is, and
// every processor asked to check its ASTs again, which is what a preemption signal that never arrived would have done.
void check_input() {
  static uint64_t lastIn; static uint32_t lastFrames; static unsigned stalled;
  struct ifnet_stats_param stats; memset(&stats, 0, sizeof stats); ifnet_stat(gIf, &stats);
  const bool moving = stats.packets_in != lastIn, offered = gRxFrames != lastFrames;
  lastIn = stats.packets_in; lastFrames = gRxFrames;
  if (moving) {
    if (stalled) { if (gStall.kicked) gStall.cleared++; log(6, "wifi-net: input moves again after %u s%s", stalled, gStall.kicked ? " and a kick" : ""); }
    stalled = 0; gStall.kicked = false; return;
  }
  if (!offered) return;   // nothing new handed over: an idle link, not a stall
  if (++stalled > gStall.longest) gStall.longest = stalled;
  if (stalled == 1) gStall.count++;
  if (stalled > 3) return;   // reported and kicked twice; the per-ten-seconds line keeps the counts
  char where[240];
  if (boot_kthread_report("dlil_input_en0", where, sizeof where)) log(6, "wifi-net: input thread not found");
  else for (size_t at = 0, n = strlen(where); at < n; at += 90) log(6, "wifi-net: input thread %u: %.90s", static_cast<unsigned>(at / 90), where + at);
  if (stalled < 2) return;   // one quiet second can be a slow thread
  char cpus[240]; const unsigned sent = boot_cpu_kick(cpus, sizeof cpus); gStall.kicks++; gStall.kicked = true;
  for (size_t at = 0, n = strlen(cpus); at < n; at += 90) log(6, "wifi-net: kick %u (%u sent) %u: %.90s", gStall.kicks, sent, static_cast<unsigned>(at / 90), cpus + at);
}

// IOKit's busy services: the network configuration waits for IOKit to be quiet before it names interfaces, and the wireless daemon waits
// for that. Logged each round until the registry is quiet once; leaves first (a busy service whose children are all quiet).
void log_busy() {
  static bool quietSeen; if (quietSeen) return;
  IOService *root = IOService::getServiceRoot(); if (!root) return;
  const UInt32 busy = root->getBusyState();
  if (!busy) { quietSeen = true; log(6, "wifi-net: IOKit is quiet"); return; }
  char line[110]; int at = snprintf(line, sizeof line, "wifi-net: IOKit busy %u:", static_cast<unsigned>(busy)); unsigned shown = 0;
  if (IORegistryIterator *it = IORegistryIterator::iterateOver(gIOServicePlane, kIORegistryIterateRecursively)) {
    while (OSObject *o = it->getNextObject()) {
      IOService *svc = OSDynamicCast(IOService, o); if (!svc || !svc->getBusyState()) continue;
      bool busyChild = false;
      if (OSIterator *ch = svc->getChildIterator(gIOServicePlane)) { while (OSObject *c = ch->getNextObject()) { IOService *cs = OSDynamicCast(IOService, c); if (cs && cs->getBusyState()) { busyChild = true; break; } } ch->release(); }
      if (busyChild) continue;
      if (at < static_cast<int>(sizeof line) - 2) at += snprintf(line + at, sizeof line - at, " %s(%s)", svc->getName(), svc->getMetaClass()->getClassName());
      if (++shown >= 6) break;
    }
    it->release();
  }
  log(6, "%s", line);
}

// While joined, every 10 s: the address, the packet counts and the kinds of traffic (whoever made the join).
void trafficLog(void *, wait_result_t) {
  for (unsigned second = 1;; ++second) {
    IOSleep(1000); nw_wifi_status st; nw_wifi_status_get(&st);
    if (st.state != NW_STA_UP) continue;
    check_input(); if (second % 10 == 0) log_traffic();
  }
}

// Unattended testing: when the firmware container holds a file named join.txt (the network name on the first line, the key on the second) the
// board joins that network by itself once a scan has found it, with the call the wireless daemon's join makes. Without the file nothing happens.
void autoJoin(void *, wait_result_t) {
  char ssid[33] = {}, psk[65] = {};
  const uint8_t *file = nullptr; size_t length = 0;
  if (sheng_wifi_file("join.txt", &file, &length) || !file || !length) { thread_terminate(current_thread()); return; }
  { size_t i = 0, o = 0; while (i < length && file[i] != '\n' && o < 32) ssid[o++] = static_cast<char>(file[i++]);
    while (i < length && file[i] != '\n') ++i; ++i; o = 0;
    while (i < length && file[i] != '\n' && o < 64) psk[o++] = static_cast<char>(file[i++]); }
  const bool hasPsk = psk[0] != 0; const size_t sl = strlen(ssid), pl = strlen(psk);
  log(6, "wifi-net: auto join of a network with a %zu byte name and a %zu byte key", sl, pl);
  for (unsigned round = 0; round < 40; ++round) {
    IOSleep(3000);
    log_busy();
    if (gUserJoin) { log(6, "wifi-net: auto join stops: the wireless daemon took over"); break; }
    nw_wifi_status st; nw_wifi_status_get(&st);
    if (st.state == NW_STA_UP) continue;   // the traffic thread logs the address and the packets
    if (st.state >= NW_STA_AUTH) continue;
    bool found = false; nw_bss_info b;
    for (int i = 0, n = nw_wifi_bss_count(); i < n; ++i)
      if (!nw_wifi_bss_get(i, &b) && b.ssid_len == sl && !memcmp(b.ssid, ssid, sl)) { found = true; break; }
    log(6, "wifi-net: auto join round %u: %d networks seen, wanted one %s", round, nw_wifi_bss_count(), found ? "found" : "not found");
    if (found) { gKeyed = hasPsk; const int r = nw_wifi_connect(reinterpret_cast<const uint8_t *>(ssid), sl, nullptr, hasPsk ? psk : nullptr, hasPsk ? pl : 0); log(6, "wifi-net: auto join requested: %d", r); }
    else nw_wifi_scan(nullptr, 0);
  }
  memset(psk, 0, sizeof psk);
  thread_terminate(current_thread());
}

}  // namespace

// Called once the module has a station interface and a hardware address.
extern "C" void sheng_wifi_net_start(const uint8_t mac[6]) {
  if (gIf) return;
  memcpy(gMac, mac, 6);
  gGrp = lck_grp_alloc_init("sheng-wifi-net", LCK_GRP_ATTR_NULL); gLock = lck_mtx_alloc_init(gGrp, LCK_ATTR_NULL);
  struct ifnet_init_eparams p; memset(&p, 0, sizeof p);
  p.ver = IFNET_INIT_CURRENT_VERSION; p.len = sizeof p; p.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO; p.name = "en"; p.unit = 0; p.family = IFNET_FAMILY_ETHERNET; p.subfamily = IFNET_SUBFAMILY_WIFI; p.type = IFT_ETHER;
  p.output = if_output; p.demux = ether_demux; p.add_proto = ether_add_proto; p.del_proto = ether_del_proto; p.check_multi = ether_check_multi; p.framer = ether_frameout;
  p.ioctl = if_ioctl; p.event = if_event_stub; static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }; p.broadcast_addr = bcast; p.broadcast_len = 6;
  // Without NX_NOAUTO the stack puts a compatibility nexus in front of a Wi-Fi "en" interface, and its input no longer goes through the
  // interface's own input thread.
  ifnet_t ifp = nullptr; errno_t e = ifnet_allocate_extended(&p, &ifp);
  if (e || !ifp) { log(3, "wifi-net: ifnet allocation failed: %d", e); return; }
  ifnet_set_mtu(ifp, 1500); ifnet_set_flags(ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST, 0xffff); ifnet_set_baudrate(ifp, 54000000ULL);
  ifnet_set_hdrlen(ifp, 14); ifnet_set_addrlen(ifp, 6); ifnet_set_eflags(ifp, IFEF_ENQUEUE_MULTI, IFEF_ENQUEUE_MULTI);
  e = ifnet_attach(ifp, nullptr);
  if (e) { log(3, "wifi-net: ifnet attach failed: %d", e); ifnet_release(ifp); return; }
  ifnet_set_lladdr(ifp, mac, 6);
  // A driver of its own interface attaches the packet filter itself: the DHCP client sends through it while the interface has no address.
  e = bpf_attach(ifp, DLT_EN10MB, 14, nullptr, nullptr); if (e) log(3, "wifi-net: packet filter attach failed: %d", e);
  gIf = ifp; ifnet_set_flags(ifp, IFF_UP, IFF_UP);
  nw_wifi_set_events(&gEvents);
  publish_registry("en0");
  // sheng-wifi-aj=0 leaves joining to Settings even when the container holds join.txt.
  { uint32_t aj = 1; PE_parse_boot_argn("sheng-wifi-aj", &aj, sizeof aj);
    thread_t t = nullptr; if (aj && kernel_thread_start(autoJoin, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
    t = nullptr; if (kernel_thread_start(trafficLog, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t); }
  log(6, "wifi-net: en0 is up, hardware address %02x:%02x:%02x:%02x:%02x:%02x, capabilities 0x%x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
      ifnet_capabilities_enabled(ifp));
}
