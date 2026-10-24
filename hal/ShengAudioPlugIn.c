// SPDX-License-Identifier: BSD-2-Clause-Patent
// The sheng built-in audio device for the iOS audio server: one device with a stereo output stream (the six speakers) and a mono input
// stream (the microphones). Samples move through rings shared with the kernel service ShengAudio (package/ShengAudioAbi.h); the service
// feeds the audio DSP. Objects: the plug-in (1), the device (2), the output stream (3), the input stream (4), the output volume (5).
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include "ShengAudioAbi.h"

enum {kPlugIn = kAudioObjectPlugInObject, kDevice = 2, kOutStream = 3, kInStream = 4, kVolume = 5};
#define kPlugInBundleID "org.unixnu.sheng.audio"
#define kDeviceUID "ShengAudioDevice"
#define kModelUID "ShengAudioModel"
static const Float64 kRate = SHENG_AUDIO_RATE;

static AudioServerPlugInHostRef gHost;
static pthread_mutex_t gLock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint gRefs = 1;
static io_connect_t gConnect;
static ShengAudioControl *gControl;
static int16_t *gOut, *gIn;
static UInt32 gIoUsers;
static Float32 gVolume = 1.0f;
static Float64 gHostTicksPerFrame;

// ---- Kernel service ------------------------------------------------------------------------------------------------------------
static int mapType(uint32_t type, void **out) {
    mach_vm_address_t address = 0;
    mach_vm_size_t size = 0;
    if (IOConnectMapMemory64(gConnect, type, mach_task_self(), &address, &size, kIOMapAnywhere) != kIOReturnSuccess) return -1;
    *out = (void *)(uintptr_t)address;
    return 0;
}
static int connectService(void) {
    if (gControl) return 0;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(SHENG_AUDIO_CLASS));
    if (!service) return -1;
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &gConnect);
    IOObjectRelease(service);
    if (kr != KERN_SUCCESS) return -1;
    void *control = NULL, *out = NULL, *in = NULL;
    if (mapType(ShengAudioMemoryControl, &control) || mapType(ShengAudioMemoryOutput, &out) || mapType(ShengAudioMemoryInput, &in)) {
        IOServiceClose(gConnect); gConnect = 0; return -1;
    }
    ShengAudioControl *c = control;
    if (c->magic != SHENG_AUDIO_MAGIC || c->version != SHENG_AUDIO_VERSION) {IOServiceClose(gConnect); gConnect = 0; return -1;}
    gOut = out; gIn = in; gControl = c;
    gHostTicksPerFrame = (Float64)c->ticks_per_frame_q32 / 4294967296.0;
    return 0;
}
static kern_return_t call(uint32_t selector) {return gConnect ? IOConnectCallScalarMethod(gConnect, selector, NULL, 0, NULL, NULL) : KERN_FAILURE;}

// ---- Helpers ---------------------------------------------------------------------------------------------------------------------
static AudioStreamBasicDescription format(UInt32 channels) {
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = kRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 4 * channels; f.mFramesPerPacket = 1; f.mBytesPerFrame = 4 * channels;
    f.mChannelsPerFrame = channels; f.mBitsPerChannel = 32;
    return f;
}
#define REPLY(type, value) do { if (inDataSize < sizeof(type)) return kAudioHardwareBadPropertySizeError; *(type *)outData = (value); *outDataSize = sizeof(type); return 0; } while (0)
#define REPLY_STR(text) REPLY(CFStringRef, CFSTR(text))
static UInt32 latencyFrames(void) {return gControl ? gControl->lead_frames : 2 * SHENG_AUDIO_PERIOD_FRAMES;}

// ---- IUnknown --------------------------------------------------------------------------------------------------------------------
static HRESULT QueryInterface(void *self, REFIID uuid, LPVOID *out);
static ULONG AddRef(void *self) {(void)self; return atomic_fetch_add(&gRefs, 1) + 1;}
static ULONG Release(void *self) {(void)self; UInt32 r = atomic_load(&gRefs); if (r) r = atomic_fetch_sub(&gRefs, 1) - 1; return r;}

static OSStatus Initialize(AudioServerPlugInDriverRef d, AudioServerPlugInHostRef host) {(void)d; gHost = host; return 0;}
static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *c, AudioObjectID *o) {(void)d; (void)desc; (void)c; (void)o; return kAudioHardwareUnsupportedOperationError;}
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID o) {(void)d; (void)o; return kAudioHardwareUnsupportedOperationError;}
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) {(void)d; (void)o; (void)c; return 0;}
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID o, const AudioServerPlugInClientInfo *c) {(void)d; (void)o; (void)c; return 0;}
static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i) {(void)d; (void)o; (void)a; (void)i; return 0;}
static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID o, UInt64 a, void *i) {(void)d; (void)o; (void)a; (void)i; return 0;}

