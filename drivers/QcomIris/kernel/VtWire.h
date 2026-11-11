// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The wire format of VideoToolbox's paravirtualization messages (VtHost.cpp), as iPadOS 27 / macOS 27 VideoToolbox writes
// and reads them (VTParavirtualizationCreateMessageAndByteStream, VTParavirtualizationMessageAppend*/Get*), recorded both ways
// by a loopback of its own guest and host halves. Big endian throughout.
//   A message: top-level FigAtoms (32-bit size, FourCC type, body). The first is 'vtpv', 48 bytes: the message type, flags
//   (1 a guest's request, 4 the host's reply, 2 the host's request, 8 the guest's reply, 0 one way), the guest UUID (16 bytes,
//   the session), the request ID (8 bytes) and 8 reserved. Each parameter is a 'para' atom: a key FourCC (",cod", ",err"...),
//   a type FourCC and the value: "=s32" "=u32" "=s64" "=uui" (16 bytes) "=dat" (bytes) "=pls" (a binary property list)
//   "=fds" (a video format description as a big-endian QuickTime ImageDescription) "=sbf" (a sample buffer, atoms).
//   A property list is bplist00 (CFPropertyListCreateData, kCFPropertyListBinaryFormat_v1_0).
#pragma once
#include <stdint.h>
#include <string.h>

