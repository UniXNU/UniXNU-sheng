// SPDX-License-Identifier: BSD-2-Clause-Patent
// The sheng keyboard cover as one HID device: keyboard (report 5), consumer keys (report 6), keyboard power (report 7) and the
// touchpad (report 0x19), compiled against the selected IOHIDFamily source interface. KeyboardProbe.cpp creates it when the cover
// attaches, terminates it when the cover detaches and hands it the controller's input reports; only the touchpad report is rewritten
// (Y from the other edge, palms lifted: wn8030_touchpad_report).
//
// One device with a Keyboard, a Touch Pad and Finger collections is what iPadOS's SmartCoverServiceFilter matches (all three usage
// pairs on one service, a transport without "test"): it turns the touchpad's finger collections into pointer, scroll, scale, rotate
// and tap events and reads the surface size from SurfaceDimensions. IOHIDDevice lists Application and Physical collections in the
// usage pairs, so the fingers are Physical collections. The event driver derives SurfaceDimensions from the X and Y physical ranges
// and converts only centimetres (unit 0x11) to millimetres, so the extents are given in centimetres: 9.35 x 5.55 cm.
//
// Keyboard Power (keyboard page 0x66) as a feature and an input element is the event driver's KeyboardEnabled state: the system shows
// the on-screen keyboard while it is 0. The probe sends 0 while the cover is folded behind the tablet or closed over it.
//
// macOS takes the same keyboard and consumer keys, but neither of the other two: Keyboard Power is its power key (held down for as long
// as the cover is open: the display dimmed and slept, and no key did anything), and its HID event driver makes no pointer events from a
// touchpad's finger collections ("createEventForDigitizerCollection generates null event"; only Apple's own trackpads have a driver).
// There the device is a keyboard and a pointer: the touchpad report becomes relative motion, scroll steps and two buttons
// (Wn8030.c wn8030_pointer_report), and the folded cover is silenced by KeyboardProbe.cpp alone.
#include <IOKit/hid/IOHIDDevice.h>
#include "Wn8030.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>

// 1: the touchpad is a pointer and there is no Keyboard Power (macOS).
#if defined(XNU_TARGET_OS_OSX) && XNU_TARGET_OS_OSX
#define NATIVE_COVER_POINTER 1
#else
#define NATIVE_COVER_POINTER 0
#endif

namespace {
#define FINGER \
    0x05, 0x0d, 0x09, 0x22, 0xa1, 0x00, \
    0x09, 0x42, 0x09, 0x32, 0x09, 0x47, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02, \
    0x95, 0x05, 0x81, 0x03, \
    0x09, 0x51, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, \
    0x05, 0x01, 0x65, 0x11, 0x55, 0x0e, 0x75, 0x10, \
    0x09, 0x30, 0x26, 0xe7, 0x0b, 0x46, 0xa7, 0x03, 0x81, 0x02, \
    0x09, 0x31, 0x26, 0xef, 0x07, 0x46, 0x2b, 0x02, 0x81, 0x02, \
    0x65, 0x00, 0x55, 0x00, 0x45, 0x00, \
    0xc0,
const uint8_t kDescriptor[] = {
    // Keyboard: 8 modifiers, a reserved byte, 6 keys (usages 0 to 0xa4); Caps Lock LED output.
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x05,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03,
    0x05, 0x08, 0x19, 0x02, 0x29, 0x02, 0x95, 0x01, 0x75, 0x01, 0x91, 0x02,
    0x75, 0x07, 0x91, 0x03,
    0x05, 0x07, 0x19, 0x00, 0x2a, 0xa4, 0x00, 0x15, 0x00, 0x26, 0xa4, 0x00, 0x95, 0x06, 0x75, 0x08, 0x81, 0x00,
#if !NATIVE_COVER_POINTER
    // Keyboard Power: input and feature.
    0x85, 0x07, 0x09, 0x66, 0x15, 0x00, 0x25, 0x01, 0x95, 0x01, 0x75, 0x01, 0x81, 0x02, 0x75, 0x07, 0x81, 0x03,
    0x09, 0x66, 0x75, 0x01, 0xb1, 0x02, 0x75, 0x07, 0xb1, 0x03,
#endif
    0xc0,
    // Consumer keys: one 16-bit usage, 0 to 0x2ff.
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x06,
    0x19, 0x00, 0x2a, 0xff, 0x02, 0x15, 0x00, 0x26, 0xff, 0x02, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xc0,
#if NATIVE_COVER_POINTER
    // Pointer: two buttons, X and Y as signed 16-bit relative counts, the wheel and the horizontal pan as signed bytes.
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x1a, 0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x95, 0x06, 0x81, 0x03,
    // Raw counts at 828 counts/inch. Physical extents are thousandths of an inch; push/pop keeps units off the scroll axes.
    0xa4, 0x65, 0x13, 0x55, 0x0d, 0x37, 0x6b, 0x65, 0xff, 0xff, 0x47, 0x95, 0x9a, 0x00, 0x00,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0x80, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x02, 0x81, 0x06, 0xb4,
    0x05, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06,
    0x05, 0x0c, 0x0a, 0x38, 0x02, 0x81, 0x06,
    0xc0, 0xc0,
#else
    // Touchpad: two buttons, three fingers (tip, in range, confidence, contact id, X 0..3047, Y 0..2031), contact count; the contact
    // count maximum as a feature.
    0x05, 0x0d, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x19,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x95, 0x06, 0x81, 0x03,
    FINGER FINGER FINGER
    0x05, 0x0d, 0x09, 0x54, 0x15, 0x00, 0x25, 0x03, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
    0x09, 0x55, 0xb1, 0x02,
    0xc0,
#endif
};
#undef FINGER
[[maybe_unused]] constexpr uint8_t kKeyboardId = 0x05, kPowerId = 0x07, kTouchpadId = 0x19, kMaxContacts = 3;
constexpr uint32_t kLongestReport = 21;

void (*volatile gOutputSink)(void *, uint8_t);
void *volatile gOutputContext;
}  // namespace

