// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off original HID observation; a separate explicit gate starts an
// owned offline input source. No entitlement or original provider is replaced.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSKext.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
#include <mach/mach_time.h>
extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_hid_static_init(void) __asm__("__GLOBAL__sub_I_NativeHidInput.cpp");
extern "C" IOService *native_hid_input_allocate(void);
extern "C" IOService *native_hid_touch_allocate(void);
extern "C" {
extern kern_return_t OSKextLoadKextWithIdentifier(const char *);
volatile uint32_t native_hid_stage,native_hid_resource_found;
volatile int native_hid_load_status=-1;
volatile int native_hid_matching_load_status=-1;
volatile uint32_t native_hid_class_sizes[5];
volatile int native_hid_input_meta_status=-1;
volatile uint64_t native_hid_input_object;
volatile uint32_t native_hid_input_events[8];
volatile int native_hid_input_results[8]={-1,-1,-1,-1,-1,-1,-1,-1};
volatile uint32_t native_hid_input_command;
char native_hid_input_registry[2048];
char native_hid_input_props[1024];
extern volatile uint32_t native_legacy_transfer_calls;
// Optional unattended Home presses for hardware, which has no external harness writing the mailbox:
// offline-hid-auto-home=<seconds after the second presented frame>, offline-hid-auto-home-count (default 1)
// and offline-hid-auto-home-interval (default 20 s). Disabled unless the first argument is set.
static unsigned autoConfigured,autoFirst,autoCount,autoInterval,autoSent;
static uint64_t autoNext,autoFrequency;
static uint64_t counterNow(void){uint64_t v;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(v));return v;}
bool native_hid_input_requested(void) {
    if(native_legacy_transfer_calls<2)return false;
    if(!autoConfigured) {
        autoConfigured=1;autoFirst=0;autoCount=1;autoInterval=20;
        PE_parse_boot_argn("offline-hid-auto-home",&autoFirst,sizeof(autoFirst));
        PE_parse_boot_argn("offline-hid-auto-home-count",&autoCount,sizeof(autoCount));
        PE_parse_boot_argn("offline-hid-auto-home-interval",&autoInterval,sizeof(autoInterval));
        if(autoCount>8)autoCount=8;if(autoInterval<5)autoInterval=5;
        __asm__ volatile("mrs %0, cntfrq_el0":"=r"(autoFrequency));
        if(autoFirst && autoFrequency)autoNext=counterNow()+(uint64_t)autoFirst*autoFrequency;
    }
    if(autoFirst && autoFrequency && autoSent<autoCount && counterNow()>=autoNext) {
        ++autoSent;autoNext+=(uint64_t)autoInterval*autoFrequency;return true;
    }
    if(native_hid_input_command!=1)return false;
    native_hid_input_command=0;return true;
}
static void describeInput(IORegistryEntry *entry,unsigned depth,size_t &used) {
    if(depth>4 || used>=sizeof(native_hid_input_registry)-128)return;
    int count=snprintf(native_hid_input_registry+used,sizeof(native_hid_input_registry)-used,
        "%u %s [%s]\n",depth,entry->getName(),entry->getMetaClass()->getClassName());
    if(count<=0 || (size_t)count>=sizeof(native_hid_input_registry)-used)return;
    used+=count;OSIterator *it=entry->getChildIterator(gIOServicePlane);
    if(!it)return;
    while(OSObject *child=it->getNextObject()) {
        IORegistryEntry *next=OSDynamicCast(IORegistryEntry,child);
        if(next)describeInput(next,depth+1,used);
    }
    it->release();
}
// Compact one property of the first IOHIDEventDriver below entry: XML tags dropped, key end -> '=', value end -> ' '.
static IORegistryEntry *findEventDriver(IORegistryEntry *parent,unsigned depth) {
    if(!depth)return nullptr;
    OSIterator *it=parent->getChildIterator(gIOServicePlane);if(!it)return nullptr;
    IORegistryEntry *found=nullptr;
    while(OSObject *child=it->getNextObject()) {
        IORegistryEntry *entry=OSDynamicCast(IORegistryEntry,child);
        if(!entry)continue;
        if(entry->metaCast("IOHIDEventDriver")){found=entry;break;}
        if((found=findEventDriver(entry,depth-1)))break;
    }
    it->release();return found;
}
static void appendProperty(IORegistryEntry *driver,const char *key,const char *label,size_t &used) {
    OSObject *value=driver->copyProperty(key);
    int n=snprintf(native_hid_input_props+used,sizeof(native_hid_input_props)-used,"%s:",label);
    if(n>0)used+=n;
    if(value) {
        OSSerialize *text=OSSerialize::withCapacity(4096);
        if(text && value->serialize(text)) {
            const char *in=text->text();bool tag=false;char name[16];unsigned nameLen=0;
            for(unsigned i=0;in && in[i] && used<sizeof(native_hid_input_props)-2;++i) {
                char c=in[i];
                if(c=='<'){tag=true;nameLen=0;continue;}
                if(tag) {
                    if(c=='>') {
                        tag=false;name[nameLen]=0;
                        if(!strcmp(name,"/key"))native_hid_input_props[used++]='=';
                        else if(name[0]=='/')native_hid_input_props[used++]=' ';
                        else if(!strcmp(name,"true/")||!strcmp(name,"false/")){
                            used+=snprintf(native_hid_input_props+used,sizeof(native_hid_input_props)-used,"%s ",name[0]=='t'?"T":"F");
                        }
                    } else if(nameLen<sizeof(name)-1)name[nameLen++]=c;
                    continue;
                }
                if(c=='\n'||c=='\t')continue;
                native_hid_input_props[used++]=c;
            }
        }
        OSSafeReleaseNULL(text);OSSafeReleaseNULL(value);
    } else if(used<sizeof(native_hid_input_props)-6) {memcpy(native_hid_input_props+used,"none ",5);used+=5;}
    native_hid_input_props[used]=0;
}
__attribute__((noinline)) void native_hid_live_checkpoint(IOService *device) {
    size_t used=0;native_hid_input_registry[0]=0;describeInput(device,0,used);
    if(IORegistryEntry *driver=findEventDriver(device,4)) {
        size_t props=0;native_hid_input_props[0]=0;
        // backboardd's digitizer service classification reads Built-In/DisplayIntegrated from the event service it
        // opened, not from the HID device below it; publish them on the service itself (idempotent, once per second).
        // Set once only: every setProperty on a service with attached user clients is a registry property-change
        // notification, and repeating it each second keeps backboardd/hidd busy re-evaluating the service.
        // iPadOS 27: the event service is the kernel side of a DriverKit driver, which publishes its own usage pair (1,6) after it
        // starts, later than the first check here; a pair read back as anything but (0x0d,4) is therefore published again.
        static IORegistryEntry *publishedFor;
        OSNumber *page=OSDynamicCast(OSNumber,driver->getProperty("PrimaryUsagePage"));
        OSNumber *usage=OSDynamicCast(OSNumber,driver->getProperty("PrimaryUsage"));
        const bool digitizerPair=page && usage && page->unsigned32BitValue()==0x0d && usage->unsigned32BitValue()==4;
        if(device->getProperty("Built-In") && (publishedFor!=driver || !digitizerPair)) {
            publishedFor=driver;
            driver->setProperty("Built-In",kOSBooleanTrue);driver->setProperty("DisplayIntegrated",kOSBooleanTrue);
            // backboardd (-[BKHIDDirectTouchEventProcessor processEvent:sender:dispatcher:], decoded from the target) drops a
            // digitizer event unless the sending service has PrimaryUsagePage 0x0D and PrimaryUsage 1, 4 or 0x20
            // ("ignoring digitizer event ... from unsupported service"). The combined device lists its keyboard collection
            // first, so the services would publish (1,6); publish Digitizer/Touch Screen on the services after they have
            // matched (setting it on the device would change driver matching, which is observed to depend on the layout).
            driver->setProperty("PrimaryUsagePage",(unsigned long long)0x0d,32);driver->setProperty("PrimaryUsage",(unsigned long long)4,32);
            // backboardd's digitizer filter is {DeviceUsagePage, DeviceUsage, Built-In}; make the flag visible on the
            // interface (the event service's provider) as well, not only on the device two levels down.
            if(IORegistryEntry *interface=driver->getParentEntry(gIOServicePlane)) {
                interface->setProperty("Built-In",kOSBooleanTrue);interface->setProperty("DisplayIntegrated",kOSBooleanTrue);
                interface->setProperty("PrimaryUsagePage",(unsigned long long)0x0d,32);interface->setProperty("PrimaryUsage",(unsigned long long)4,32);
            }
        }
        // The Digitizer/DebugState dumps (appendProperty) are not read on hardware runs without a photograph and walk
        // driver-internal state from a foreign thread; only the cheap Built-In readback stays.
        appendProperty(driver,"Built-In","BI",props);
        appendProperty(driver,"PrimaryUsagePage","PP",props);appendProperty(driver,"PrimaryUsage","PU",props);
    }
    __asm__ volatile("" ::: "memory");
}
// The time (mach units) of every touch report handed to the HID event driver (event 6), the newest 65536, for the export's touch timeline.
// Allocated by BootDiagExport.c once the home screen draws (a static array of this size does not fit the kernel's data segment;
// an allocation in the boot-time probe was in both images that stopped early in boot, i3ccf8f4a6 and i3f12fdf1a).
volatile uint64_t *native_touch_report_ring;
volatile uint32_t native_touch_report_count;
void native_hid_input_observed(unsigned event,int result) {
    if(event<8){native_hid_input_events[event]=native_hid_input_events[event]+1;native_hid_input_results[event]=result;}
    if(event==6 && native_touch_report_ring) {
        const uint32_t n=native_touch_report_count;
        native_touch_report_ring[n&65535]=mach_absolute_time();
        native_touch_report_count=n+1;
    }
}
__attribute__((noinline)) void native_hid_checkpoint(void) { __asm__ volatile("" ::: "memory"); }
void native_hid_input_probe(void) {
    unsigned enabled=0;
    if(!PE_parse_boot_argn("offline-hid-input",&enabled,sizeof(enabled)) || enabled!=1)return;
    if(native_hid_stage)return;
    native_hid_stage=1;
    native_hid_load_status=OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
    if(!native_hid_load_status)
        native_hid_matching_load_status=OSKext::loadKextWithIdentifier("com.apple.iokit.IOHIDEventDriver",
            true,false,kOSKextExcludeNone,kOSKextExcludeNone,nullptr);
    native_hid_stage=2;
    const char *names[]={"IOHIDUserDevice","IOHIDEventDriver","IOHIDEventService","IOHIDResourceDeviceUserClient","IOHIDDevice"};
    for(unsigned i=0;i<5;++i) {
        const OSSymbol *name=OSSymbol::withCString(names[i]);
        const OSMetaClass *meta=name?OSMetaClass::getMetaClassWithName(name):nullptr;
        if(meta)native_hid_class_sizes[i]=meta->getClassSize();
        OSSafeReleaseNULL(name);
    }
    if(native_hid_load_status==0) {
        OSDictionary *match=IOService::serviceMatching("IOHIDResource");
        IOService *resource=match?IOService::waitForMatchingService(match,5000000000ULL):nullptr;
        native_hid_resource_found=resource!=nullptr;
        unsigned sendHome=0;PE_parse_boot_argn("offline-hid-home",&sendHome,sizeof(sendHome));
        if(resource && sendHome==1 && native_hid_class_sizes[4]==208) {
            void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
            if(handle) {
                native_hid_static_init();native_hid_input_meta_status=OSMetaClass::postModLoad(handle);
                if(!native_hid_input_meta_status) {
                    IOService *device=native_hid_input_allocate();
                    native_hid_input_object=(uintptr_t)device;
                    bool valid=device && device->metaCast("IOHIDDevice") && device->init();
                    native_hid_input_observed(0,valid?0:kIOReturnError);
                    bool attached=valid && device->attach(resource);
                    native_hid_input_observed(1,attached?0:kIOReturnError);
                    bool started=attached && device->start(resource);
                    if(!started) {
                        native_hid_input_observed(7,kIOReturnError);
                        if(attached)device->detach(resource);
                        OSSafeReleaseNULL(device);native_hid_input_object=0;
                    }
                    unsigned touchHid=0;
                    unsigned split=0;
                    PE_parse_boot_argn("sheng-touch-split",&split,sizeof(split));
                    if(started && split==1 && PE_parse_boot_argn("sheng-touch-hid",&touchHid,sizeof(touchHid)) && touchHid==1) {
                        // Separate digitizer service (primary usage Digitizer/Touch Screen, Built-In).
                        IOService *touch=native_hid_touch_allocate();
                        bool touchStarted=touch && touch->metaCast("IOHIDDevice") && touch->init() && touch->attach(resource) && touch->start(resource);
                        if(!touchStarted && touch){touch->detach(resource);OSSafeReleaseNULL(touch);}
                    }
                    // The offline devices live for this bounded boot. Its
                    // service attachment and timer remain genuine live objects.
                }
            }
        }
        OSSafeReleaseNULL(resource);OSSafeReleaseNULL(match);
    }
    native_hid_stage=3;native_hid_checkpoint();
}
}