namespace vtwire {

constexpr uint32_t cc(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint8_t(s[3]);
}
inline uint32_t be16(const uint8_t *p) { return uint32_t(p[0]) << 8 | p[1]; }
inline uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint64_t be64(const uint8_t *p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
inline void put32(uint8_t *p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
inline void put64(uint8_t *p, uint64_t v) { put32(p, uint32_t(v >> 32)); put32(p + 4, uint32_t(v)); }

enum : uint32_t { FlagRequest = 1, FlagHostRequest = 2, FlagReply = 4, FlagGuestReply = 8 };
constexpr uint32_t HeaderBytes = 48;

struct Atom {
    uint32_t type = 0;
    const uint8_t *body = nullptr;
    uint32_t size = 0;
};

// The atoms of [at, end), one at a time: false at the end or at a malformed one.
inline bool next(const uint8_t *&at, const uint8_t *end, Atom &atom) {
    if (end - at < 8) return false;
    const uint32_t size = be32(at);
    if (size < 8 || size > uint64_t(end - at)) return false;
    atom.type = be32(at + 4);
    atom.body = at + 8;
    atom.size = size - 8;
    at += size;
    return true;
}

// The first atom of `type` directly in [body, body + size).
inline bool find(const uint8_t *body, uint32_t size, uint32_t type, Atom &out) {
    const uint8_t *at = body, *end = body + size;
    Atom atom;
    while (next(at, end, atom))
        if (atom.type == type) { out = atom; return true; }
    return false;
}

struct Header {
    uint32_t type = 0, flags = 0;
    uint8_t uuid[16] = {};
    uint64_t request = 0;
};

inline bool header(const uint8_t *message, uint32_t size, Header &h) {
    if (size < HeaderBytes || be32(message) != HeaderBytes || be32(message + 4) != cc("vtpv")) return false;
    h.type = be32(message + 8);
    h.flags = be32(message + 12);
    memcpy(h.uuid, message + 16, 16);
    h.request = be64(message + 32);
    return true;
}

struct Param {
    uint32_t type = 0;
    const uint8_t *value = nullptr;
    uint32_t size = 0;
};

// The parameter of `key` of a message.
inline bool param(const uint8_t *message, uint32_t size, uint32_t key, Param &out) {
    const uint8_t *at = message, *end = message + size;
    Atom atom;
    while (next(at, end, atom)) {
        if (atom.type != cc("para") || atom.size < 8 || be32(atom.body) != key) continue;
        out.type = be32(atom.body + 4);
        out.value = atom.body + 8;
        out.size = atom.size - 8;
        return true;
    }
    return false;
}

inline bool paramS64(const uint8_t *m, uint32_t n, uint32_t key, int64_t &v) {
    Param p;
    if (!param(m, n, key, p) || p.type != cc("=s64") || p.size < 8) return false;
    v = int64_t(be64(p.value));
    return true;
}
inline bool paramS32(const uint8_t *m, uint32_t n, uint32_t key, int32_t &v) {
    Param p;
    if (!param(m, n, key, p) || (p.type != cc("=s32") && p.type != cc("=u32")) || p.size < 4) return false;
    v = int32_t(be32(p.value));
    return true;
}

// A binary property list whose top object is an ASCII string, into `out` (NUL-terminated).
inline bool plistString(const uint8_t *p, uint32_t n, char *out, uint32_t outSize) {
    if (n < 40 || memcmp(p, "bplist00", 8) || !outSize) return false;
    const uint8_t *trailer = p + n - 32;
    const uint32_t offsetSize = trailer[6], refSize = trailer[7];
    const uint64_t objects = be64(trailer + 8), top = be64(trailer + 16), table = be64(trailer + 24);
    (void)refSize;
    if (!offsetSize || offsetSize > 8 || top >= objects || table + objects * offsetSize > n - 32) return false;
    uint64_t offset = 0;
    for (uint32_t i = 0; i < offsetSize; ++i) offset = offset << 8 | p[table + top * offsetSize + i];
    if (offset >= n - 32) return false;
    const uint8_t marker = p[offset];
    if ((marker & 0xf0) != 0x50) return false;
    uint64_t length = marker & 0x0f, at = offset + 1;
    if (length == 0x0f) {
        if (at >= n || (p[at] & 0xf0) != 0x10) return false;
        const uint32_t bytes = 1u << (p[at] & 0x0f);
        length = 0;
        for (uint32_t i = 0; i < bytes; ++i) length = length << 8 | p[at + 1 + i];
        at += 1 + bytes;
    }
    if (at + length > n - 32 || length >= outSize) return false;
    memcpy(out, p + at, length);
    out[length] = 0;
    return true;
}

// A binary property list, built bottom up: leaves first, then the arrays and dictionaries that hold them, the last object made
// the top one. Integers are written as 8 bytes, object references as 1 byte (at most 255 objects).
class Plist {
public:
    enum Kind : uint8_t { False, True, Integer, String, Data, Array, Dict };
    static constexpr int MaxNodes = 160, MaxKids = 320;

    int boolean(bool v) { return add(v ? True : False); }
    int integer(int64_t v) { int n = add(Integer); if (n >= 0) nodes[n].value = v; return n; }
    int string(const char *s) { int n = add(String); if (n >= 0) { nodes[n].bytes = reinterpret_cast<const uint8_t *>(s); nodes[n].length = uint32_t(strlen(s)); } return n; }
    int data(const uint8_t *d, uint32_t length) { int n = add(Data); if (n >= 0) { nodes[n].bytes = d; nodes[n].length = length; } return n; }
    int array(const int *items, uint32_t count) {
        int n = add(Array);
        if (n < 0 || kidCount + count > MaxKids) return fail();
        nodes[n].first = uint16_t(kidCount); nodes[n].count = uint16_t(count);
        for (uint32_t i = 0; i < count; ++i) kids[kidCount++] = int16_t(items[i]);
        return n;
    }
    // keys[i] -> values[i]; keys are string nodes.
    int dict(const int *keys, const int *values, uint32_t count) {
        int n = add(Dict);
        if (n < 0 || kidCount + 2 * count > MaxKids) return fail();
        nodes[n].first = uint16_t(kidCount); nodes[n].count = uint16_t(count);
        for (uint32_t i = 0; i < count; ++i) kids[kidCount++] = int16_t(keys[i]);
        for (uint32_t i = 0; i < count; ++i) kids[kidCount++] = int16_t(values[i]);
        return n;
    }
    bool ok() const { return !failed && nodeCount > 0; }

    // The bplist00 bytes of the tree whose top object is `top`, into out; 0 when it does not fit.
    uint32_t write(int top, uint8_t *out, uint32_t capacity) const {
        if (failed || top < 0 || top >= nodeCount || nodeCount > 255) return 0;
        uint32_t at = 0, offsets[MaxNodes];
        auto put = [&](const void *src, uint32_t n) { if (at + n <= capacity) memcpy(out + at, src, n); at += n; };
        auto byte = [&](uint8_t b) { put(&b, 1); };
        auto marker = [&](uint8_t high, uint32_t count) {
            if (count < 15) { byte(uint8_t(high | count)); return; }
            byte(uint8_t(high | 0x0f));
            if (count < 256) { byte(0x10); byte(uint8_t(count)); }
            else { byte(0x11); byte(uint8_t(count >> 8)); byte(uint8_t(count)); }
        };
        put("bplist00", 8);
        for (int i = 0; i < nodeCount; ++i) {
            offsets[i] = at;
            const Node &node = nodes[i];
            switch (node.kind) {
            case False: byte(0x08); break;
            case True: byte(0x09); break;
            case Integer: { uint8_t v[8]; put64(v, uint64_t(node.value)); byte(0x13); put(v, 8); break; }
            case String: marker(0x50, node.length); put(node.bytes, node.length); break;
            case Data: marker(0x40, node.length); put(node.bytes, node.length); break;
            case Array:
                marker(0xa0, node.count);
                for (uint32_t k = 0; k < node.count; ++k) byte(uint8_t(kids[node.first + k]));
                break;
            case Dict:
                marker(0xd0, node.count);
                for (uint32_t k = 0; k < 2u * node.count; ++k) byte(uint8_t(kids[node.first + k]));
                break;
            }
        }
        const uint32_t table = at;
        if (table > 0xffff) return 0;
        for (int i = 0; i < nodeCount; ++i) { byte(uint8_t(offsets[i] >> 8)); byte(uint8_t(offsets[i])); }
        uint8_t trailer[32] = {};
        trailer[6] = 2;
        trailer[7] = 1;
        put64(trailer + 8, uint64_t(nodeCount));
        put64(trailer + 16, uint64_t(top));
        put64(trailer + 24, table);
        put(trailer, 32);
        return at <= capacity ? at : 0;
    }

private:
    struct Node {
        Kind kind;
        uint16_t first, count;
        uint32_t length;
        int64_t value;
        const uint8_t *bytes;
    };
    Node nodes[MaxNodes];
    int16_t kids[MaxKids];
    int nodeCount = 0;
    uint32_t kidCount = 0;
    bool failed = false;
    int add(Kind kind) {
        if (nodeCount >= MaxNodes) return fail();
        nodes[nodeCount] = Node{kind, 0, 0, 0, 0, nullptr};
        return nodeCount++;
    }
    int fail() { failed = true; return -1; }
};

// A message being written: the 'vtpv' header, then parameters. ok() is false once anything did not fit.
class Writer {
public:
    Writer(uint8_t *buffer, uint32_t capacity) : out(buffer), cap(capacity) {}
    void header(uint32_t type, uint32_t flags, const uint8_t *uuid, uint64_t request) {
        uint8_t h[HeaderBytes] = {};
        put32(h, HeaderBytes);
        put32(h + 4, cc("vtpv"));
        put32(h + 8, type);
        put32(h + 12, flags);
        memcpy(h + 16, uuid, 16);
        put64(h + 32, request);
        bytes(h, HeaderBytes);
    }
    void s32(uint32_t key, int32_t v) { uint8_t b[4]; put32(b, uint32_t(v)); para(key, cc("=s32"), b, 4); }
    void u32(uint32_t key, uint32_t v) { uint8_t b[4]; put32(b, v); para(key, cc("=u32"), b, 4); }
    void s64(uint32_t key, int64_t v) { uint8_t b[8]; put64(b, uint64_t(v)); para(key, cc("=s64"), b, 8); }
    void uuid(uint32_t key, const uint8_t *u) { para(key, cc("=uui"), u, 16); }
    void uuids(uint32_t key, const uint8_t *u, uint32_t count) { para(key, cc("=uu+"), u, 16 * count); }   // a CFUUID array
    void plist(uint32_t key, const Plist &p, int top) {
        uint8_t prefix[16];
        const uint32_t start = len;
        put32(prefix + 8, key);
        put32(prefix + 12, cc("=pls"));
        put32(prefix + 4, cc("para"));
        bytes(prefix, 16);
        if (!good) return;
        const uint32_t written = p.write(top, out + len, cap - len);
        if (!written) { good = false; return; }
        len += written;
        put32(out + start, len - start);
    }
    bool ok() const { return good; }
    uint32_t size() const { return len; }

private:
    uint8_t *out;
    uint32_t cap, len = 0;
    bool good = true;
    void bytes(const void *src, uint32_t n) {
        if (!good || len + n > cap) { good = false; return; }
        memcpy(out + len, src, n);
        len += n;
    }
    void para(uint32_t key, uint32_t type, const uint8_t *value, uint32_t n) {
        uint8_t prefix[16];
        put32(prefix, 16 + n);
        put32(prefix + 4, cc("para"));
        put32(prefix + 8, key);
        put32(prefix + 12, type);
        bytes(prefix, 16);
        bytes(value, n);
    }
};

// A QuickTime video ImageDescription ("=fds"): its codec, size and extension atoms (avcC, hvcC...).
struct ImageDescription {
    uint32_t codec = 0, width = 0, height = 0;
    const uint8_t *extensions = nullptr;
    uint32_t extensionsSize = 0;
};
inline bool imageDescription(const uint8_t *p, uint32_t n, ImageDescription &d) {
    if (n < 86) return false;
    const uint32_t size = be32(p);
    if (size < 86 || size > n) return false;
    d.codec = be32(p + 4);
    d.width = be16(p + 32);
    d.height = be16(p + 34);
    d.extensions = p + 86;
    d.extensionsSize = size - 86;
    return true;
}

}  // namespace vtwire
