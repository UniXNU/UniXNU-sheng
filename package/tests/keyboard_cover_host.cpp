// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../KeyboardCover.cpp"
#include <cstdio>
#include <cstdlib>
#include <thread>
static unsigned checks;
#define CHECK(x) do {if (!(x)) {std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1);} ++checks;} while (0)
// Parse the actual descriptor globals, including push/pop. Check the resolution and scroll units the OS sees.
static void checkDescriptor() {
    struct Globals {int32_t v[16] = {};};
    Globals g; std::vector<Globals> stack;
    uint32_t usage = 0;
    unsigned axes = 0, wheel = 0, pan = 0;
    for (size_t i = 0; i < sizeof kDescriptor;) {
        const uint8_t tag = kDescriptor[i++];
        const unsigned size = (tag & 3) == 3 ? 4 : tag & 3, type = (tag >> 2) & 3, key = tag >> 4;
        CHECK(i + size <= sizeof kDescriptor);
        uint32_t value = 0;
        for (unsigned j = 0; j < size; ++j) value |= uint32_t(kDescriptor[i++]) << (8 * j);
        if (type == 1) {
            if (key == 10) stack.push_back(g);
            else if (key == 11) {CHECK(!stack.empty()); g = stack.back(); stack.pop_back();}
            else {
                if ((key == 1 || key == 3) && size && size < 4 && (value & (1u << (size * 8 - 1)))) value |= ~((1u << (size * 8)) - 1);
                g.v[key] = static_cast<int32_t>(value);
            }
        } else if (type == 2 && key == 0) usage = value;
        else if (type == 0) {
            if (key == 8 && g.v[8] == WN8030_POINTER_ID) {
                if (g.v[0] == 1 && usage == 0x31) {
                    CHECK(g.v[1] == -32767 && g.v[2] == 32767 && g.v[7] == 16 && g.v[9] == 2 && (value & 4));
                    CHECK(g.v[6] == 0x13 && g.v[5] == 13);
                    CHECK((g.v[2] - g.v[1]) * 1000 / (g.v[4] - g.v[3]) == 828);
                    ++axes;
                } else if (g.v[0] == 1 && usage == 0x38) {CHECK(!g.v[6] && !g.v[5] && g.v[7] == 8); ++wheel;}
                else if (g.v[0] == 12 && usage == 0x238) {CHECK(!g.v[6] && !g.v[5]); ++pan;}
            }
            usage = 0;
        }
    }
    CHECK(stack.empty());
    CHECK(axes == NATIVE_COVER_POINTER && wheel == NATIVE_COVER_POINTER && pan == NATIVE_COVER_POINTER);
}
int main() {
    checkDescriptor();
    IOService provider;
    const unsigned baseline = liveObjects;
    // Cleanup includes partially started devices, including an installed gate but no report buffer.
    for (unsigned failure = 0; failure < 3; ++failure) {
        failCapacity = failure == 0; failGateAdd = failure == 1; failSuperStart = failure == 2;
        auto cover = new ShengKeyboardCover;
        CHECK(!cover->start(&provider)); cover->release(); CHECK(liveObjects == baseline);
    }
    failCapacity = failGateAdd = failSuperStart = false;
    auto cover = new ShengKeyboardCover; CHECK(cover->start(&provider));
    const unsigned allocated = allocations;
    const uint8_t key[9] = {5, 2, 0, 4, 0, 0, 0, 0, 0}, consumer[3] = {6, 0xe9, 0};
    for (unsigned i = 0; i < 1000; ++i) {cover->submit(key, sizeof key); cover->submit(consumer, sizeof consumer);}
    CHECK(allocations == allocated && reports.size() == 2000);
    for (unsigned i = 0; i < reports.size(); ++i) {
        CHECK(descriptors[i] == descriptors[0]);
        CHECK(reports[i] == (i & 1 ? std::vector<uint8_t>(consumer, consumer + 3) : std::vector<uint8_t>(key, key + 9)));
    }
    // Reentrant input cannot corrupt the report being read by an IOHID callback.
    reportCallback = [&] {cover->submit(consumer, sizeof consumer);};
    cover->submit(key, sizeof key); reportCallback = nullptr;
    CHECK(reports.size() == 2001 && reports.back() == std::vector<uint8_t>(key, key + 9));
    CHECK(native_keyboard_hid_stats[2] == 0);
    // Independent producer threads share one descriptor through the gate.
    std::thread first([&] {for (unsigned i = 0; i < 500; ++i) cover->submit(key, sizeof key);});
    std::thread second([&] {for (unsigned i = 0; i < 500; ++i) cover->submit(consumer, sizeof consumer);});
    first.join(); second.join();
    CHECK(reports.size() == 3001 && allocations == allocated);
    for (unsigned i = 2001; i < reports.size(); ++i) CHECK(reports[i].size() == (reports[i][0] == 5 ? 9 : 3));
    cover->stop(&provider); cover->submit(key, sizeof key);
    CHECK(reports.size() == 3001 && allocations == allocated);
    cover->release(); CHECK(liveObjects == baseline);
    std::printf("PASS keyboard HID (%s): %u checks\n", NATIVE_COVER_POINTER ? "macOS" : "iPadOS", checks);
}