// ---- Properties ------------------------------------------------------------------------------------------------------------------
static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a);
static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qs, const void *q, UInt32 *size);
static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                                const void *q, UInt32 inDataSize, UInt32 *outDataSize, void *outData);

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out) {
    (void)d; (void)pid;
    *out = (o == kVolume && (a->mSelector == kAudioLevelControlPropertyScalarValue || a->mSelector == kAudioLevelControlPropertyDecibelValue)) ||
           ((o == kOutStream || o == kInStream) && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat)) ||
           (o == kDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate);
    return HasProperty(d, o, pid, a) ? 0 : kAudioHardwareUnknownPropertyError;
}

static Float32 toDb(Float32 scalar) {return scalar <= 0.0001f ? -96.0f : 20.0f * log10f(scalar);}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                                const void *q, UInt32 size, const void *data) {
    (void)d; (void)pid; (void)qs; (void)q;
    if (o == kVolume && size >= sizeof(Float32)) {
        Float32 v = *(const Float32 *)data;
        if (a->mSelector == kAudioLevelControlPropertyDecibelValue) v = v <= -96.0f ? 0.0f : powf(10.0f, v / 20.0f);
        else if (a->mSelector != kAudioLevelControlPropertyScalarValue) return kAudioHardwareUnknownPropertyError;
        gVolume = v < 0 ? 0 : v > 1 ? 1 : v;
        AudioObjectPropertyAddress changed[2] = {{kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
                                                 {kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}};
        gHost->PropertiesChanged(gHost, kVolume, 2, changed);
        return 0;
    }
    if ((o == kOutStream || o == kInStream) && (a->mSelector == kAudioStreamPropertyVirtualFormat || a->mSelector == kAudioStreamPropertyPhysicalFormat)) {
        const AudioStreamBasicDescription *f = data;
        return size >= sizeof *f && f->mSampleRate == kRate && f->mChannelsPerFrame == (o == kOutStream ? 2u : 1u) ? 0 : kAudioDeviceUnsupportedFormatError;
    }
    if (o == kDevice && a->mSelector == kAudioDevicePropertyNominalSampleRate)
        return size >= sizeof(Float64) && *(const Float64 *)data == kRate ? 0 : kAudioDeviceUnsupportedFormatError;
    return kAudioHardwareUnknownPropertyError;
}

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a) {
    UInt32 size = 0;
    return GetPropertyDataSize(d, o, pid, a, 0, NULL, &size) == 0;
}

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qs, const void *q, UInt32 *size) {
    UInt8 scratch[1024];
    UInt32 written = 0;
    OSStatus r = GetPropertyData(d, o, pid, a, qs, q, sizeof scratch, &written, scratch);
    if (r == 0) *size = written;
    return r;
}

static UInt32 objects(AudioObjectID o, AudioObjectPropertyScope scope, AudioObjectID *list) {
    UInt32 n = 0;
    if (o == kPlugIn) list[n++] = kDevice;
    else if (o == kDevice) {
        if (scope != kAudioObjectPropertyScopeInput) list[n++] = kOutStream;
        if (scope != kAudioObjectPropertyScopeOutput) list[n++] = kInStream;
        if (scope != kAudioObjectPropertyScopeInput) list[n++] = kVolume;
    }
    return n;
}

