#!/usr/bin/env python3
"""Exercise production WifiNet receive batching with an owning mbuf test sink."""
from pathlib import Path
import hashlib
import json
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / "package/WifiNet.cpp").read_text()
production = "void release_rx_spares()" + source.split("void release_rx_spares()", 1)[1].split("const struct nw_wifi_events gEvents", 1)[0]
out = root / "build/wifi-rx"
out.mkdir(parents=True, exist_ok=True)
prefix = r'''
#include <array>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
struct Mbuf {
  std::array<uint8_t, 2048> data{};
  size_t offset{}, length{}, packet_length{};
  Mbuf *next{}, *nextpkt{};
  void *header{};
};
using mbuf_t = Mbuf *; using errno_t = int;
using nw_packet_reader = int (*)(void *, size_t, void *, size_t);
static int gIf = 1;
static uint32_t gRxFrames, gRxErrors; static int gRxLastError;
static uint32_t gRxNoBuffer, gRxChains, gRxLongest;
static mbuf_t gRxHead, gRxTail;
static mbuf_t gRxSpare;
static uint32_t gRxBulkAllocations, gRxAllocRetries;
static uint32_t gRxPendingPackets, gRxPendingBytes, gRxBatches, gRxLargestBatch;
static unsigned live, input_calls, reported_packets, reported_bytes;
static bool fail_allocation; static int input_error;
static std::vector<std::vector<uint8_t>> delivered;
enum { MBUF_DONTWAIT = 1 };
struct ifnet_stat_increment_param { uint32_t packets_in{}, bytes_in{}; };
static void note_frame(const uint8_t *, size_t, bool) {}
static void mbuf_freem(mbuf_t m) {
  while (m) { auto next = m->next; delete m; live--; m = next; }
}
static int mbuf_allocpacket(int, size_t size, void *, mbuf_t *out) {
  if (fail_allocation) return ENOMEM;
  *out = nullptr; mbuf_t *link = out;
  while (size) { *link = new Mbuf; live++; size -= size < 2048 ? size : 2048; link = &(*link)->next; }
  return 0;
}
static int mbuf_allocpacket_list(unsigned count, int how, size_t size, unsigned *chunks, mbuf_t *out) {
  assert(count == 16 && size == 2048 && *chunks == 1);
  if (fail_allocation) return ENOMEM;
  *out = nullptr; mbuf_t *next = out;
  for (unsigned i = 0; i < count; i++) { assert(!mbuf_allocpacket(how, size, nullptr, next)); next = &(*next)->nextpkt; }
  return 0;
}
static mbuf_t mbuf_nextpkt(mbuf_t m) { return m->nextpkt; }
static mbuf_t mbuf_next(mbuf_t m) { return m->next; }
static size_t mbuf_maxlen(mbuf_t m) { return m->data.size(); }
static size_t mbuf_leadingspace(mbuf_t m) { return m->offset; }
static void *mbuf_datastart(mbuf_t m) { return m->data.data(); }
static void *mbuf_data(mbuf_t m) { return m->data.data() + m->offset; }
static void mbuf_setdata(mbuf_t m, void *p, size_t n) { m->offset = static_cast<uint8_t *>(p) - m->data.data(); m->length = n; }
static void mbuf_setlen(mbuf_t m, size_t n) { assert(m->offset + n <= m->data.size()); m->length = n; }
static void mbuf_pkthdr_setlen(mbuf_t m, size_t n) { m->packet_length = n; }
static void mbuf_pkthdr_setrcvif(mbuf_t, int) {}
static void mbuf_pkthdr_setheader(mbuf_t m, void *p) { m->header = p; }
static void mbuf_setnextpkt(mbuf_t m, mbuf_t next) { m->nextpkt = next; }
static void mbuf_adj(mbuf_t m, size_t n) {
  m->packet_length -= n;
  for (; m && n; m = m->next) { size_t take = n < m->length ? n : m->length; m->offset += take; m->length -= take; n -= take; }
  assert(!n);
}
static int ifnet_input(int, mbuf_t m, const ifnet_stat_increment_param *stats) {
  input_calls++; reported_packets += stats->packets_in; reported_bytes += stats->bytes_in;
  unsigned packets = 0, bytes = 0;
  while (m) {
    auto next = m->nextpkt;
    auto h = static_cast<const uint8_t *>(m->header);
    std::vector<uint8_t> frame(h, h + 14);
    assert((reinterpret_cast<uintptr_t>(mbuf_data(m)) & 3) == 0);
    for (auto p = m; p; p = p->next) {
      auto data = static_cast<const uint8_t *>(mbuf_data(p)); frame.insert(frame.end(), data, data + p->length);
    }
    assert(frame.size() == m->packet_length + 14); bytes += frame.size(); packets++;
    delivered.push_back(frame); mbuf_freem(m); m = next;
  }
  assert(packets == stats->packets_in && bytes == stats->bytes_in);
  return input_error;
}
'''
tests = r'''
static std::vector<uint8_t> frame(unsigned size, uint8_t salt) {
  std::vector<uint8_t> data(size); for (unsigned i = 0; i < size; i++) data[i] = uint8_t(i ^ salt); return data;
}
int main() {
  auto a = frame(1500, 0x5a), b = frame(14, 0x99), c = frame(4096, 0x27);
  for (unsigned i = 0; i < 32; i++) on_rx(a.data(), a.size());
  assert(!input_calls && gRxPendingPackets == 32 && gRxBulkAllocations == 2); flush_rx();
  assert(input_calls == 1 && reported_packets == 32 && gRxFrames == 32 && !live);
  for (const auto &packet : delivered) assert(packet == a);
  delivered.clear();
  for (unsigned i = 0; i < 64; i++) on_rx(b.data(), b.size());
  assert(input_calls == 2 && gRxLargestBatch == 64 && !live); flush_rx(); assert(input_calls == 2);
  for (const auto &packet : delivered) assert(packet == b);
  delivered.clear(); on_rx(c.data(), c.size()); flush_rx();
  assert(delivered.size() == 1 && delivered[0] == c && gRxLongest == c.size() && !live);
  fail_allocation = true; on_rx(a.data(), a.size()); fail_allocation = false;
  assert(gRxNoBuffer == 1 && !gRxHead && !live);
  on_rx(a.data(), a.size());
  assert(gRxSpare && live == 16);
  release_rx_spares(); fail_allocation = true;
  on_rx(a.data(), a.size()); fail_allocation = false;
  assert(gRxAllocRetries == 1 && gRxNoBuffer == 2 && !gRxSpare && !gRxHead && !live);
  const auto accepted = gRxFrames; input_error = EIO;
  for (unsigned i = 0; i < 3; i++) on_rx(a.data(), a.size());
  flush_rx(); assert(gRxErrors == 3 && gRxLastError == EIO && gRxFrames == accepted && !live);
  const auto calls = input_calls; on_rx(b.data(), 13); gIf = 0; on_rx(a.data(), a.size()); flush_rx();
  assert(input_calls == calls && !gRxHead && !live);
  puts("PASS: production RX batching, 16-packet refill, spare cleanup, pressure flush/retry, packet chains, alignment and input-error ownership");
}
'''
(out / "test.cpp").write_text(prefix + production + tests)
subprocess.run(["clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                "-fsanitize=address,undefined", str(out / "test.cpp"), "-o", str(out / "test")], check=True)
result = subprocess.run([str(out / "test")], check=True, text=True, capture_output=True)
print(result.stdout, end="")
(out / "report.json").write_text(json.dumps({
    "passed": True, "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
    "mbuf_and_ifnet_substituted": True, "hardware_verified": False,
    "output": result.stdout,
}, indent=2) + "\n")