// [0] devices started, [1] input reports, [2] failed input reports, [3] output reports, [4] feature reads, [5] last failure
extern "C" {volatile int32_t native_keyboard_hid_stats[6];}

class ShengKeyboardCover : public IOHIDDevice {
    OSDeclareDefaultStructors(ShengKeyboardCover)
    IOWorkLoop *inputLoop = nullptr;
    IOCommandGate *gate = nullptr;
    volatile uint8_t enabled = 1;
    IOBufferMemoryDescriptor *inputReport = nullptr;
    bool gateAdded = false, stopping = false, reporting = false;
#if NATIVE_COVER_POINTER
    struct wn8030_pointer pointer = {};
#endif

    static IOReturn submitAction(OSObject *owner, void *bytes, void *length, void *, void *) {
        ShengKeyboardCover *self = OSDynamicCast(ShengKeyboardCover, owner);
        if (!self) return kIOReturnBadArgument;
        const uintptr_t count = reinterpret_cast<uintptr_t>(length);
        if (self->stopping || self->reporting) return kIOReturnNotReady;
        if (!self->inputReport || !count || count > kLongestReport) return kIOReturnBadArgument;
        // The gate serializes producers. handleReport and its interface callbacks consume/copy the bytes before returning.
        // A recursive callback must not overwrite a descriptor that is still being consumed.
        self->reporting = true;
        self->inputReport->setLength(count);
        const IOReturn result = self->inputReport->writeBytes(0, bytes, count) == count
            ? self->handleReport(self->inputReport, kIOHIDReportTypeInput, 0) : kIOReturnNoSpace;
        self->reporting = false;
        if (result) {native_keyboard_hid_stats[2] = native_keyboard_hid_stats[2] + 1; native_keyboard_hid_stats[5] = result;}
        else native_keyboard_hid_stats[1] = native_keyboard_hid_stats[1] + 1;
        return result;
    }

public:
    using IOHIDDevice::getReport;
    using IOHIDDevice::setReport;
    void submit(const uint8_t *bytes, uint32_t length) {
        if (!gate || !bytes || !length || length > kLongestReport) return;
#if NATIVE_COVER_POINTER
        // Called from the probe's thread only, which owns the pointer state.
        uint8_t moved[WN8030_POINTER_REPORT];
        if (bytes[0] == kTouchpadId) {
            if (length != WN8030_TOUCHPAD_REPORT) return;
            length = wn8030_pointer_report(&pointer, bytes, moved);
            if (!length) return;
            bytes = moved;
        }
#endif
        uint8_t copy[kLongestReport];
        memcpy(copy, bytes, length);
        gate->runAction(&ShengKeyboardCover::submitAction, copy, reinterpret_cast<void *>(uintptr_t(length)));
    }
    void setEnabled(bool on) {
        enabled = on ? 1 : 0;
#if !NATIVE_COVER_POINTER
        const uint8_t report[2] = {kPowerId, enabled};
        submit(report, sizeof report);
#endif
    }

