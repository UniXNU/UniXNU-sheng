// SPDX-License-Identifier: BSD-2-Clause-Patent
// macOS: the board's network interface en0, an IOEthernetController of the original IONetworkingFamily (SDK headers; the build
// checks its vtable against the collection's, net-ether-<build>.json). Its address is the WiFi radio's (WifiProbe.cpp: made from
// /chosen/unique-chip-id), and it carries the WiFi module's frames (WifiNet.cpp, macOS: sheng_ethernet_wifi_attach): the stack's
// frames go to the module's transmit queue, received ones enter the interface's ifnet, the link follows the module's join. Before
// the module attaches, frames are dropped and the link is reported up while the interface is enabled.
//
// Why macOS needs it before the radio: IOTimeSyncFamily makes the TimeSync clock identifier from the MAC of an IOEthernetInterface or
// IO80211Interface; without one clocksyncd never gets a clock, and WindowServer (QuartzCore's TimeSync clock) waits on clocksyncd.
// The WiFi module's own registry classes (WifiNet.cpp) are named like IONetworkingFamily's and cannot register next to them.
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/OSKextLib.h>
#include <pexpert/pexpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <kern/thread.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-anon-enum-enum-conversion"   // the family's headers mix anonymous enums
#define __PRIVATE_SPI__   // the shipped family uses IONetworkController's reserved slots 2-5 (outputStart ... networkInterfaceNotification)
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IONetworkMedium.h>
#include <IOKit/network/IOOutputQueue.h>   // kIOReturnOutputDropped
#pragma clang diagnostic pop

extern kmod_info_t g_kernel_kmod_info;
extern "C" void sheng_ethernet_static_init(void) __asm__("__GLOBAL__sub_I_ShengEthernet.cpp");
// Result of the probe: 0 not run, 1 started, negative: the step that failed.
extern "C" volatile int sheng_ethernet_state = 0;
// The WiFi module's transmit function (consumes the frame; WifiNet.cpp) and its link state, once it attached.
static int (*volatile gWifiOutput)(mbuf_t);
static volatile bool gWifiLink;

namespace {

// The WiFi radio's address, as WifiProbe.cpp's boardMac makes it: FNV-1a of /chosen/unique-chip-id, locally administered, unicast.
bool radioAddress(uint8_t m[6]) {
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    OSData *id = chosen ? OSDynamicCast(OSData, chosen->getProperty("unique-chip-id")) : nullptr;
    uint64_t seed = 0;
    if (id && id->getLength() >= sizeof seed) memcpy(&seed, id->getBytesNoCopy(), sizeof seed);
    OSSafeReleaseNULL(chosen);
    if (!seed) return false;
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned i = 0; i < 8; ++i) h = (h ^ ((seed >> (8 * i)) & 0xff)) * 0x100000001b3ull;
    for (unsigned i = 0; i < 6; ++i) m[i] = (uint8_t)(h >> (8 * i));
    m[0] = (uint8_t)((m[0] & 0xfe) | 0x02);
    return true;
}

}  // namespace

class ShengEthernet : public IOEthernetController {
    OSDeclareDefaultStructors(ShengEthernet)
    IOEthernetInterface *interface = nullptr;
    const IONetworkMedium *medium = nullptr;
    uint8_t address[6] = {};
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    // The family's start waits for IOBSD unless the controller says not to; that wait needs IOResources matched, which it is not while
    // this controller, attached to IOResources, is being started there: a random boot deadlock (macOS-DroidVM 0855e2a, DroidVMEthernet).
    UInt32 getFeatures() const override { return kIONetworkFeatureNoBSDWait; }
    IOReturn getHardwareAddress(IOEthernetAddress *out) override {
        if (!out) return kIOReturnBadArgument;
        memcpy(out->bytes, address, sizeof address);
        return kIOReturnSuccess;
    }
    IOReturn enable(IONetworkInterface *) override {
        enabled = true;
        link(!gWifiOutput || gWifiLink);
        return kIOReturnSuccess;
    }
    IOReturn disable(IONetworkInterface *) override {
        enabled = false;
        setLinkStatus(kIONetworkLinkValid, nullptr);
        return kIOReturnSuccess;
    }
    // To the WiFi module's queue once it attached (it takes the frame in every case); before that consumed and counted as dropped,
    // as a controller with no carrier does.
    UInt32 outputPacket(mbuf_t packet, void *) override {
        if (int (*output)(mbuf_t) = gWifiOutput) {output(packet); return kIOReturnOutputSuccess;}
        freePacket(packet);
        return kIOReturnOutputDropped;
    }
    void link(bool up) {
        if (up) setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive, medium, 1000ULL * 1000 * 1000);
        else setLinkStatus(kIONetworkLinkValid, nullptr);
    }
    ifnet_t bsdIfnet() const {return interface ? interface->getIfnet() : nullptr;}
    bool enabled = false;
    IOReturn setPromiscuousMode(bool) override { return kIOReturnSuccess; }
    IOReturn setMulticastMode(bool) override { return kIOReturnSuccess; }
    IOReturn setMulticastList(IOEthernetAddress *, UInt32) override { return kIOReturnSuccess; }
};

