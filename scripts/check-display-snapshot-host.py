#!/usr/bin/env python3
"""Verify that log readers do not wait on display property serialization."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'package/LegacyDisplayProbe.cpp').read_text()
body = source[source.index('extern "C" unsigned native_legacy_properties_now'):source.index('void native_legacy_client_enter')]
harness = r'''
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
#include <cstdint>
using namespace std::chrono_literals;
static char native_legacy_properties[4096];
static volatile uint32_t native_legacy_properties_length, native_legacy_registration_calls;
static std::mutex guard;
static std::condition_variable changed;
static bool entered, released;
struct OSSerialize {
    static OSSerialize *withCapacity(unsigned) { return new OSSerialize; }
    unsigned getLength() const { return 6; }
    const char *text() const { return "first"; }
};
struct IOService {
    bool serializeProperties(OSSerialize *) {
        std::unique_lock<std::mutex> lock(guard);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [] { return released; });
        return true;
    }
};
#define OSSafeReleaseNULL(p) do { delete (p); (p) = nullptr; } while (0)
''' + body + r'''
int main() {
    IOService display;
    auto writer = std::async(std::launch::async, [&] { native_legacy_registration_observed(&display); });
    {
        std::unique_lock<std::mutex> lock(guard);
        assert(changed.wait_for(lock, 2s, [] { return entered; }));
    }
    auto reader = std::async(std::launch::async, [] {
        char out[16];
        assert(native_legacy_properties_now(out, sizeof out) == 0);
        assert(out[0] == 0);
    });
    assert(reader.wait_for(200ms) == std::future_status::ready);
    reader.get();
    {
        std::lock_guard<std::mutex> lock(guard);
        released = true;
        changed.notify_all();
    }
    writer.get();
    native_legacy_registration_observed(&display);
    char out[16];
    assert(native_legacy_properties_now(out, sizeof out) == 6);
    assert(!strcmp(out, "first"));
    assert(native_legacy_properties_now(out, 3) == 2);
    assert(!strcmp(out, "fi"));
    assert(native_legacy_properties_now(nullptr, 3) == 0);
    assert(native_legacy_properties_now(out, 0) == 0);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(harness)
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-pthread',
                    str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True, timeout=5)
print('PASS: display snapshot readers remain responsive while registration is blocked')
