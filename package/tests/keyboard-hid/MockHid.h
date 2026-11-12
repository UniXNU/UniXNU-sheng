// SPDX-License-Identifier: BSD-2-Clause-Patent
// Minimal synchronous IOHID interface for exercising the real KeyboardCover.cpp on the host.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
#include <functional>
using IOReturn = int;
using IOByteCount = size_t;
using IOOptionBits = unsigned;
enum IOHIDReportType {kIOHIDReportTypeInput, kIOHIDReportTypeOutput, kIOHIDReportTypeFeature};
constexpr int kIOReturnSuccess = 0, kIOReturnBadArgument = 1, kIOReturnNoMemory = 2, kIOReturnNoSpace = 3,
              kIOReturnUnsupported = 4, kIOReturnNotReady = 5, kIODirectionNone = 0;
inline unsigned liveObjects = 0, allocations = 0;
inline bool failCapacity = false, failGateAdd = false, failSuperStart = false;
inline std::function<void()> reportCallback;
inline std::vector<std::vector<uint8_t>> reports;
inline std::vector<const void *> descriptors;
class OSObject {
public:
    OSObject() {++liveObjects;}
    virtual ~OSObject() {--liveObjects;}
    virtual void free() {}
    void release() {free(); delete this;}
};
#define OSDeclareDefaultStructors(x)
#define OSDefineMetaClassAndStructors(x, y)
#define OSDynamicCast(x, p) dynamic_cast<x *>(p)
#define OSSafeReleaseNULL(p) do {if (p) {(p)->release(); (p) = nullptr;}} while (0)
class OSString : public OSObject {public: static OSString *withCString(const char *) {return new OSString;}};
class OSNumber : public OSObject {public: static OSNumber *withNumber(uint64_t, unsigned) {return new OSNumber;}};
class IOMemoryDescriptor : public OSObject {
public:
    std::vector<uint8_t> data;
    size_t length = 0;
    size_t readBytes(size_t offset, void *out, size_t count) {
        count = offset <= length ? std::min(count, length - offset) : 0;
        if (count) std::memcpy(out, data.data() + offset, count);
        return count;
    }
    size_t writeBytes(size_t offset, const void *in, size_t count) {
        count = offset <= length ? std::min(count, length - offset) : 0;
        if (count) std::memcpy(data.data() + offset, in, count);
        return count;
    }
    void setLength(size_t n) {length = std::min(n, data.size());}
};
class IOBufferMemoryDescriptor : public IOMemoryDescriptor {
public:
    static IOBufferMemoryDescriptor *withCapacity(size_t n, int) {
        if (failCapacity) return nullptr;
        auto p = new IOBufferMemoryDescriptor; p->data.resize(n); p->length = n; ++allocations; return p;
    }
    static IOBufferMemoryDescriptor *withBytes(const void *bytes, size_t n, int direction) {
        auto p = withCapacity(n, direction); if (p) p->writeBytes(0, bytes, n); return p;
    }
};
class IOWorkLoop : public OSObject {
public:
    static IOWorkLoop *workLoop() {return new IOWorkLoop;}
    int addEventSource(OSObject *) {return failGateAdd ? kIOReturnNoMemory : kIOReturnSuccess;}
    void removeEventSource(OSObject *) {}
};
class IOCommandGate : public OSObject {
    OSObject *owner;
    std::recursive_mutex lock;
public:
    using Action = IOReturn (*)(OSObject *, void *, void *, void *, void *);
    explicit IOCommandGate(OSObject *p) : owner(p) {}
    static IOCommandGate *commandGate(OSObject *p) {return new IOCommandGate(p);}
    IOReturn runAction(Action action, void *a = nullptr, void *b = nullptr) {
        std::lock_guard<std::recursive_mutex> guard(lock); return action(owner, a, b, nullptr, nullptr);
    }
};
class IOService : public OSObject {};
class IOHIDDevice : public IOService {
public:
    virtual IOWorkLoop *getWorkLoop() const {return nullptr;}
    virtual OSString *newTransportString() const {return nullptr;}
    virtual OSString *newProductString() const {return nullptr;}
    virtual OSNumber *newVendorIDNumber() const {return nullptr;}
    virtual OSNumber *newProductIDNumber() const {return nullptr;}
    virtual IOReturn newReportDescriptor(IOMemoryDescriptor **) const {return kIOReturnUnsupported;}
    virtual IOReturn getReport(IOMemoryDescriptor *, IOHIDReportType, IOOptionBits) {return kIOReturnUnsupported;}
    virtual IOReturn setReport(IOMemoryDescriptor *, IOHIDReportType, IOOptionBits) {return kIOReturnUnsupported;}
    virtual bool start(IOService *) {return !failSuperStart;}
    virtual void stop(IOService *) {}
    bool setProperty(const char *, bool) {return true;}
    int handleReport(IOMemoryDescriptor *report, IOHIDReportType, IOOptionBits) {
        const std::vector<uint8_t> before(report->data.begin(), report->data.begin() + report->length);
        if (reportCallback) reportCallback();
        if (before != std::vector<uint8_t>(report->data.begin(), report->data.begin() + report->length)) return kIOReturnBadArgument;
        reports.push_back(before); descriptors.push_back(report); return kIOReturnSuccess;
    }
};