OSDefineMetaClassAndStructors(ShengEthernet, IOEthernetController)

static ShengEthernet *volatile gController;   // the started controller (never stopped)

bool ShengEthernet::start(IOService *provider)
{
    if (!radioAddress(address) || !IOEthernetController::start(provider)) return false;
    OSDictionary *media = OSDictionary::withCapacity(1);
    IONetworkMedium *auto1g = IONetworkMedium::medium(IOMediumType(kIOMediumEthernetAuto) | IOMediumType(kIOMediumOptionFullDuplex),
                                                     1000ULL * 1000 * 1000);
    bool ok = media && auto1g && IONetworkMedium::addMedium(media, auto1g) && publishMediumDictionary(media);
    medium = ok ? IONetworkMedium::getMediumWithType(getMediumDictionary(), auto1g->getType()) : nullptr;
    OSSafeReleaseNULL(auto1g);
    OSSafeReleaseNULL(media);
    if (!ok || !medium) return false;
    setProperty("IOBuiltin", true);
    if (!attachInterface(reinterpret_cast<IONetworkInterface **>(&interface), true)) return false;
    return true;
}

void ShengEthernet::stop(IOService *provider)
{
    if (interface) {detachInterface(interface); OSSafeReleaseNULL(interface);}
    IOEthernetController::stop(provider);
}

namespace {

void worker(void *, wait_result_t)
{
    // IONetworkingFamily has no personality on this board; loading it runs its constructors before this class registers.
    const OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.iokit.IONetworkingFamily");
    if (loaded != kOSReturnSuccess && loaded != kOSKextReturnStartStopError) {sheng_ethernet_state = -1; thread_terminate(current_thread());}
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {sheng_ethernet_state = -2; thread_terminate(current_thread());}
    sheng_ethernet_static_init();
    if (OSMetaClass::postModLoad(handle)) {sheng_ethernet_state = -3; thread_terminate(current_thread());}
    ShengEthernet *controller = OSTypeAlloc(ShengEthernet);
    IOService *parent = IOService::getResourceService();
    bool attached = false;
    if (!controller || !controller->init(nullptr) || !parent) sheng_ethernet_state = -4;
    else if (!(attached = controller->attach(parent))) sheng_ethernet_state = -5;
    else if (!controller->start(parent)) sheng_ethernet_state = -6;
    else {controller->registerService(); controller->retain(); gController = controller; sheng_ethernet_state = 1;}
    if (sheng_ethernet_state != 1) {
        if (attached) controller->detach(parent);
        IOLog("ShengEthernet: not started (%d)\n", sheng_ethernet_state);
    }
    OSSafeReleaseNULL(controller);
    thread_terminate(current_thread());
}

}  // namespace

// The WiFi module (WifiNet.cpp, macOS): its transmit function; the link goes down until it reports a join. False without the controller.
extern "C" bool sheng_ethernet_wifi_attach(int (*output)(mbuf_t))
{
    ShengEthernet *controller = gController;
    if (!controller || !output) return false;
    gWifiLink = false;
    gWifiOutput = output;
    if (controller->enabled) controller->link(false);
    return true;
}
// The module's link: up after a join, down when it is lost or left.
extern "C" void sheng_ethernet_wifi_link(bool up)
{
    gWifiLink = up;
    ShengEthernet *controller = gController;
    if (controller && controller->enabled) controller->link(up);
}
// The interface's ifnet: none until the network configuration has named the interface (IONetworkStack attaches it then).
extern "C" ifnet_t sheng_ethernet_ifnet(void)
{
    ShengEthernet *controller = gController;
    return controller ? controller->bsdIfnet() : nullptr;
}

// The board entry (LegacyDisplayProbe.cpp's late probes), macOS only: ethernet=1.
extern "C" void sheng_ethernet_probe(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("ethernet", &enabled, sizeof enabled) || enabled != 1) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