    IOWorkLoop *getWorkLoop() const override {return inputLoop;}
    OSString *newTransportString() const override {return OSString::withCString("I2C");}
    OSString *newProductString() const override {return OSString::withCString("Xiaomi Keyboard");}
    OSNumber *newVendorIDNumber() const override {return OSNumber::withNumber(0x15d9, 32);}
    OSNumber *newProductIDNumber() const override {return OSNumber::withNumber(0x00a3, 32);}
    IOReturn newReportDescriptor(IOMemoryDescriptor **out) const override {
        if (!out) return kIOReturnBadArgument;
        *out = IOBufferMemoryDescriptor::withBytes(kDescriptor, sizeof kDescriptor, kIODirectionNone);
        return *out ? kIOReturnSuccess : kIOReturnNoMemory;
    }
    // Feature reads: the keyboard power state and the contact count maximum.
    IOReturn getReport(IOMemoryDescriptor *report, IOHIDReportType type, IOOptionBits options) override {
        if (!report || type != kIOHIDReportTypeFeature) return kIOReturnUnsupported;
        const uint8_t id = static_cast<uint8_t>(options & 0xff);
        uint8_t bytes[2] = {id, 0};
#if NATIVE_COVER_POINTER
        return kIOReturnUnsupported;   // this descriptor has no feature report
#else
        if (id == kPowerId) bytes[1] = enabled;
        else if (id == kTouchpadId) bytes[1] = kMaxContacts;
        else return kIOReturnUnsupported;
#endif
        native_keyboard_hid_stats[4] = native_keyboard_hid_stats[4] + 1;
        return report->writeBytes(0, bytes, sizeof bytes) == sizeof bytes ? kIOReturnSuccess : kIOReturnNoSpace;
    }
    // Output: the Caps Lock LED, passed to the probe's thread (the controller is only spoken to from there).
    IOReturn setReport(IOMemoryDescriptor *report, IOHIDReportType type, IOOptionBits options) override {
        if (!report || type != kIOHIDReportTypeOutput) return kIOReturnUnsupported;
        uint8_t bytes[2] = {0, 0};
        const IOByteCount length = report->readBytes(0, bytes, sizeof bytes);
        // With report IDs the report starts with its ID; a report without one is the LED byte alone.
        uint8_t leds;
        if (length >= 2 && bytes[0] == kKeyboardId) leds = bytes[1];
        else if (length == 1 && (options & 0xff) == kKeyboardId) leds = bytes[0];
        else return kIOReturnUnsupported;
        native_keyboard_hid_stats[3] = native_keyboard_hid_stats[3] + 1;
        void (*sink)(void *, uint8_t) = __atomic_load_n(&gOutputSink, __ATOMIC_ACQUIRE);
        if (sink) sink(gOutputContext, leds & 1);
        return kIOReturnSuccess;
    }
    bool start(IOService *provider) override {
        inputLoop = IOWorkLoop::workLoop();
        if (!inputLoop) return false;
        gate = IOCommandGate::commandGate(this);
        if (!gate || inputLoop->addEventSource(gate) != kIOReturnSuccess) return false;
        gateAdded = true;
        inputReport = IOBufferMemoryDescriptor::withCapacity(kLongestReport, kIODirectionNone);
        if (!inputReport) return false;
        // The generic HID event personality, as for any keyboard; not built in.
        if (!setProperty("HIDDefaultBehavior", true)) return false;
        if (!IOHIDDevice::start(provider)) return false;
        native_keyboard_hid_stats[0] = native_keyboard_hid_stats[0] + 1;
        return true;
    }
    void stop(IOService *provider) override {
        if (gateAdded) gate->runAction([](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            static_cast<ShengKeyboardCover *>(owner)->stopping = true;
            return kIOReturnSuccess;
        });
        IOHIDDevice::stop(provider);
    }
    void free() override {
        if (gateAdded) inputLoop->removeEventSource(gate);
        OSSafeReleaseNULL(inputReport);
        OSSafeReleaseNULL(gate);
        OSSafeReleaseNULL(inputLoop);
        IOHIDDevice::free();
    }
};
OSDefineMetaClassAndStructors(ShengKeyboardCover, IOHIDDevice)

extern "C" IOService *sheng_keyboard_cover_allocate(void) {return new ShengKeyboardCover;}
extern "C" void sheng_keyboard_cover_input(IOService *device, const uint8_t *report, uint32_t length) {
    ShengKeyboardCover *cover = OSDynamicCast(ShengKeyboardCover, device);
    if (cover) cover->submit(report, length);
}
extern "C" void sheng_keyboard_cover_enable(IOService *device, bool enabled) {
    ShengKeyboardCover *cover = OSDynamicCast(ShengKeyboardCover, device);
    if (cover) cover->setEnabled(enabled);
}
extern "C" void sheng_keyboard_cover_output_sink(void (*sink)(void *, uint8_t), void *context) {
    gOutputContext = context;
    __atomic_store_n(&gOutputSink, sink, __ATOMIC_RELEASE);
}
