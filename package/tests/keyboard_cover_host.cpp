// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../KeyboardCover.cpp"
extern "C" {volatile int32_t native_keyboard_stats[17];}   // KeyboardProbe.cpp's controller counters
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
#if NATIVE_COVER_POINTER
    // The touchpad report carries no time; the device stamps each one as it arrives. A finger whose lift was never reported and
    // that lands far away after a pause starts from there instead of moving the pointer by the whole distance.
    auto touch = [&](uint64_t ms, uint16_t x) {
        uint8_t pad[WN8030_TOUCHPAD_REPORT] = {WN8030_TOUCHPAD_ID, 0, 7, 3, uint8_t(x), uint8_t(x >> 8), 0xe8, 0x03};
        pad[WN8030_TOUCHPAD_REPORT - 1] = 1;
        hostClockNs = ms * 1000000;
        const size_t before = reports.size();
        cover->submit(pad, sizeof pad);
        return reports.size() == before ? 0 : int16_t(reports.back()[2] | reports.back()[3] << 8);
    };
    CHECK(touch(1000, 1000) == 0);
    CHECK(touch(1013, 1013) == 13);
    CHECK(touch(1163, 2500) == 0);   // 150 ms later, 1487 counts away
    CHECK(touch(1176, 2504) == 4);
    CHECK(reports.back().size() == WN8030_POINTER_REPORT && reports.back()[0] == WN8030_POINTER_ID);
    // The timing record goes out with the first touchpad report and then at most once a second of report time, as 32-bit
    // little-endian words: layout 1, reports, pointer reports sent, restarts, the gap buckets, then the controller's counters.
    auto record = [&] {
        const auto &bytes = cover->dataProperties.at("ShengTouchpadTiming");
        std::vector<uint32_t> words(bytes.size() / 4);
        std::memcpy(words.data(), bytes.data(), words.size() * 4);
        return words;
    };
    auto words = record();
    CHECK(words.size() == 4 + WN8030_GAP_BUCKETS + 17 && words[0] == 1 && words[1] == 1 && words[2] == 0);
    native_keyboard_stats[6] = 1234;
    CHECK(touch(2200, 2510) == 0);   // over a second after the first record, and 1024 ms after the last report: a new touch
    words = record();
    CHECK(words[1] == 5 && words[2] == 2 && words[3] == 2);
    CHECK(words[4 + 1] == 2 && words[4 + 6] == 1 && words[4 + 7] == 1);   // 13 ms twice, 150 ms, 1024 ms
    CHECK(words[4 + WN8030_GAP_BUCKETS + 6] == 1234);
    CHECK(touch(2213, 2516) == 6);
    CHECK(record()[1] == 5);         // within the second: the record is not sent again
    const size_t stamped = reports.size();
#else
    const size_t stamped = 3001;
#endif
    cover->stop(&provider); cover->submit(key, sizeof key);
    CHECK(reports.size() == stamped && allocations == allocated);
    cover->release(); CHECK(liveObjects == baseline);
    std::printf("PASS keyboard HID (%s): %u checks\n", NATIVE_COVER_POINTER ? "macOS" : "iPadOS", checks);
}