// User client calls (the xnu hook in IOUserClient::callExternalMethod, installed by native_ioc_trace_start once the home screen draws):
// every call from the processes in native_ioc_pids (the client sampler's targets), and calls of 200 us or more from any process.
// Record: when it came in, microseconds waiting for the client's IPC lock, microseconds in the method, the selector, the caller's
// pid, and the address of the client's class name (static; BootDiagExport.c names it). kernel.ioc.bin.
struct NativeIocRec {uint64_t entered;uint32_t lockUs,callUs,selector;int32_t pid;uint64_t className;};
static_assert(sizeof(NativeIocRec)==32,"user client call record");
extern "C" {
extern void (*sheng_ioconnect_hook)(void *,uint32_t,uint64_t,uint64_t,uint64_t) __attribute__((weak_import));
int proc_selfpid(void);
NativeIocRec *native_ioc_ring;
volatile uint32_t native_ioc_count;
volatile int native_ioc_pids[32];
}
static void nativeIocHook(void *client,uint32_t selector,uint64_t entered,uint64_t locked,uint64_t returned) {
    const int pid=proc_selfpid();
    uint64_t lockNs=0,callNs=0;
    absolutetime_to_nanoseconds(locked-entered,&lockNs);absolutetime_to_nanoseconds(returned-locked,&callNs);
    bool target=false;
    for(unsigned i=0;i<32 && !target;++i)target=native_ioc_pids[i]==pid && pid>0;
    if(!target && lockNs+callNs<200000)return;
    const uint32_t n=__atomic_fetch_add(&native_ioc_count,1,__ATOMIC_RELAXED);
    NativeIocRec &r=native_ioc_ring[n&262143];
    r.entered=entered;r.lockUs=(uint32_t)(lockNs/1000);r.callUs=(uint32_t)(callNs/1000);r.selector=selector;r.pid=pid;
    r.className=(uint64_t)(uintptr_t)reinterpret_cast<OSObject *>(client)->getMetaClass()->getClassName();
}
extern "C" void native_ioc_trace_start(const int *pids,unsigned count) {
    if(!&sheng_ioconnect_hook)return;
    for(unsigned i=0;i<32;++i)native_ioc_pids[i]=i<count?pids[i]:0;
    if(!native_ioc_ring) {
        NativeIocRec *ring=(NativeIocRec *)IOMallocZero(sizeof(NativeIocRec)*262144);
        if(!ring)return;
        native_ioc_ring=ring;
    }
    if(!sheng_ioconnect_hook)sheng_ioconnect_hook=nativeIocHook;
}