static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID o, pid_t pid, const AudioObjectPropertyAddress *a, UInt32 qs,
                                const void *q, UInt32 inDataSize, UInt32 *outDataSize, void *outData) {
    (void)d; (void)pid;
    const AudioObjectPropertySelector s = a->mSelector;
    switch (o) {
    case kPlugIn:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: REPLY_STR("UniXNU");
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            if (inDataSize < sizeof(AudioObjectID)) {*outDataSize = 0; return 0;}
            ((AudioObjectID *)outData)[0] = kDevice; *outDataSize = sizeof(AudioObjectID); return 0;
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            CFStringRef uid = qs >= sizeof(CFStringRef) ? *(const CFStringRef *)q : NULL;
            REPLY(AudioObjectID, uid && CFStringCompare(uid, CFSTR(kDeviceUID), 0) == kCFCompareEqualTo ? kDevice : kAudioObjectUnknown);
        }
        case kAudioPlugInPropertyResourceBundle: REPLY_STR("");
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kDevice:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kPlugIn);
        case kAudioObjectPropertyName: REPLY_STR("Speaker");
        case kAudioObjectPropertyManufacturer: REPLY_STR("UniXNU");
        case kAudioDevicePropertyDeviceUID: REPLY_STR(kDeviceUID);
        case kAudioDevicePropertyModelUID: REPLY_STR(kModelUID);
        case kAudioDevicePropertyTransportType: REPLY(UInt32, kAudioDeviceTransportTypeBuiltIn);
        case kAudioDevicePropertyRelatedDevices: REPLY(AudioObjectID, kDevice);
        case kAudioDevicePropertyClockDomain: REPLY(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: REPLY(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: REPLY(UInt32, gIoUsers > 0);
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: REPLY(UInt32, 1);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: REPLY(UInt32, 1);
        case kAudioDevicePropertyLatency: REPLY(UInt32, latencyFrames());
        case kAudioDevicePropertySafetyOffset: REPLY(UInt32, latencyFrames());
        case kAudioDevicePropertyNominalSampleRate: REPLY(Float64, kRate);
        case kAudioDevicePropertyAvailableNominalSampleRates: REPLY(AudioValueRange, ((AudioValueRange){kRate, kRate}));
        case kAudioDevicePropertyIsHidden: REPLY(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: REPLY(UInt32, SHENG_AUDIO_RING_FRAMES);
        case kAudioDevicePropertyClockIsStable: REPLY(UInt32, 1);
        case kAudioDevicePropertyClockAlgorithm: REPLY(UInt32, kAudioDeviceClockAlgorithmSimpleIIR);
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            ((UInt32 *)outData)[0] = 1; ((UInt32 *)outData)[1] = a->mScope == kAudioObjectPropertyScopeInput ? 1 : 2;
            *outDataSize = 2 * sizeof(UInt32); return 0;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            const UInt32 channels = a->mScope == kAudioObjectPropertyScopeInput ? 1 : 2;
            const UInt32 bytes = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + channels * sizeof(AudioChannelDescription));
            if (inDataSize < bytes) return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout *l = outData;
            memset(l, 0, bytes);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mNumberChannelDescriptions = channels;
            l->mChannelDescriptions[0].mChannelLabel = channels == 1 ? kAudioChannelLabel_Mono : kAudioChannelLabel_Left;
            if (channels == 2) l->mChannelDescriptions[1].mChannelLabel = kAudioChannelLabel_Right;
            *outDataSize = bytes; return 0;
        }
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:
        case kAudioObjectPropertyControlList: {
            AudioObjectID list[3];
            UInt32 n = objects(kDevice, a->mScope, list), out = 0;
            for (UInt32 i = 0; i < n; ++i) {
                if (s == kAudioDevicePropertyStreams && list[i] == kVolume) continue;
                if (s == kAudioObjectPropertyControlList && list[i] != kVolume) continue;
                if ((out + 1) * sizeof(AudioObjectID) > inDataSize) break;
                ((AudioObjectID *)outData)[out++] = list[i];
            }
            *outDataSize = out * (UInt32)sizeof(AudioObjectID); return 0;
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    case kOutStream:
    case kInStream: {
        const UInt32 channels = o == kOutStream ? 2 : 1;
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kDevice);
        case kAudioObjectPropertyName: if (o == kOutStream) REPLY_STR("Speaker"); REPLY_STR("Microphone");
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioStreamPropertyIsActive: REPLY(UInt32, 1);
        case kAudioStreamPropertyDirection: REPLY(UInt32, o == kInStream);
        case kAudioStreamPropertyTerminalType: REPLY(UInt32, o == kOutStream ? kAudioStreamTerminalTypeSpeaker : kAudioStreamTerminalTypeMicrophone);
        case kAudioStreamPropertyStartingChannel: REPLY(UInt32, 1);
        case kAudioStreamPropertyLatency: REPLY(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: REPLY(AudioStreamBasicDescription, format(channels));
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription r = {format(channels), {kRate, kRate}};
            REPLY(AudioStreamRangedDescription, r);
        }
        default: return kAudioHardwareUnknownPropertyError;
        }
    }
    case kVolume:
        switch (s) {
        case kAudioObjectPropertyBaseClass: REPLY(AudioClassID, kAudioLevelControlClassID);
        case kAudioObjectPropertyClass: REPLY(AudioClassID, kAudioVolumeControlClassID);
        case kAudioObjectPropertyOwner: REPLY(AudioObjectID, kDevice);
        case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
        case kAudioControlPropertyScope: REPLY(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: REPLY(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
        case kAudioLevelControlPropertyScalarValue: REPLY(Float32, gVolume);
        case kAudioLevelControlPropertyDecibelValue: REPLY(Float32, toDb(gVolume));
        case kAudioLevelControlPropertyDecibelRange: REPLY(AudioValueRange, ((AudioValueRange){-96.0, 0.0}));
        case kAudioLevelControlPropertyConvertScalarToDecibels:
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            *(Float32 *)outData = toDb(*(Float32 *)outData); *outDataSize = sizeof(Float32); return 0;
        case kAudioLevelControlPropertyConvertDecibelsToScalar:
            if (inDataSize < sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
            *(Float32 *)outData = *(Float32 *)outData <= -96.0f ? 0.0f : powf(10.0f, *(Float32 *)outData / 20.0f); *outDataSize = sizeof(Float32); return 0;
        default: return kAudioHardwareUnknownPropertyError;
        }
    default:
        return kAudioHardwareBadObjectError;
    }
}

// ---- IO ----------------------------------------------------------------------------------------------------------------------------
static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client) {
    (void)d; (void)client;
    if (o != kDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    OSStatus r = 0;
    if (gIoUsers == 0) {
        if (connectService()) r = kAudioHardwareNotRunningError;
        else if (call(ShengAudioStartOutput) != KERN_SUCCESS) r = kAudioHardwareNotRunningError;
        else call(ShengAudioStartInput);
    }
    if (!r) gIoUsers++;
    pthread_mutex_unlock(&gLock);
    return r;
}
static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client) {
    (void)d; (void)client;
    if (o != kDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gLock);
    if (gIoUsers && --gIoUsers == 0) {call(ShengAudioStopOutput); call(ShengAudioStopInput);}
    pthread_mutex_unlock(&gLock);
    return 0;
}
static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, Float64 *sampleTime, UInt64 *hostTime, UInt64 *seed) {
    (void)d; (void)o; (void)client;
    if (!gControl) return kAudioHardwareNotRunningError;
    uint32_t seq;
    uint64_t anchor;
    do {seq = gControl->out_seq; anchor = gControl->out_anchor_host;} while ((seq & 1) || seq != gControl->out_seq);
    const uint64_t now = mach_absolute_time();
    const Float64 period = gHostTicksPerFrame * SHENG_AUDIO_RING_FRAMES;
    const uint64_t wraps = now > anchor ? (uint64_t)((Float64)(now - anchor) / period) : 0;
    *sampleTime = (Float64)(wraps * SHENG_AUDIO_RING_FRAMES);
    *hostTime = anchor + (uint64_t)((Float64)wraps * period);
    *seed = 1 + seq / 2;
    return 0;
}
static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, Boolean *will, Boolean *inPlace) {
    (void)d; (void)o; (void)client;
    *will = op == kAudioServerPlugInIOOperationWriteMix || op == kAudioServerPlugInIOOperationReadInput;
    *inPlace = true;
    return 0;
}
static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *info) {
    (void)d; (void)o; (void)client; (void)op; (void)frames; (void)info; return 0;
}
static inline int16_t toS16(Float32 v) {v *= 32767.0f; return (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);}
static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, AudioObjectID stream, UInt32 client, UInt32 op, UInt32 frames,
                              const AudioServerPlugInIOCycleInfo *info, void *main, void *secondary) {
    (void)d; (void)o; (void)client; (void)secondary;
    if (!gControl) return 0;
    if (op == kAudioServerPlugInIOOperationWriteMix && stream == kOutStream) {
        const Float32 *src = main;
        uint64_t t = (uint64_t)info->mOutputTime.mSampleTime;
        const Float32 gain = gVolume;
        for (UInt32 i = 0; i < frames; ++i, ++t) {
            int16_t *dst = gOut + 2 * (t % SHENG_AUDIO_RING_FRAMES);
            dst[0] = toS16(src[2 * i] * gain);
            dst[1] = toS16(src[2 * i + 1] * gain);
        }
    } else if (op == kAudioServerPlugInIOOperationReadInput && stream == kInStream) {
        Float32 *dst = main;
        uint64_t t = (uint64_t)info->mInputTime.mSampleTime;
        for (UInt32 i = 0; i < frames; ++i, ++t) dst[i] = gIn[t % SHENG_AUDIO_RING_FRAMES] / 32768.0f;
    }
    return 0;
}
static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID o, UInt32 client, UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *info) {
    (void)d; (void)o; (void)client; (void)op; (void)frames; (void)info; return 0;
}

static AudioServerPlugInDriverInterface gInterface = {
    NULL, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice, AddDeviceClient, RemoveDeviceClient,
    PerformDeviceConfigurationChange, AbortDeviceConfigurationChange, HasProperty, IsPropertySettable, GetPropertyDataSize,
    GetPropertyData, SetPropertyData, StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginIOOperation, DoIOOperation, EndIOOperation};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;

static HRESULT QueryInterface(void *self, REFIID uuid, LPVOID *out) {
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID)) {
        AddRef(self);
        *out = &gInterfacePtr;
        r = S_OK;
    }
    CFRelease(requested);
    return r;
}

__attribute__((visibility("default"))) void *ShengAudio_Create(CFAllocatorRef allocator, CFUUIDRef type) {
    (void)allocator;
    if (!CFEqual(type, kAudioServerPlugInTypeUUID)) return NULL;
    if (!gHostTicksPerFrame) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        gHostTicksPerFrame = 1e9 / kRate * tb.denom / tb.numer;
    }
    return &gInterfacePtr;
}
