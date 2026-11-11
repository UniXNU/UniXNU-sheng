// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// The host half of VideoToolbox's paravirtualized decoder and encoder, in the guest's own kernel: what a Mac running an Apple
// virtual machine answers, played over the board's stateful V4L2 decoder device (IrisMedia.cpp, the iris VPU). The encoder's half is described where it begins ("The encoder").
// VtParavirt.cpp is the transport (Apple's AppleVideoToolboxParavirtualizationDriver user client) and hands every message
// here. The messages and their parameters are those a loopback of VideoToolbox's own guest and host halves exchanges on macOS 27
// (VtWire.h for the format):
//   decs   the decoder list: one entry per coded format the iris media decoder offers that VideoToolbox can describe (H.264,
//          HEVC). The guest registers each as "paravirtualized:<CMClassImplementationID>" (rating 400, above its software ones).
//   +dec   a decoder instance (,cod the codec type, ,cid the ID): answered with ,err ,ver ,hin.
//   dset dcop dsup   properties: taken; the supported list (dsup) names UsingHardwareAcceleratedVideoDecoder alone, which dcop
//          answers true (esup/ecop: the encoder's, UsingHardwareAcceleratedVideoEncoder).
//   dsta   the session's format (,fds an ImageDescription with avcC or hvcC): a iris media session is opened, its OUTPUT queue set
//          up and streaming; the guest is told the pixel buffers it should allocate (Dpba, 420v of the coded size) before the
//          answer. What the device cannot decode (outside its frame sizes, not 4:2:0) is refused here, and VideoToolbox takes its
//          software decoder instead: refused later, frame by frame, it would not.
//   dfrm   a frame (,frm its ID, ,sbf the sample buffer): its NAL units, from length-prefixed to Annex B (the parameter sets
//          before the first), kept for the worker, which puts it in a free OUTPUT buffer with the frame ID as its timestamp;
//          answered at once, asynchronous (,inf 1).
//   dfin   answered by the worker once the frames given so far are handed over (at most two seconds later): a codec keeps
//          frames back (the codec holds even a lone IDR), so the decoder is drained (V4L2_DEC_CMD_STOP, until a LAST buffer
//          or EOS) and started again (V4L2_DEC_CMD_START); dinv tears the V4L2 session down; -dec forgets the instance.
// Nothing here waits, on the guest's thread, for anything the guest must answer first: VideoToolboxParavirtualizationSupport
// holds its lock across the whole send (selector 3), so a wait there for a Dbuf reply the guest cannot send would hang both.
// The session's worker thread takes the device's events: SOURCE_CHANGE sets the CAPTURE queue up (G_FMT, REQBUFS MMAP, every
// buffer queued, STREAMON); each decoded CAPTURE buffer asks the guest for a pixel buffer (Dbuf, the host's request; the guest's
// reply carries the pixel buffer's UUID and its IOSurface), copies the NV12 into that IOSurface, hands it over (Demt) and queues
// the CAPTURE buffer again.
//
// The IOSurface arrives as an ID. An IOSurfaceRootUserClient the kernel opens for the guest process looks it up (the process's
// own refuses surfaces it already holds; an IOUserClient2022's 2022 externalMethod; selector 4, IOSurface.framework's
// IOSurfaceClientLookup: a 0xc68-byte lock result whose fields IOSurfaceClientGet* read, at the client object's offsets less
// 0x70), which maps it into that process and reports where and how its planes lie; the copy goes through that mapping. The
// client is the session's: a lookup of a surface it already holds is refused, and the guest's pool hands the same few surfaces
// out again and again, so what each lookup found is kept by surface ID (the client's reference keeps the surface, and so its
// ID, alive), and closing the client at the session's end gives every reference and mapping back. A frame to encode is the app's
// surface, handed to the codec process over XPC: IOSurfaceRoot lets a client look a surface up only when the client's task made
// it, it is global, or the client knows it already, so it goes as IOSurfaceLookupFromMachPort does, through a send right made by
// the process's own client (which knows it; found by its IOUserClientCreator) and looked up by the kernel's (selectors 0x23,
// 0x22); the right is dropped from the process's space at once.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSString.h>
#include <kern/thread.h>
#include <stdarg.h>
#include "VtWire.h"
#include "IrisMedia.h"
#include <pexpert/pexpert.h>

using namespace vtwire;

extern "C" {

int proc_selfpid(void);
ipc_space_t get_task_ipcspace(task_t task);
kern_return_t mach_port_deallocate(ipc_space_t space, mach_port_name_t name);

// For the boot log: sessions made, frames given, frames emitted, errors; the last error's stage and value.
volatile uint32_t vt_host_sessions, vt_host_frames_in, vt_host_frames_out, vt_host_errors;
volatile uint32_t vt_host_frames_dropped;   // handed over as dropped (frame thinning), no pixels
volatile uint32_t vt_host_frames_skipped;   // of those, never given to the device (a temporal layer above the kept ones)
volatile uint32_t vt_host_error_stage;
volatile int32_t vt_host_error_value;
volatile uint32_t vt_host_replies, vt_host_unknown, vt_host_last_unknown;
// The host's last diagnostic lines, a ring of 16 (the kernel's message buffer turns over too fast to keep them).
char vt_host_trace[16][112];
volatile uint32_t vt_host_trace_count;
}

namespace {

void trace(const char *format, ...) __attribute__((format(printf, 1, 2)));
void trace(const char *format, ...) {
    const uint32_t n = __atomic_fetch_add(&vt_host_trace_count, 1, __ATOMIC_RELAXED);
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(vt_host_trace[n % 16], sizeof vt_host_trace[0], format, arguments);
    va_end(arguments);
}

void failed(uint32_t stage, int32_t value) {
    vt_host_errors = vt_host_errors + 1;
    vt_host_error_stage = stage;
    vt_host_error_value = value;
    if (vt_host_errors < 40) IOLog("vt: stage %u failed: %d\n", stage, value);
}

// The board offers no encoder device (IrisMedia.h has the decoder alone): the encoder's half is compiled out, which keeps its code
// and strings out of the kernel's text, and an encoder session is refused as when no device answers.
constexpr bool VtEncoder = false;

// --- V4L2 over iris media (64-bit Linux layouts) ----------------------------------------------------------------------------
enum : uint32_t {
    IocGFmt = 4, IocSFmt = 5, IocReqbufs = 8, IocQuerybuf = 9, IocQbuf = 15, IocStreamon = 18, IocStreamoff = 19, IocGCtrl = 27,
    IocSCtrl = 28,
    IocSubscribeEvent = 90, IocDecoderCmd = 96, IocEnumFramesizes = 74,
    TypeCaptureMplane = 9, TypeOutputMplane = 10, MemoryMmap = 1, MemoryUserPtr = 2,
    EventEos = 2, EventSourceChange = 5,
    BufFlagLast = 0x00100000, BufFlagError = 0x40,
    CidMinBuffersForCapture = 0x00980927,
    CidDecDisplayDelay = 0x00990b8d, CidDecDisplayDelayEnable = 0x00990b8e,   // V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY(_ENABLE)
    DecCmdStart = 0, DecCmdStop = 1,
    MediaEvtError = 0, MediaEvtDqbuf = 1, MediaEvtEvent = 2,
};
struct V4l2Buffer {   // struct v4l2_buffer, then one struct v4l2_plane
    uint32_t index, type, bytesused, flags, field, pad0;
    int64_t sec, usec;
    uint8_t timecode[16];
    uint32_t sequence, memory;
    uint64_t m;
    uint32_t length, reserved2, request_fd, pad1;
    struct { uint32_t bytesused, length; uint64_t m; uint32_t data_offset, reserved[11]; } plane;
};
static_assert(sizeof(V4l2Buffer) == 88 + 64, "v4l2_buffer + v4l2_plane");
struct V4l2Format {   // struct v4l2_format with v4l2_pix_format_mplane
    uint32_t type, pad;
    uint32_t width, height, pixelformat, field, colorspace;
    struct { uint32_t sizeimage, bytesperline; uint16_t reserved[6]; } planes[8];
    uint8_t num_planes, flags, ycbcr, quantization, xfer, reserved[7];
    uint8_t rest[200 - 192];
};
static_assert(sizeof(V4l2Format) == 208, "v4l2_format");

constexpr unsigned OutputBuffers = 4, MaxCaptureBuffers = 32, MaxInputs = 4096;
// CAPTURE buffers queued at once. the reference decoder asks for 12 (MIN_BUFFERS_FOR_CAPTURE) and is given 14, but decodes the aerials with
// 6 queued (5 with layers 0-1; none with 4). Direct, each held a 4K P010 pool surface
// (26.7 MB) of the player's: four sessions' 14 were the aerials' 1.49 GB, killed at its memory limit (11-04 on the reference device).
constexpr unsigned CaptureInFlight = 6;
// Synchronous dfrms waiting for their frame (the guest sends the next only once one is answered: one at a time per thread).
constexpr unsigned MaxSyncReplies = 8;
// Asynchronous frames given and not handed over before a dfrm's reply waits (Apple's host: at most 3 in flight).
constexpr unsigned AsyncInFlight = 3;
constexpr uint32_t AsyncDemtMs = 2000;
constexpr uint32_t IdleCloseMs = 1000;   // workerMain: the lock screen's player opens a new session every 3 s or so
constexpr uint32_t FinishMs = 2000;   // a dfin with no frame coming out for this long drops what is left (answerFinish)
// Device sessions held at once (makeRoom). The lock screen's player starts a session for every clip and lets the old ones go
// only much later; each held a 4K decoder on the host (one of the reference decoder's eight) and, copying, 14 CAPTURE buffers of 26.7 MB in
// the guest -- five of them left VideoToolbox no memory for pixel buffers, its Dbuf answers stopped, no session's frames came
// out and none went quiet enough to be closed. A new session closes the least recently fed other
// one with every frame handed over once SoftDeviceSessions are open and that one has had no frame for EvictIdleMs, or whatever
// its age once HardDeviceSessions are (makeRoom); a session closed so starts again at its next sync sample, or from the frames
// since its last (MaxReplay).
constexpr unsigned SoftDeviceSessions = 2, HardDeviceSessions = 3;
constexpr uint32_t EvictIdleMs = 500;
// A session let go is drained first (its last frames, which the codec keeps for reordering till the stream ends, handed over with
// pixels): for at most EvictDrainMs, and only one with at most EvictFrames still to hand over. The player gives no dfin till much
// later, and these frames kept every session from counting as done: eight open, the aerials' pool surfaces 1.22 GB (11-04).
constexpr uint32_t EvictDrainMs = 500;
// A session given nothing for IdleDrainMs with frames still inside the codec is drained (workerMain): the codec keeps the last
// few back for reordering till more come, and the lock screen's player reads a clip in at once and lets the session go once it
// has what it waited for -- a frame handed over after that ended the session inside the Demt, under the decoder's lock, and its
// VTDecompressionSessionInvalidate took that lock again: os_unfair_lock_recursive_abort (11-04 on the reference device, v6). After the
// drain the device goes on (DEC_CMD_START) from the next frame, a few frames flagged ERROR (handed over dropped).
// IdleDrainMs, EvictDrainMs and MaxBacklog are for display order only (a device without the display delay control: the codec,
// a device without the control): in decode order every frame comes out as soon as decoded and a synchronous player waits for each (dfrm).
constexpr uint32_t IdleDrainMs = 200;
constexpr unsigned EvictFrames = 16;
// Frames waiting for an OUTPUT buffer (queueFrame): past MaxBacklog the session is behind (its worker waiting on the guest, the
// device slower than the stream) and sheds one more temporal layer (keptLayer), at most every ShedMs -- never a frame another
// refers to: the aerials' clips have one sync sample, their first frame, and a stretch dropped up to the next one was the rest
// of the clip black (11-04 on the reference device).
constexpr unsigned MaxBacklog = 32;
// The frames since the last sync sample kept to start a session let go again from (queueFrame): the next clip's session, opened
// ahead with its first frame, then let go for a third -- the clip's one sync sample gone with it, the clip was black (11-04).
constexpr unsigned MaxReplay = 16;
constexpr uint32_t MaxReplayBytes = 16 << 20;
constexpr uint32_t ShedMs = 500;
constexpr uint32_t DbufPauseMs = 500;
// Frame thinning. The lock screen's aerials are 4K HEVC at 240 frames a second (idleassetsd's 4KSDR240FPS); its player decodes
// every frame and shows about 30 of them a second, and nothing in AVFoundation asks for fewer (ReducedFrameDelivery is set by no
// one but VideoToolbox's own callers). A pixel buffer and a 4K copy for each of the 240 is ten times what this path does, so the
// player falls behind, fills MaxInputs and starts over. A frame whose presentation time is less than ThinUs after the last one
// handed over with pixels goes back as dropped (kVTDecodeInfo_FrameDropped, no pixel buffer): the references it is to the
// stream stay in the device's decoder, which is the only place they are needed.
constexpr int64_t ThinUs = 31000;   // a 30 Hz picture, less a little: every 8th frame of 240
constexpr unsigned FrameTimes = 256;   // frames given to the device and not yet out: OUTPUT + the codec's delay + CAPTURE   // Qualcomm's AVC decoder asks 21 output slots at 720p
constexpr uint32_t OutputBytes = 2u << 20;

// --- IOSurface, through the guest process's own IOSurfaceRootUserClient ----------------------------------------------------
struct SurfaceView {
    uint64_t base = 0, size = 0;
    uint32_t id = 0, format = 0, planes = 0;
    struct { uint64_t width, height, offset, bytesPerRow; } plane[2] = {};
};
constexpr uint32_t LockResultBytes = 0xc68;

IOService *surfaceRoot() {
    static IOService *root;
    if (!root) {
        // by class: iPadOS names the service IOCoreSurfaceRoot, macOS leaves it the class's name
        OSDictionary *matching = IOService::serviceMatching("IOSurfaceRoot");
        IOService *found = matching ? IOService::waitForMatchingService(matching, 1000000000ULL) : nullptr;
        OSSafeReleaseNULL(matching);
        if (found && !OSCompareAndSwapPtr(nullptr, found, &root)) found->release();
    }
    return root;
}

// An IOSurfaceRootUserClient of the kernel's own for the guest process `task`, as IOSurface.framework opens one (type 0): the
// process's own client already holds the surfaces it made, and answers a lookup of one with kIOReturnBadArgument (the framework
// asks the kernel only for surfaces it does not know), while a second client of the same task looks any of them up.
IOUserClient *openSurfaceClient(task_t task) {
    IOService *root = surfaceRoot();
    if (!root) return nullptr;
    IOUserClient *client = nullptr;
    const IOReturn r = root->newUserClient(task, task, 0, nullptr, &client);
    trace("surface client: %#x %s", r, client ? client->getMetaClass()->getClassName() : "none");
    if (r != kIOReturnSuccess) { OSSafeReleaseNULL(client); return nullptr; }
    return client;
}

// IOSurface.framework's selectors (IOSurfaceRootUserClient, iPadOS 27).
enum : uint32_t { SurfaceLookup = 4, SurfaceLookupFromPort = 0x22, SurfaceCreatePort = 0x23 };

IOReturn surfaceMethod(IOUserClient *client, uint32_t selector, const uint64_t *input, uint32_t inputCount, uint64_t *output,
                       uint32_t outputCount, void *structure, uint32_t structureSize) {
    IOExternalMethodArguments arguments = {};
    arguments.version = kIOExternalMethodArgumentsCurrentVersion;
    arguments.selector = selector;
    arguments.scalarInput = input;
    arguments.scalarInputCount = inputCount;
    arguments.scalarOutput = output;
    arguments.scalarOutputCount = outputCount;
    arguments.structureOutput = structure;
    arguments.structureOutputSize = structureSize;
    // IOSurfaceRootUserClient is an IOUserClient2022, whose legacy externalMethod panics; a client the kernel opened itself has
    // not been through is_io_service_open_extended, which tells callExternalMethod so, hence the 2022 method, directly.
    IOUserClient2022 *client2022 = OSDynamicCast(IOUserClient2022, client);
    return client2022 ? client2022->externalMethod(selector, reinterpret_cast<IOExternalMethodArgumentsOpaque *>(&arguments))
                      : client->callExternalMethod(selector, &arguments);
}

// The IOSurfaceRootUserClient the process `pid` opened itself (IOSurface.framework's), retained: the one that holds the surfaces
// the process was given by another (a frame to encode comes from the app, through XPC), which IOSurfaceRoot lets a client look
// up only when the client knows the surface already, it is global, or the client's task made it.
IOUserClient *processSurfaceClient(int pid) {
    IOService *root = surfaceRoot();
    OSIterator *clients = root ? root->getClientIterator() : nullptr;
    if (!clients) return nullptr;
    char prefix[24];
    snprintf(prefix, sizeof prefix, "pid %d,", pid);
    IOUserClient *found = nullptr;
    while (OSObject *object = clients->getNextObject()) {
        IOUserClient *client = OSDynamicCast(IOUserClient, object);
        OSString *creator = client ? OSDynamicCast(OSString, client->getProperty(kIOUserClientCreatorKey)) : nullptr;
        if (creator && !strncmp(creator->getCStringNoCopy(), prefix, strlen(prefix))) {
            found = client;
            found->retain();
            break;
        }
    }
    clients->release();
    return found;
}

bool lookupSurface(IOUserClient *client, uint32_t id, SurfaceView &view, IOUserClient *holder = nullptr) {
    uint8_t *result = static_cast<uint8_t *>(IOMallocZero(LockResultBytes));
    if (!result) return false;
    IOReturn status;
    if (holder) {
        // A surface this process was given: a send right to it made by the client that holds it, in the process (whose thread
        // this is), and looked up through that, as IOSurfaceLookupFromMachPort does; the right is the process's to drop.
        const uint64_t create[2] = {id, 0};
        uint64_t port = 0;
        status = surfaceMethod(holder, SurfaceCreatePort, create, 2, &port, 1, nullptr, 0);
        if (status == kIOReturnSuccess) {
            status = surfaceMethod(client, SurfaceLookupFromPort, &port, 1, nullptr, 0, result, LockResultBytes);
            mach_port_deallocate(get_task_ipcspace(current_task()), mach_port_name_t(port));
        } else {
            failed(42, status);
        }
    } else {
        const uint64_t input = id;
        status = surfaceMethod(client, SurfaceLookup, &input, 1, nullptr, 0, result, LockResultBytes);
    }
    bool good = status == kIOReturnSuccess;
    if (good) {
        auto u64 = [&](uint32_t at) { uint64_t v; memcpy(&v, result + at, 8); return v; };
        auto u32 = [&](uint32_t at) { uint32_t v; memcpy(&v, result + at, 4); return v; };
        view.base = u64(0x00);
        view.id = u32(0x18);
        view.size = u64(0x20);
        view.format = u32(0x48);
        view.planes = u32(0x58);
        for (uint32_t p = 0; p < 2 && p < view.planes; ++p) {
            const uint32_t at = 0x88 + p * 0x98;
            view.plane[p] = {u64(at), u64(at + 0x08), u64(at + 0x18), u64(at + 0x20)};
        }
        good = view.id == id && view.base && view.size && view.planes == 2;
        if (!good) failed(40, int32_t(view.planes));
    } else {
        failed(41, status);
    }
    trace("lookup %u: %#x planes %u base %#llx size %#llx", id, status, view.planes, view.base, view.size);
    IOFree(result, LockResultBytes);
    return good;
}


// --- Sessions ---------------------------------------------------------------------------------------------------------------
struct Client;

// A buffer of a queue: guest memory of ours the device reads or writes in place (USERPTR), given to it by the scatter list of its
// pages at every QBUF. Not MMAP: a host buffer comes out of the media_host pool, which a virtual device shares once at boot and two 4K
// decoders (a lock screen's video and its still) outgrew; ours come from the guest's memory, as many as the codec asks.
struct Mapped {
    uint64_t length = 0;
    IOBufferMemoryDescriptor *buffer = nullptr, *sg = nullptr;   // the bytes; their {start, length} list (physically contiguous)
    uint64_t sgAddress = 0;
    uint32_t sgBytes = 0, sgCapacity = 0;   // sgCapacity: what m.sg holds (a direct slot keeps its list from surface to surface)
    bool sharedSg = false;                  // m.sg is one of gSgLists', given back, not freed
    uint8_t *bytes = nullptr;
    bool queued = false;
    // A CAPTURE buffer that is one of the guest's pool surfaces (Session::direct): its pages in the guest process, wired; the
    // pixel buffer's UUID, which the frame decoded into it is handed over (Demt) with.
    IOMemoryDescriptor *memory = nullptr;
    uint8_t uuid[16] = {};
};

struct Event {
    uint32_t kind, type, index, bytesused, flags;
    int64_t sec;
};

struct Session {
    Session *next = nullptr;
    Client *client = nullptr;
    uint8_t uuid[16] = {};
    uint32_t codec = 0, width = 0, height = 0;
    bool encoder = false;
    uint32_t nalLength = 4;
    bool tenBit = false;   // hvcC's bitDepthLumaMinus8 above 0: the pixel buffers asked for are x420
    // Temporal layers (hvcC's numTemporalLayers; 0 unknown, 1 none). The lock screen's aerials are five: TID 0-1 are 30 of the
    // 240 frames a second, and no frame of a lower layer refers to a higher one (any sub-layer cut is a stream), so the layers
    // above the ones that make about 30 a second are never given to the device: neither decoded nor copied (Z0).
    uint8_t temporalLayers = 0;
    int64_t lastInputUs = INT64_MIN, minStepUs = INT64_MAX;   // the frame period, the least step between two inputs' times
    unsigned steps = 0;
    bool skipLogged = false;
    uint8_t parameterSets[512] = {};
    uint32_t parameterSetsSize = 0;
    bool parametersSent = false;

    unsigned device = ~0u;
    uint32_t v4l2 = 0;
    bool open = false, outputStreaming = false, captureStreaming = false, fast = false;
    Mapped output[OutputBuffers];
    Mapped capture[MaxCaptureBuffers];
    unsigned captureCount = 0;
    uint32_t captureWidth = 0, captureHeight = 0, captureStride = 0;
    bool captureP010 = false;   // CAPTURE is P010 (16 bits a sample, the value in the top 10), not NV12: a 10-bit stream on the reference decoder
    // P3: the CAPTURE buffers are the guest's own pool surfaces, taken ahead with Dbuf (which the guest answers from the pool,
    // whatever the frame), the device decoding into them in place: a decoded frame goes over with its buffer's UUID, no copy,
    // and the slot takes another surface; a frame thinned out keeps its surface. Unused ones go back with Dfub. Only where the
    // surface's planes lie as the device's single-plane layout has them (the luma rows the device pads to, the chroma right
    // after): Dpba asks for ExtendedPixelsBottom up to the padded height.
    bool direct = false;
    uint32_t extendBottom = 0;   // the ExtendedPixelsBottom the last Dpba asked for

    IOLock *lock = nullptr;
    // Frames given (dfrm) and not yet in an OUTPUT buffer: Annex B, IOMalloc'd.
    // The frames sent and not yet given to the device. MaxInputs, not 128: the lock screen's video player sends 100-200 frames at
    // once when a session starts; refused at 128 (-12911), VideoToolbox opened another session without invalidating the first, every
    // few seconds, and each kept a 4K hardware decoder and its buffers on the host (500 MB apiece) until the host ran out of memory.
    struct Input { int64_t frame; uint8_t *bytes; uint32_t size, capacity; int64_t ptsUs; uint8_t tid; } inputs[MaxInputs];
    // The presentation times (microseconds, -1 unknown) of the frames given to the device, by frame % FrameTimes; and that of the
    // last frame handed over with pixels (INT64_MIN: none yet).
    struct FrameTime { int64_t frame = -1, ptsUs = -1; } frameTimes[FrameTimes];
    int64_t lastShownUs = INT64_MIN;
    unsigned inputHead = 0, inputTail = 0;
    // An outstanding dfin: its request, answered when pendingFrames is 0 or at finishDeadline.
    bool finishWanted = false, draining = false;
    uint64_t finishRequest = 0;
    uint64_t finishDeadline = 0;
    uint32_t pendingAtDeadline = 0;   // pendingFrames when finishDeadline was last set: fewer since, the deadline moves on
    Event events[64];
    unsigned eventHead = 0, eventTail = 0;
    bool stopping = false, workerRunning = false;
    // The device session let go while VideoToolbox's lives on (workerMain): the dsta that started it, to start it again with.
    bool idleClosed = false, finished = false;
    // evict: makeRoom (or a device error) asks the worker to let the device session go as when idle; resync: frames are
    // dropped until a sync sample, which (re)starts the device session if it was let go.
    bool evict = false, resync = false;
    uint64_t evictDrainUntil = 0;   // evicted with frames inside the codec: drained first, till then (evictNow)
    bool idleDrained = false;   // drained for having been given nothing for IdleDrainMs, since the last frame given
    // Decode order (the device's display delay 0, set at startSession): a frame out for every one given, as soon as it is
    // decoded -- what Apple's host does. Only then is a synchronous dfrm answered after its frame's Demt (syncReplies); in
    // display order the device keeps frames back till later ones come, and a player waiting on each would wait for ever.
    bool decodeOrder = false;
    // demt: its Demt's request, sent from the guest's message thread (not waited for there): the dfrm answered when the Demt is.
    // async: an asynchronous dfrm's, held while AsyncInFlight frames are out (releaseAsync), not tied to its frame.
    struct SyncReply { int64_t frame; uint64_t request, demt; bool async; } syncReplies[MaxSyncReplies];
    unsigned syncReplyCount = 0;
    // Demts of asynchronous frames sent and not yet answered by the guest (request, when sent): out still, as VideoToolbox sees
    // it (its queue behind). One unanswered for AsyncDemtMs no longer counts (an answer lost must not hold the player for ever).
    struct SentDemt { uint64_t request, at; } sentDemts[32];
    unsigned sentDemtCount = 0;
    bool dflLogged = false;
    // For the life log at its end (logEnd): frames given to the device, handed over with pixels, handed over dropped; when the
    // last went out with pixels.
    uint32_t statIn = 0, statOut = 0, statDropped = 0;
    uint64_t lastOutAt = 0;
    uint8_t shed = 0;          // temporal layers given up below keptLayer's own choice because the session fell behind
    uint64_t shedAfter = 0;    // the next shedding not before this (mach_absolute_time)
    uint64_t resumeAfter = 0;   // a start again the device refused: the next try not before this (mach_absolute_time)
    uint64_t dbufPauseUntil = 0;   // emitFrame: no Dbuf before this, after one went unanswered
    uint8_t *startMessage = nullptr;
    uint32_t startSize = 0;
    // The frames given to the device since the last sync sample (Annex B, no parameter sets), while MaxReplay and MaxReplayBytes
    // hold them all (replayable): a session let go with these behind it starts again from them, at any frame (queueFrame).
    struct Saved { int64_t frame; uint8_t *bytes; uint32_t size; int64_t ptsUs; uint8_t tid; } replay[MaxReplay];
    unsigned replayCount = 0;
    uint32_t replaySize = 0;
    bool replayable = false;
    uint64_t lastInputAt = 0;   // mach_absolute_time of the last dfrm queued
    thread_t worker = nullptr;
    uint32_t pendingFrames = 0;
    // The frames given and not yet handed over, by ID: at dfin's deadline each gets a Demt with an error, or VideoToolbox would
    // wait for it forever.
    int64_t outstanding[MaxInputs];
    unsigned outstandingCount = 0;

    // The session's IOSurfaceRootUserClient (opened at its first pixel buffer) and the surfaces it looked up, by ID.
    IOUserClient *surfaces = nullptr;
    IOUserClient *holder = nullptr;   // the process's own client (an encoder's frames are the app's)
    IOLock *surfaceLock = nullptr;
    struct { uint32_t id; SurfaceView view; } known[48];
    unsigned knownCount = 0;

    // An encoder's frames (efrm), in the order given: the frame ID, its pixel buffer's UUID, its times, its IOSurface; fed once
    // copied into an OUTPUT buffer, forgotten once handed over (Eemt).
    struct Frame {
        int64_t frame;
        uint8_t uuid[16];
        uint8_t pts[24], duration[24];   // CMTime, as the 'para' carried it (big endian)
        SurfaceView view;
        bool fed;
    } frames[64];
    unsigned frameHead = 0, frameTail = 0;
    // The stream's parameter sets (Annex B NAL units, without start codes) and whether the guest has the description made of them.
    uint8_t sps[256] = {}, pps[256] = {}, vps[256] = {};
    uint32_t spsSize = 0, ppsSize = 0, vpsSize = 0;
    bool describedToGuest = false;

    // The host's own requests to the guest (Dbuf, Demt) and the guest's replies to them.
    uint64_t nextHostRequest = 100;
    uint64_t awaitedRequest = 0;
    bool replied = false;
    int32_t replyError = 0;
    uint8_t replyUuid[16] = {};
    bool replyHasSurface = false;
    SurfaceView replySurface;
};

struct Client {
    void *transport;
    bool (*send)(void *transport, const uint8_t *bytes, uint32_t size);
    task_t task;
    int pid;
    IOLock *lock;
    // The sessions list, against the guest's replies (vt_host_message), which come on another guest thread than its
    // requests and take no `lock` (a dinv holds that while its worker waits): a -dec freed the session a reply had found.
    IOLock *listLock;
    Session *sessions;
};

// Every session the device's events may name, by V4L2 session ID.
IOLock *gTableLock;
Session *gTable[32];

void registerSession(Session *s) {
    IOLockLock(gTableLock);
    for (Session *&slot : gTable) if (!slot) { slot = s; break; }
    IOLockUnlock(gTableLock);
}
void unregisterSession(Session *s) {
    IOLockLock(gTableLock);
    for (Session *&slot : gTable) if (slot == s) slot = nullptr;
    IOLockUnlock(gTableLock);
}

// The device's events (with its lock held): copied into their session's ring.
void mediaSink(void *context, const uint8_t *event, uint32_t size) {
    const unsigned device = unsigned(uintptr_t(context));
    if (size < 8) return;
    uint32_t kind, session;
    memcpy(&kind, event, 4);
    memcpy(&session, event + 4, 4);
    Event e = {kind, 0, 0, 0, 0, 0};
    if (kind == MediaEvtDqbuf && size >= 8 + sizeof(V4l2Buffer)) {
        V4l2Buffer b;
        memcpy(&b, event + 8, 88);
        memcpy(&b.plane, event + 8 + 88, sizeof b.plane);
        e.type = b.type; e.index = b.index; e.bytesused = b.plane.bytesused; e.flags = b.flags; e.sec = b.sec;
    } else if (kind == MediaEvtEvent && size >= 12) {
        memcpy(&e.type, event + 8, 4);
    }
    IOLockLock(gTableLock);
    for (Session *s : gTable) {
        if (!s || !s->open || s->device != device || s->v4l2 != session) continue;
        IOLockLock(s->lock);
        if (s->eventHead - s->eventTail < 64) s->events[s->eventHead++ % 64] = e;
        IOLockUnlock(s->lock);
        IOLockWakeup(s->lock, &s->events, false);
    }
    IOLockUnlock(gTableLock);
}

unsigned decoderDevice() {
    static unsigned device = ~0u;
    if (device != ~0u) return device;
    for (unsigned d = 0; d < iris_media_devices(); ++d) {
        if (!iris_media_is_up(d) || !strnstr(iris_media_card(d), "decoder", 33)) continue;
        iris_media_set_sink(d, mediaSink, reinterpret_cast<void *>(uintptr_t(d)));
        device = d;
        break;
    }
    return device;
}

unsigned encoderDevice() {
    static unsigned device = ~0u;
    if (device != ~0u) return device;
    for (unsigned d = 0; d < iris_media_devices(); ++d) {
        if (!iris_media_is_up(d) || !strnstr(iris_media_card(d), "encoder", 33)) continue;
        iris_media_set_sink(d, mediaSink, reinterpret_cast<void *>(uintptr_t(d)));
        device = d;
        break;
    }
    return device;
}

int ioctl(Session *s, uint32_t code, void *payload, uint32_t out, uint32_t in) {
    return iris_media_ioctl(s->device, s->v4l2, code, payload, out, in);
}

struct SgEntry { uint64_t start; uint32_t length, padding; };   // one run of a buffer's pages

// The lists of direct slots: physically contiguous (the device reads each from one descriptor), about 100 KB for a 4K P010
// frame whose pages are all apart, and a guest that has been up a while has no such run free; made once and kept for every
// session after (MaxSgLists of them, any session's slots).
constexpr unsigned MaxSgLists = 64;
constexpr uint32_t SgListBytes = 128u << 10;   // 8192 entries: 32 MiB of pages
IOLock *gSgLock;
IOBufferMemoryDescriptor *gSgLists[MaxSgLists];
bool gSgBusy[MaxSgLists];

IOBufferMemoryDescriptor *takeSgList() {
    IOLockLock(gSgLock);
    IOBufferMemoryDescriptor *list = nullptr;
    for (unsigned i = 0; i < MaxSgLists && !list; ++i) {
        if (gSgBusy[i]) continue;
        if (!gSgLists[i])
            gSgLists[i] = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, IOOptionBits(kIODirectionOut) |
                                                                           IOOptionBits(kIOMemoryPhysicallyContiguous), SgListBytes,
                                                                           0x0000fffffffff000ULL);
        if (gSgLists[i]) { gSgBusy[i] = true; list = gSgLists[i]; }
        else break;
    }
    IOLockUnlock(gSgLock);
    return list;
}

void giveSgList(IOBufferMemoryDescriptor *list) {
    IOLockLock(gSgLock);
    for (unsigned i = 0; i < MaxSgLists; ++i) if (gSgLists[i] == list) gSgBusy[i] = false;
    IOLockUnlock(gSgLock);
}

void unmapBuffer(Session *s, Mapped &m) {
    if (m.buffer) { m.buffer->complete(); m.buffer->release(); }
    if (m.memory) { m.memory->complete(); m.memory->release(); }
    if (m.sharedSg && m.sg) { giveSgList(m.sg); m.sg = nullptr; }
    OSSafeReleaseNULL(m.sg);
    m = Mapped();
}

// Buffer `index` of the queue (after REQBUFS with USERPTR): as long as the device says (QUERYBUF's plane length, the queue's
// sizeimage, which a USERPTR buffer must give exactly), wired, and the list of its pages, adjacent ones merged.
// Pageable kernel memory that prepare() wires: the wiring waits for the pageout daemon to reclaim and compress pages where few are
// free. A wired allocation (inTaskWithOptions without kIOMemoryPageable) does not: at the lock screen, with 20-50 MB free, every
// 4K buffer (13 MB capture, 6.6 MB bitstream) came back null, and VideoToolbox fell back to software decoding.
bool scatter(Mapped &m, IOMemoryDescriptor *memory);
bool mapBuffer(Session *s, uint32_t type, uint32_t index, Mapped &m) {
    V4l2Buffer b = {};
    b.index = index; b.type = type; b.memory = MemoryUserPtr; b.length = 1;
    int r = ioctl(s, IocQuerybuf, &b, sizeof b, sizeof b);
    if (r || !b.plane.length) { failed(10, r ? r : -1); return false; }
    m.length = b.plane.length;
    m.buffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, IOOptionBits(kIODirectionInOut) | IOOptionBits(kIOMemoryPageable),
                                                           m.length, 16384);
    if (!m.buffer || m.buffer->prepare() != kIOReturnSuccess) { OSSafeReleaseNULL(m.buffer); failed(11, int32_t(m.length)); return false; }
    m.bytes = static_cast<uint8_t *>(m.buffer->getBytesNoCopy());
    if (!scatter(m, m.buffer)) { unmapBuffer(s, m); return false; }
    return true;
}

// The list of the pages of `memory` (prepared), adjacent ones merged, for the device: m.sg, m.sgAddress, m.sgBytes.
bool scatter(Mapped &m, IOMemoryDescriptor *memory) {
    // The device maps a queued buffer from its memory descriptor (iris_media_qbuf): no list of pages is made here.
    return memory != nullptr && m.length != 0;
}

// QBUF of a buffer of ours: the v4l2_buffer names it by its kernel address (the device only echoes it) and its pages follow.
int queueBuffer(Session *s, V4l2Buffer &b, const Mapped &m) {
    b.memory = MemoryUserPtr;
    b.length = 1;
    b.plane.m = uint64_t(uintptr_t(m.bytes));
    b.plane.length = uint32_t(m.length);
    return iris_media_qbuf(s->device, s->v4l2, &b, m.buffer ? static_cast<IOMemoryDescriptor *>(m.buffer) : m.memory);
}

// The parameter sets of an avcC or hvcC, as Annex B, and the NAL length size.
bool parameterSets(Session *s, const ImageDescription &d) {
    s->tenBit = false;
    s->temporalLayers = 0;
    Atom ext;
    const uint8_t start[4] = {0, 0, 0, 1};
    s->parameterSetsSize = 0;
    auto add = [&](const uint8_t *nal, uint32_t n) {
        if (s->parameterSetsSize + 4 + n > sizeof s->parameterSets) return false;
        memcpy(s->parameterSets + s->parameterSetsSize, start, 4);
        memcpy(s->parameterSets + s->parameterSetsSize + 4, nal, n);
        s->parameterSetsSize += 4 + n;
        return true;
    };
    if (find(d.extensions, d.extensionsSize, cc("avcC"), ext)) {
        const uint8_t *p = ext.body, *end = ext.body + ext.size;
        if (ext.size < 7) return false;
        s->nalLength = (p[4] & 3) + 1;
        unsigned count = p[5] & 0x1f;
        p += 6;
        for (int set = 0; set < 2; ++set) {
            for (unsigned i = 0; i < count; ++i) {
                if (end - p < 2) return false;
                const uint32_t n = be16(p);
                if (uint32_t(end - p) < 2 + n || !add(p + 2, n)) return false;
                p += 2 + n;
            }
            if (set == 0) { if (p >= end) return false; count = *p++; }
        }
        return true;
    }
    if (find(d.extensions, d.extensionsSize, cc("hvcC"), ext)) {
        const uint8_t *p = ext.body, *end = ext.body + ext.size;
        if (ext.size < 23) return false;
        s->nalLength = (p[21] & 3) + 1;
        s->tenBit = (p[17] & 7) != 0;
        s->temporalLayers = (p[21] >> 3) & 7;
        const unsigned arrays = p[22];
        p += 23;
        for (unsigned a = 0; a < arrays; ++a) {
            if (end - p < 3) return false;
            const unsigned count = be16(p + 1);
            p += 3;
            for (unsigned i = 0; i < count; ++i) {
                if (end - p < 2) return false;
                const uint32_t n = be16(p);
                if (uint32_t(end - p) < 2 + n || !add(p + 2, n)) return false;
                p += 2 + n;
            }
        }
        return true;
    }
    return false;
}

// --- Messages to the guest -----------------------------------------------------------------------------------------------
bool sendToGuest(Client *c, const Writer &w, const uint8_t *buffer) {
    if (!w.ok()) { failed(20, int32_t(w.size())); return false; }
    vt_host_replies = vt_host_replies + 1;
    return c->send(c->transport, buffer, w.size());
}

void replyStatus(Client *c, const Header &h, int32_t error) {
    uint8_t buffer[128];
    Writer w(buffer, sizeof buffer);
    w.header(h.type, FlagReply, h.uuid, h.request);
    w.s32(cc(",err"), error);
    sendToGuest(c, w, buffer);
}

// A request of the host's (Dbuf, Demt) and the guest's reply to it (up to two seconds).
bool hostRequest(Session *s, Writer &w, const uint8_t *buffer, uint64_t request) {
    IOLockLock(s->lock);
    s->awaitedRequest = request;
    s->replied = false;
    s->replyHasSurface = false;
    IOLockUnlock(s->lock);
    if (!sendToGuest(s->client, w, buffer)) return false;
    IOLockLock(s->lock);
    AbsoluteTime deadline;
    clock_interval_to_deadline(2000, kMillisecondScale, &deadline);
    while (!s->replied && !s->stopping && !s->evict)
        if (IOLockSleepDeadline(s->lock, &s->replied, deadline, THREAD_UNINT) == THREAD_TIMED_OUT) break;
    const bool replied = s->replied;
    s->awaitedRequest = 0;
    IOLockUnlock(s->lock);
    if (!replied) failed(30, int32_t(request));
    return replied;
}

// --- P3: the guest's pool surfaces as CAPTURE buffers --------------------------------------------------------------------
void pixelBufferAttributes(Session *s);

// vt-direct=0: never (the pixel buffers as before P3 too, copied into); =2: P3's pixel buffers, copied into.
int directMode() {
    static int mode = -1;
    if (mode < 0) {
        int value = 1;
        PE_parse_boot_argn("vt-direct", &value, sizeof value);
        mode = value;
    }
    return mode;
}
bool directAllowed() { return directMode() == 1; }
bool directLayout() { return directMode() != 0; }

// Whether a pool surface lies as the device's CAPTURE layout does: one plane of `length` bytes from the luma's first byte, the
// chroma at captureStride rows times captureHeight after it, both planes captureStride a row, page aligned.
bool surfaceFits(const Session *s, const SurfaceView &v, uint64_t length) {
    const bool format = s->captureP010 ? v.format == cc("x420") || v.format == cc("xf20")
                                       : v.format == 875704438 || v.format == 875704422;   // 420v, 420f
    return format && v.planes == 2 && v.plane[0].bytesPerRow == s->captureStride && v.plane[1].bytesPerRow == s->captureStride &&
           v.plane[1].offset == v.plane[0].offset + uint64_t(s->captureStride) * s->captureHeight &&
           v.plane[0].offset + length <= v.size && !((v.base + v.plane[0].offset) & 4095) && !(length & 4095);
}

// Dfub: the guest's pending pixel buffers `uuids` (taken with Dbuf, never handed over) back to its pool.
void forgetSurfaces(Session *s, const uint8_t (*uuids)[16], unsigned count) {
    if (!count) return;
    uint8_t message[64 + 16 * MaxCaptureBuffers];
    Writer w(message, sizeof message);
    w.header(cc("Dfub"), FlagHostRequest, s->uuid, __atomic_fetch_add(&s->nextHostRequest, 1, __ATOMIC_RELAXED));
    w.uuids(cc(",uu+"), uuids[0], count);
    sendToGuest(s->client, w, message);
}

// A pool surface for CAPTURE slot `index` (its length already the device's): Dbuf, the layout checked, its pages wired and
// listed. False, the surface given back, when it does not fit or anything fails.
bool takeSurface(Session *s, unsigned index) {
    Mapped &m = s->capture[index];
    uint8_t message[128];
    const uint64_t request = __atomic_fetch_add(&s->nextHostRequest, 1, __ATOMIC_RELAXED);
    Writer w(message, sizeof message);
    w.header(cc("Dbuf"), FlagHostRequest, s->uuid, request);
    w.s64(cc(",frm"), 0);   // the guest answers from the pool whatever the frame (VTDecoderSessionCreatePixelBufferWithOptions)
    if (!hostRequest(s, w, message, request) || s->replyError || !s->replyHasSurface) { failed(80, s->replyError); return false; }
    const SurfaceView view = s->replySurface;
    uint8_t uuid[16];
    memcpy(uuid, s->replyUuid, 16);
    if (!surfaceFits(s, view, m.length)) {
        static uint32_t told;
        if (told++ < 4)
            IOLog("vt %u surf %c%c%c%c %u %llu/%llu %llu/%llu %llu %#llx !fit %ux%u %u %llu\n", s->v4l2, char(view.format >> 24), char(view.format >> 16),
                  char(view.format >> 8), char(view.format), view.planes, view.plane[0].offset, view.plane[1].offset,
                  view.plane[0].bytesPerRow, view.plane[1].bytesPerRow, view.size, view.base, s->captureWidth, s->captureHeight,
                  s->captureStride, m.length);
        forgetSurfaces(s, &uuid, 1);
        return false;
    }
    IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(view.base + view.plane[0].offset, m.length,
                                                                       kIODirectionInOut, s->client->task);
    if (!memory || memory->prepare() != kIOReturnSuccess) {
        OSSafeReleaseNULL(memory);
        failed(81, 0);
        forgetSurfaces(s, &uuid, 1);
        return false;
    }
    m.memory = memory;
    if (!scatter(m, memory)) {
        m.memory = nullptr;
        memory->complete();
        memory->release();
        forgetSurfaces(s, &uuid, 1);
        return false;
    }
    // The device writes the pages by DMA, past the guest's caches: whatever lines of them the guest holds dirty (the pool's own
    // clearing, an earlier frame's reader) go to memory now, before the device's frame would be overwritten by their eviction.
    memory->performOperation(kIOMemoryIncoherentIOFlush, 0, m.length);
    m.bytes = reinterpret_cast<uint8_t *>(uintptr_t(view.base + view.plane[0].offset));   // echoed by the device, never read here
    memcpy(m.uuid, uuid, 16);
    return true;
}

// Slot `index` without its surface (handed over, or going back): unwired; its list's memory and the device's length kept, a
// physically contiguous allocation per frame being what a fragmented guest fails first.
void dropSurface(Mapped &m) {
    if (m.memory) { m.memory->complete(); m.memory->release(); m.memory = nullptr; }
    m.bytes = nullptr;
}

// Every surface the slots still hold, back to the guest's pool (the session's end).
void giveSurfacesBack(Session *s) {
    uint8_t uuids[MaxCaptureBuffers][16];
    unsigned count = 0;
    for (unsigned i = 0; i < s->captureCount; ++i)
        if (s->capture[i].memory) memcpy(uuids[count++], s->capture[i].uuid, 16);
    forgetSurfaces(s, uuids, count);
}

// The CAPTURE slots in direct mode: each with a pool surface, queued. Slot 0 decides: a surface that does not fit (or none)
// leaves the session copying into buffers of its own.
bool setUpDirect(Session *s) {
    // The device's P010 into a 10-bit stream's x420 surfaces, its NV12 into an 8-bit stream's 420v ones.
    if (s->captureP010 != s->tenBit || !directAllowed()) return false;
    const uint32_t extend = s->captureHeight > s->height ? s->captureHeight - s->height : 0;
    if (extend != s->extendBottom) {
        IOLog("vt %u pixel buffers again: bottom %u, not %u\n", s->v4l2, extend, s->extendBottom);
        s->extendBottom = extend;
        pixelBufferAttributes(s);
    }
    for (unsigned i = 0; i < s->captureCount; ++i) {
        V4l2Buffer b = {};
        b.index = i; b.type = TypeCaptureMplane; b.memory = MemoryUserPtr; b.length = 1;
        const int r = ioctl(s, IocQuerybuf, &b, sizeof b, sizeof b);
        if (r || !b.plane.length) { failed(10, r ? r : -1); return false; }
        Mapped &m = s->capture[i];
        m.length = b.plane.length;
        // A list for any surface of this length (every page apart), kept across sessions.
        if (!m.sg && m.length / 4096 + 2 <= SgListBytes / sizeof(SgEntry)) {
            m.sg = takeSgList();
            m.sharedSg = m.sg != nullptr;
            m.sgCapacity = m.sg ? SgListBytes : 0;
            if (!m.sg) failed(83, int32_t(i));
        }
        if (!takeSurface(s, i)) {
            if (i == 0) return false;
            giveSurfacesBack(s);
            for (unsigned j = 0; j < i; ++j) dropSurface(s->capture[j]);
            return false;
        }
    }
    s->direct = true;
    IOLog("vt %u direct, %u surfaces\n", s->v4l2, s->captureCount);
    return true;
}

// --- The worker: the device's events ---------------------------------------------------------------------------------------
bool setUpCapture(Session *s) {
    V4l2Format f = {};
    f.type = TypeCaptureMplane;
    int r = ioctl(s, IocGFmt, &f, sizeof f, sizeof f);
    if (r) { failed(50, r); return false; }
    s->captureWidth = f.width;
    s->captureHeight = f.height;
    s->captureStride = f.planes[0].bytesperline ? f.planes[0].bytesperline : f.width;
    s->captureP010 = f.pixelformat == cc("010P");   // V4L2_PIX_FMT_P010
    uint32_t control[2] = {CidMinBuffersForCapture, 0};
    unsigned count = ioctl(s, IocGCtrl, control, sizeof control, sizeof control) ? 8 : control[1] + 2;
    if (count > MaxCaptureBuffers) count = MaxCaptureBuffers;
    uint32_t request[5] = {count, TypeCaptureMplane, MemoryUserPtr, 0, 0};
    r = ioctl(s, IocReqbufs, request, sizeof request, sizeof request);
    if (r || !request[0]) { failed(51, r); return false; }
    // Only CaptureInFlight of them used: the rest never queued, never given memory.
    s->captureCount = request[0] < CaptureInFlight ? request[0] : CaptureInFlight;
    const bool direct = setUpDirect(s);
    for (unsigned i = 0; i < s->captureCount; ++i) {
        if (!direct && !mapBuffer(s, TypeCaptureMplane, i, s->capture[i])) return false;
        V4l2Buffer b = {};
        b.index = i; b.type = TypeCaptureMplane;
        r = queueBuffer(s, b, s->capture[i]);
        if (r) { failed(52, r); return false; }
        s->capture[i].queued = true;
    }
    uint32_t type = TypeCaptureMplane;
    r = ioctl(s, IocStreamon, &type, 4, 0);
    if (r) { failed(53, r); return false; }
    s->captureStreaming = true;
    IOLog("vt: capture %ux%u stride %u, %u buffers%s\n", s->captureWidth, s->captureHeight, s->captureStride, s->captureCount,
          s->captureP010 ? ", P010" : "");
    return true;
}

void copyFrame(Session *s, const Mapped &from, const SurfaceView &to) {
    // Wired and mapped for this frame only: the pool frees a surface it no longer needs, and one the kernel still wired (kept
    // from frame to frame) took the player down when it did.
    IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(to.base, to.size, kIODirectionInOut, s->client->task);
    if (!memory || memory->prepare() != kIOReturnSuccess) { OSSafeReleaseNULL(memory); failed(60, 0); return; }
    IOMemoryMap *map = memory->createMappingInTask(kernel_task, 0, kIOMapAnywhere);
    uint8_t *surface = map ? reinterpret_cast<uint8_t *>(map->getVirtualAddress()) : nullptr;
    if (!surface) failed(61, 0);
    if (surface) {
        const uint32_t width = s->captureWidth < to.plane[0].width ? s->captureWidth : uint32_t(to.plane[0].width);
        const uint32_t height = s->captureHeight < to.plane[0].height ? s->captureHeight : uint32_t(to.plane[0].height);
        const uint8_t *luma = from.bytes, *chroma = from.bytes + uint64_t(s->captureStride) * s->captureHeight;
        // A 10-bit stream's player asks for x420/xf20 pixel buffers (16 bits a sample, the value in the top 10): the 8 bits the
        // device gives widened, not copied as they are -- 8-bit samples in the low byte of each word read as black.
        const bool wide = to.format == cc("x420") || to.format == cc("xf20");
        // P010 from the device is x420's layout already, row for row; into a 420v buffer its top bytes.
        const bool p010 = s->captureP010;
        auto row = [&](uint8_t *out, const uint8_t *in, uint32_t samples, uint64_t room) {
            if (p010 && wide) { memcpy(out, in, uint64_t(samples) * 2 < room ? samples * 2 : uint32_t(room)); return; }
            if (p010) {
                const uint16_t *words = reinterpret_cast<const uint16_t *>(in);
                const uint32_t n = samples < room ? samples : uint32_t(room);
                for (uint32_t x = 0; x < n; ++x) out[x] = uint8_t(words[x] >> 8);
                return;
            }
            if (!wide) { memcpy(out, in, samples < room ? samples : uint32_t(room)); return; }
            uint16_t *words = reinterpret_cast<uint16_t *>(out);
            const uint32_t n = samples < room / 2 ? samples : uint32_t(room / 2);
            for (uint32_t x = 0; x < n; ++x) words[x] = uint16_t(in[x] << 8);
        };
        const uint32_t chromaRows = (height + 1) / 2 < to.plane[1].height ? (height + 1) / 2 : uint32_t(to.plane[1].height);
        if (p010 && wide && to.plane[0].bytesPerRow == s->captureStride && to.plane[1].bytesPerRow == s->captureStride) {
            // The same rows the same distance apart: each plane one copy.
            memcpy(surface + to.plane[0].offset, luma, uint64_t(s->captureStride) * height);
            memcpy(surface + to.plane[1].offset, chroma, uint64_t(s->captureStride) * chromaRows);
        } else {
            for (uint32_t y = 0; y < height; ++y)
                row(surface + to.plane[0].offset + y * to.plane[0].bytesPerRow, luma + uint64_t(y) * s->captureStride, width,
                    to.plane[0].bytesPerRow);
            for (uint32_t y = 0; y < chromaRows; ++y)
                row(surface + to.plane[1].offset + y * to.plane[1].bytesPerRow, chroma + uint64_t(y) * s->captureStride, width,
                    to.plane[1].bytesPerRow);
        }
    }
    OSSafeReleaseNULL(map);
    memory->complete();
    memory->release();
}

void handOverDropped(Session *s, int64_t frame, bool wait = true);
bool syncFrame(Session *s, int64_t frame);
bool sendDemt(Session *s, Writer &e, const uint8_t *message, uint64_t request, int64_t frame, bool sync, bool wait = true);

// Returns whether the slot's pool surface went with the frame (direct): the slot then needs another.
bool emitFrame(Session *s, unsigned index, int64_t frame, uint32_t bytesused) {
    Mapped &buffer = s->capture[index];
    uint8_t message[256];
    const Session::FrameTime &time = s->frameTimes[uint64_t(frame) % FrameTimes];
    const int64_t pts = time.frame == frame ? time.ptsUs : -1;
    if (bytesused && pts >= 0) {
        // Shown when ThinUs have passed since the last shown, or when time went back (a loop, a seek, a new segment).
        if (s->lastShownUs != INT64_MIN && pts >= s->lastShownUs && pts - s->lastShownUs < ThinUs) {
            handOverDropped(s, frame);
            return false;
        }
        s->lastShownUs = pts;
    }
    if (s->direct) {
        // Decoded in place: the surface goes as it is, once no line the guest's caches hold of it (prefetched while the device
        // wrote) can stand in for what the device put there.
        if (buffer.memory) buffer.memory->performOperation(kIOMemoryIncoherentIOFlush, 0, buffer.length);
        const bool sync = syncFrame(s, frame);
        const uint64_t request = __atomic_fetch_add(&s->nextHostRequest, 1, __ATOMIC_RELAXED);
        Writer e(message, sizeof message);
        e.header(cc("Demt"), FlagHostRequest, s->uuid, request);
        e.s64(cc(",frm"), frame);
        e.s32(cc(",err"), 0);
        e.u32(cc(",inf"), sync ? 0 : 1);
        e.uuid(cc(",uui"), buffer.uuid);
        if (sendDemt(s, e, message, request, frame, sync)) vt_host_frames_out = vt_host_frames_out + 1;
        ++s->statOut;
        s->lastOutAt = mach_absolute_time();
        return true;
    }
    // Dbuf: a pixel buffer for the frame -- not while the guest is not answering (DbufPauseMs after one went unanswered: each
    // more would wait its two seconds behind the last in the guest's queue), the frame handed over dropped then.
    if (mach_absolute_time() < s->dbufPauseUntil) {
        handOverDropped(s, frame);
        return false;
    }
    const uint64_t request = __atomic_fetch_add(&s->nextHostRequest, 1, __ATOMIC_RELAXED);
    Writer w(message, sizeof message);
    w.header(cc("Dbuf"), FlagHostRequest, s->uuid, request);
    w.s64(cc(",frm"), frame);
    const bool replied = hostRequest(s, w, message, request);
    if (!replied || s->replyError || !s->replyHasSurface) {
        failed(31, s->replyError);
        if (!replied) {
            uint64_t pause;
            nanoseconds_to_absolutetime(uint64_t(DbufPauseMs) * 1000000, &pause);
            s->dbufPauseUntil = mach_absolute_time() + pause;
        }
        handOverDropped(s, frame);   // VideoToolbox waits for every frame given: one never handed over, it waited for forever
        return false;
    }
    uint8_t pixelBuffer[16];
    memcpy(pixelBuffer, s->replyUuid, 16);
    const SurfaceView surface = s->replySurface;
    if (bytesused) {
        // The VPU wrote the picture past this CPU's caches: the lines an earlier frame's copy left over the buffer are dropped
        // before it is read.
        if (buffer.buffer) buffer.buffer->performOperation(kIOMemoryIncoherentIOFlush, 0, bytesused);
        copyFrame(s, buffer, surface);
    }
    // Demt: the frame, decoded into that pixel buffer.
    const bool sync = syncFrame(s, frame);
    const uint64_t emit = __atomic_fetch_add(&s->nextHostRequest, 1, __ATOMIC_RELAXED);
    Writer e(message, sizeof message);
    e.header(cc("Demt"), FlagHostRequest, s->uuid, emit);
    e.s64(cc(",frm"), frame);
    e.s32(cc(",err"), bytesused ? 0 : -12911);
    e.u32(cc(",inf"), sync ? 0 : 1);
    e.uuid(cc(",uui"), pixelBuffer);
    // An asynchronous frame's not waited for: the guest's answer to a Demt carries nothing this side uses (an answer no one
    // awaits is let go, see vt_host_message), and waiting for it was a round trip to the player's process for every
    // frame -- 40 ms a frame for the lock screen's (a background process), when it gave thousands of frames a second.
    if (sendDemt(s, e, message, emit, frame, sync)) vt_host_frames_out = vt_host_frames_out + 1;
    ++s->statOut;
    s->lastOutAt = mach_absolute_time();
    return false;
}

void forgetFrame(Session *s, int64_t frame) {
    IOLockLock(s->lock);
    for (unsigned i = 0; i < s->outstandingCount; ++i)
        if (s->outstanding[i] == frame) { s->outstanding[i] = s->outstanding[--s->outstandingCount]; break; }
    IOLockUnlock(s->lock);
}

// Whether VideoToolbox still waits for `frame`: a frame handed over already (failed at a dfin's deadline, dropped when the
// session fell behind or let its device session go) may still come out of the device, and must not be handed over twice.
bool isOutstanding(Session *s, int64_t frame) {
    IOLockLock(s->lock);
    bool found = false;
    for (unsigned i = 0; i < s->outstandingCount && !found; ++i) found = s->outstanding[i] == frame;
    IOLockUnlock(s->lock);
    return found;
}

// --- Synchronous dfrm --------------------------------------------------------------------------------------------------------
// Apple's host answers a dfrm without ,dfl's asynchronous bit only once the frame is handed over: Dbuf, its answer, Demt, its
// answer, then the dfrm's reply (,err 0, ,inf 0). The guest's VTDecompressionSessionDecodeFrame waits for that reply before it
// gives the next frame: the player's pace. Answered at once instead, the lock screen's player gave 4000-5000 frames a second,
// and every session-lifecycle problem of 11-03/04 followed from it.

// Frames out as an asynchronous player sees them (the lock held): not handed over, and handed over in a Demt not yet answered.
unsigned asyncOut(Session *s) {
    uint64_t limit;
    nanoseconds_to_absolutetime(uint64_t(AsyncDemtMs) * 1000000, &limit);
    const uint64_t now = mach_absolute_time();
    unsigned n = s->outstandingCount;
    for (unsigned i = 0; i < s->sentDemtCount;)
        if (now - s->sentDemts[i].at >= limit) s->sentDemts[i] = s->sentDemts[--s->sentDemtCount];
        else { ++n; ++i; }
    return n;
}

// A dfrm's reply: ,inf 0 synchronous, 1 asynchronous, as Apple's.
void answerRequest(Session *s, uint64_t request, int32_t error, bool async) {
    uint8_t buffer[128];
    Writer w(buffer, sizeof buffer);
    w.header(cc("dfrm"), FlagReply, s->uuid, request);
    w.s32(cc(",err"), error);
    w.u32(cc(",inf"), async ? 1 : 0);
    sendToGuest(s->client, w, buffer);
}

void releaseAsync(Session *s, int32_t error = 0, bool all = false);

// Whether `frame` is a synchronous dfrm's, still unanswered.
bool syncFrame(Session *s, int64_t frame) {
    IOLockLock(s->lock);
    bool found = false;
    for (unsigned i = 0; i < s->syncReplyCount && !found; ++i) found = !s->syncReplies[i].async && s->syncReplies[i].frame == frame;
    IOLockUnlock(s->lock);
    return found;
}

// The dfrm's reply for `frame`, if it is a synchronous one still unanswered (,inf 0, as Apple's); with `demt`, the one whose
// Demt that is (its answer come).
void answerFrame(Session *s, int64_t frame, int32_t error, uint64_t demt = 0) {
    uint64_t request = 0;
    bool found = false;
    IOLockLock(s->lock);
    for (unsigned i = 0; i < s->syncReplyCount && !found; ++i)
        if (!s->syncReplies[i].async && (demt ? s->syncReplies[i].demt == demt : s->syncReplies[i].frame == frame)) {
            request = s->syncReplies[i].request;
            s->syncReplies[i] = s->syncReplies[--s->syncReplyCount];
            found = true;
        }
    IOLockUnlock(s->lock);
    if (found) answerRequest(s, request, error, false);
}

// The asynchronous dfrms held (the oldest first) answered while fewer than AsyncInFlight frames are out; `all`: every one.
void releaseAsync(Session *s, int32_t error, bool all) {
    for (;;) {
        uint64_t request = 0;
        bool found = false;
        IOLockLock(s->lock);
        if (all || asyncOut(s) <= AsyncInFlight) {
            unsigned oldest = s->syncReplyCount;
            for (unsigned i = 0; i < s->syncReplyCount; ++i)
                if (s->syncReplies[i].async && (oldest == s->syncReplyCount || s->syncReplies[i].request < s->syncReplies[oldest].request))
                    oldest = i;
            if (oldest < s->syncReplyCount) {
                request = s->syncReplies[oldest].request;
                s->syncReplies[oldest] = s->syncReplies[--s->syncReplyCount];
                found = true;
            }
        }
        IOLockUnlock(s->lock);
        if (!found) return;
        answerRequest(s, request, error, true);
    }
}

// Every synchronous dfrm still unanswered, answered (the session's device session ends: nothing more will come of them).
void answerAllFrames(Session *s, int32_t error) {
    releaseAsync(s, error, true);
    for (;;) {
        IOLockLock(s->lock);
        const bool any = s->syncReplyCount;
        const int64_t frame = any ? s->syncReplies[0].frame : 0;
        IOLockUnlock(s->lock);
        if (!any) return;
        answerFrame(s, frame, error);
    }
}

// An asynchronous frame's Demt answered (or never sent): out no more.
void forgetDemt(Session *s, uint64_t request) {
    IOLockLock(s->lock);
    for (unsigned i = 0; i < s->sentDemtCount; ++i)
        if (s->sentDemts[i].request == request) { s->sentDemts[i] = s->sentDemts[--s->sentDemtCount]; break; }
    IOLockUnlock(s->lock);
}

// A Demt: for a synchronous frame waited for (as Apple's host does: the dfrm's reply after the Demt's), then the dfrm answered;
// otherwise sent and not waited for (see emitFrame). Whether it went. `wait` false on the thread that takes the guest's
// messages (the dfrm's own): the Demt's answer comes on it, waiting there only ever times out -- sent, then the dfrm answered.
bool sendDemt(Session *s, Writer &e, const uint8_t *message, uint64_t request, int64_t frame, bool sync, bool wait) {
    if (!sync) {
        IOLockLock(s->lock);
        if (s->decodeOrder && s->sentDemtCount < 32) s->sentDemts[s->sentDemtCount++] = {request, mach_absolute_time()};
        IOLockUnlock(s->lock);
        const bool sent = sendToGuest(s->client, e, message);
        if (!sent) forgetDemt(s, request);
        releaseAsync(s);   // one frame fewer out (or none, till its Demt is answered)
        return sent;
    }
    // Answered when the Demt's answer comes (vt_host_message) if not here: a wait cut short (evict, stopping) too.
    IOLockLock(s->lock);
    for (unsigned i = 0; i < s->syncReplyCount; ++i)
        if (s->syncReplies[i].frame == frame) s->syncReplies[i].demt = request;
    IOLockUnlock(s->lock);
    const bool sent = wait ? hostRequest(s, e, message, request) : sendToGuest(s->client, e, message);
    if (sent == wait) answerFrame(s, frame, 0);   // waited and answered; or never went (sent without waiting: its answer will)
    return sent;
}

// A frame handed over as dropped, no pixels (a frame the session skipped: a temporal layer above the kept ones, a frame of a
// stretch the session fell behind on).
void handOverDropped(Session *s, int64_t frame, bool wait) {
    ++s->statDropped;
    const bool sync = syncFrame(s, frame);
    uint8_t message[160];
    const uint64_t request = __atomic_fetch_add(&s->nextHostRequest, 1, __ATOMIC_RELAXED);
    Writer e(message, sizeof message);
    e.header(cc("Demt"), FlagHostRequest, s->uuid, request);
    e.s64(cc(",frm"), frame);
    e.s32(cc(",err"), 0);
    e.u32(cc(",inf"), (sync ? 0 : 1) | 2);   // kVTDecodeInfo_Asynchronous (an asynchronous frame's) | kVTDecodeInfo_FrameDropped
    if (sendDemt(s, e, message, request, frame, sync, wait)) vt_host_frames_dropped = vt_host_frames_dropped + 1;
}

// The frames that never came out (a codec that dropped them, a decoder too small for the stream, a device session let go): each
// handed over as dropped -- not failed: a failed frame kills the lock screen's player (11-04, an evicted session's). Not waited for, as emitFrame's Demt is not: each was a round trip to the player's process (40 ms for the lock screen's), and
// the guest's answers cannot come while its thread is in a send to this side (selector 3) waiting for the worker.
void dropOutstanding(Session *s) {
    for (;;) {
        IOLockLock(s->lock);
        if (!s->outstandingCount) { IOLockUnlock(s->lock); return; }
        const int64_t frame = s->outstanding[--s->outstandingCount];
        IOLockUnlock(s->lock);
        const bool sync = syncFrame(s, frame);
        uint8_t message[160];
        const uint64_t request = __atomic_fetch_add(&s->nextHostRequest, 1, __ATOMIC_RELAXED);
        Writer e(message, sizeof message);
        e.header(cc("Demt"), FlagHostRequest, s->uuid, request);
        e.s64(cc(",frm"), frame);
        e.s32(cc(",err"), 0);
        e.u32(cc(",inf"), (sync ? 0 : 1) | 2);   // kVTDecodeInfo_Asynchronous | kVTDecodeInfo_FrameDropped
        if (sendDemt(s, e, message, request, frame, sync)) vt_host_frames_dropped = vt_host_frames_dropped + 1;
        ++s->statDropped;
    }
}

// The frames waiting for an OUTPUT buffer, freed (the lock held); their IDs stay outstanding, for dropOutstanding or the caller.
unsigned discardInputs(Session *s, int64_t *frames, unsigned room) {
    unsigned n = 0;
    while (s->inputTail != s->inputHead) {
        const Session::Input &input = s->inputs[s->inputTail++ % MaxInputs];
        if (frames && n < room) frames[n] = input.frame;
        ++n;
        IOFree(input.bytes, input.capacity);
        if (s->pendingFrames) --s->pendingFrames;
    }
    return n;
}

void requeueCapture(Session *s, unsigned index) {
    V4l2Buffer b = {};
    b.index = index; b.type = TypeCaptureMplane;
    const int r = queueBuffer(s, b, s->capture[index]);
    if (r) failed(54, r);
    else s->capture[index].queued = true;
}

// The highest temporal layer given to the device: the least that still makes about 30 frames a second, layers taken as halving
// the rate (the aerials' do), once 16 steps between inputs' times have shown the frame period; every layer till then.
bool hevcSession(const Session *s);
unsigned keptLayer(const Session *s) {
    if (!hevcSession(s) || s->temporalLayers < 2) return 7;
    unsigned kept = s->temporalLayers - 1;
    if (s->steps >= 16 && s->minStepUs > 0 && s->minStepUs != INT64_MAX) {
        const int64_t rate = 1000000 / s->minStepUs;
        unsigned drop = 0;
        while (drop + 1 < s->temporalLayers && (rate >> (drop + 1)) >= 30) ++drop;
        kept = s->temporalLayers - 1 - drop;
    }
    return kept > s->shed ? kept - s->shed : 0;
}

// The oldest kept frame into a free OUTPUT buffer, if both exist (the lock held, and released while the buffer is filled).
bool feedInput(Session *s) {
    if (s->inputHead == s->inputTail || s->draining) return false;   // after a drain's LAST (endDrain)
    // A frame to skip needs no OUTPUT buffer: one waiting for one would hold the 7 in 8 behind it that never go to the device.
    if (s->inputs[s->inputTail % MaxInputs].tid > keptLayer(s)) {
        const Session::Input input = s->inputs[s->inputTail++ % MaxInputs];
        // Z0: a frame of a temporal layer no kept frame refers to, handed over as dropped without the device seeing it -- if
        // VideoToolbox waits for it (not one replayed to start the session again, handed over long ago).
        bool given = false;
        for (unsigned i = 0; i < s->outstandingCount; ++i)
            if (s->outstanding[i] == input.frame) { s->outstanding[i] = s->outstanding[--s->outstandingCount]; given = true; break; }
        if (!given) {
            IOLockUnlock(s->lock);
            IOFree(input.bytes, input.capacity);
            IOLockLock(s->lock);
            return true;
        }
        if (s->pendingFrames) --s->pendingFrames;
        const bool first = !s->skipLogged;
        s->skipLogged = true;
        IOLockUnlock(s->lock);
        IOFree(input.bytes, input.capacity);
        if (first)
            IOLog("vt %u keeps layers 0-%u of %u (%lld us)\n", s->v4l2, keptLayer(s),
                  s->temporalLayers, s->minStepUs);
        uint8_t message[160];
        const uint64_t request = __atomic_fetch_add(&s->nextHostRequest, 1, __ATOMIC_RELAXED);
        Writer e(message, sizeof message);
        e.header(cc("Demt"), FlagHostRequest, s->uuid, request);
        e.s64(cc(",frm"), input.frame);
        e.s32(cc(",err"), 0);
        e.u32(cc(",inf"), 1 | 2);   // kVTDecodeInfo_Asynchronous | kVTDecodeInfo_FrameDropped
        if (sendToGuest(s->client, e, message)) {
            vt_host_frames_dropped = vt_host_frames_dropped + 1;
            vt_host_frames_skipped = vt_host_frames_skipped + 1;
        }
        IOLockLock(s->lock);
        return true;
    }
    unsigned index = OutputBuffers;
    for (unsigned i = 0; i < OutputBuffers; ++i) if (!s->output[i].queued) { index = i; break; }
    if (index == OutputBuffers) return false;
    const Session::Input input = s->inputs[s->inputTail++ % MaxInputs];
    s->frameTimes[uint64_t(input.frame) % FrameTimes] = {input.frame, input.ptsUs};
    s->output[index].queued = true;
    IOLockUnlock(s->lock);
    Mapped &out = s->output[index];
    const uint32_t size = input.size < out.length ? input.size : uint32_t(out.length);
    memcpy(out.bytes, input.bytes, size);
    IOFree(input.bytes, input.capacity);
    V4l2Buffer b = {};
    b.index = index; b.type = TypeOutputMplane;
    b.sec = input.frame;
    b.plane.bytesused = size;
    const int r = queueBuffer(s, b, out);
    IOLockLock(s->lock);
    if (r) {
        failed(8, r);
        out.queued = false;
        if (s->pendingFrames) --s->pendingFrames;
    }
    return true;
}

// dfin's answer, once nothing given is still on its way (or at its deadline). The lock held.
int decoderCommand(Session *s, uint32_t command) {
    uint32_t cmd[18] = {command};
    return ioctl(s, IocDecoderCmd, cmd, sizeof cmd, sizeof cmd);
}

void answerFinish(Session *s) {
    if (!s->finishWanted) return;
    // Every given frame fed and some still inside the codec: drain it (once per dfin).
    if (s->pendingFrames && !s->draining && s->inputHead == s->inputTail && mach_absolute_time() < s->finishDeadline) {
        s->draining = true;
        IOLockUnlock(s->lock);
        const int r = decoderCommand(s, DecCmdStop);
        IOLockLock(s->lock);
        if (r) { failed(55, r); s->draining = false; }
        return;
    }
    if (s->pendingFrames && mach_absolute_time() < s->finishDeadline) return;
    // The deadline is for a stuck device or guest, not a slow one: frames still coming out, another FinishMs. The player reads a
    // clip in at once and its dfin waits for the whole clip; the rest of it dropped at two seconds was the clip on screen cut.
    if (s->pendingFrames && s->pendingFrames < s->pendingAtDeadline) {
        s->pendingAtDeadline = s->pendingFrames;
        clock_interval_to_deadline(FinishMs, kMillisecondScale, &s->finishDeadline);
        return;
    }
    if (s->outstandingCount) {
        // Handed over failed, these frames are VideoToolbox's no more: those still waiting go too (isOutstanding keeps the
        // device's late ones from being handed over again).
        discardInputs(s, nullptr, 0);
        IOLockUnlock(s->lock);
        dropOutstanding(s);
        IOLockLock(s->lock);
        s->pendingFrames = 0;
    }
    Header h;
    h.type = cc("dfin");
    memcpy(h.uuid, s->uuid, 16);
    h.request = s->finishRequest;
    s->finishWanted = false;
    s->finished = true;
    IOLockUnlock(s->lock);
    replyStatus(s->client, h, 0);
    IOLockLock(s->lock);
}

// A drain's end (its LAST buffer, or EOS): the decoder takes frames again.
void endDrain(Session *s) {
    IOLockLock(s->lock);
    const bool draining = s->draining;
    s->draining = false;
    // What the drain did not bring out never will: handed over dropped now (not left for VideoToolbox to wait on); the frames
    // given since, still waiting to be fed, are what is pending.
    int64_t gone[64];
    unsigned goneCount = 0;
    if (draining) {
        for (unsigned i = 0; i < s->outstandingCount;) {
            bool waiting = false;
            for (unsigned at = s->inputTail; at != s->inputHead && !waiting; ++at)
                waiting = s->inputs[at % MaxInputs].frame == s->outstanding[i];
            if (waiting || goneCount == 64) { ++i; continue; }
            gone[goneCount++] = s->outstanding[i];
            s->outstanding[i] = s->outstanding[--s->outstandingCount];
        }
        s->pendingFrames = s->inputHead - s->inputTail;
    }
    IOLockUnlock(s->lock);
    for (unsigned i = 0; i < goneCount; ++i) handOverDropped(s, gone[i]);
    if (draining) {
        const int r = decoderCommand(s, DecCmdStart);
        if (r) failed(56, r);
    }
}

// Whether the session has nothing to do: no frame waiting to be fed, no dfin, no event. Frames may still be inside the codec,
// which keeps the last few of a stream until it is drained.
bool sessionQuiet(const Session *s) {
    return s->inputHead == s->inputTail && !s->finishWanted && !s->draining && s->eventHead == s->eventTail;
}

void closeDevice(Session *s);
void forgetStartMessage(Session *s);

// The life log's line for a session's end (dinv, -dec, let go): what was still VideoToolbox's then.
void logEnd(Session *s, const char *why) {
    const uint64_t now = mach_absolute_time();
    uint64_t ms;
    nanoseconds_to_absolutetime(1000000, &ms);
    IOLockLock(s->lock);
    const unsigned outstanding = s->outstandingCount, pending = s->pendingFrames, waiting = s->inputHead - s->inputTail;
    const uint64_t sinceIn = s->lastInputAt ? (now - s->lastInputAt) / ms : 0, sinceOut = s->lastOutAt ? (now - s->lastOutAt) / ms : 0;
    IOLockUnlock(s->lock);
    IOLog("vt %u %s: in %u out %u drop %u, %u left (%u pend, %u wait), in %llu out %llu ms ago\n", s->v4l2, why, s->statIn, s->statOut, s->statDropped, outstanding, pending, waiting, sinceIn, sinceOut);
}

// Whether an evicted session lets its device session go now (the lock held): not while a drain started for it brings its last
// frames out, for EvictDrainMs at most. In decode order (decodeOrder) nothing is kept back: at once.
bool evictNow(Session *s) {
    if (!s->evict) return false;
    if (s->decodeOrder) return true;
    if (!s->pendingFrames || s->inputHead != s->inputTail || !s->captureStreaming) return true;
    if (!s->evictDrainUntil) {
        uint64_t wait;
        nanoseconds_to_absolutetime(uint64_t(EvictDrainMs) * 1000000, &wait);
        s->evictDrainUntil = mach_absolute_time() + wait;
        if (!s->draining) {
            s->draining = true;
            IOLockUnlock(s->lock);
            const int r = decoderCommand(s, DecCmdStop);
            IOLockLock(s->lock);
            if (r) { failed(57, r); s->draining = false; return true; }
        }
    }
    return mach_absolute_time() >= s->evictDrainUntil;
}

void workerMain(void *argument, wait_result_t) {
    Session *s = static_cast<Session *>(argument);
    bool idle = false;
    for (;;) {
        IOLockLock(s->lock);
        for (;;) {
            if (s->stopping || evictNow(s) || s->eventHead != s->eventTail) break;
            if (feedInput(s)) continue;
            answerFinish(s);
            if (s->stopping || evictNow(s) || s->eventHead != s->eventTail) break;
            // The lock screen's player reads each clip to its end with a new session and lets the old one go only much later
            // (its -dec): each held its device session, some 300 MB of the host's memory and one of the reference decoder's eight, until
            // the host ran out (and the reference decoder refused the ninth). A session with every frame handed over after a dfin, or given
            // none for IdleCloseMs, let its device session go. Only the first now: the player opens the next clip's session ahead,
            // gives it its first frame (the clip's one sync sample) and nothing more for seconds -- closed then, it had nothing to
            // start again from (11-04). Sessions left open are makeRoom's.
            if (sessionQuiet(s) && s->finished && !s->pendingFrames) {
                idle = true;
                break;
            }
            AbsoluteTime deadline;
            clock_interval_to_deadline(IdleCloseMs, kMillisecondScale, &deadline);
            if (!s->decodeOrder && sessionQuiet(s) && s->pendingFrames && s->captureStreaming && !s->idleDrained) {
                uint64_t idleFor;
                nanoseconds_to_absolutetime(uint64_t(IdleDrainMs) * 1000000, &idleFor);
                if (mach_absolute_time() - s->lastInputAt >= idleFor) {
                    s->idleDrained = s->draining = true;
                    const unsigned pending = s->pendingFrames, outstanding = s->outstandingCount;
                    IOLockUnlock(s->lock);
                    IOLog("vt %u idle drain (%u pending, %u outstanding)\n", s->v4l2, pending, outstanding);
                    const int r = decoderCommand(s, DecCmdStop);
                    IOLockLock(s->lock);
                    if (r) { failed(58, r); s->draining = false; }
                    continue;
                }
                if (s->lastInputAt + idleFor < deadline) deadline = s->lastInputAt + idleFor;
            }
            if (s->finishWanted && s->finishDeadline < deadline) deadline = s->finishDeadline;
            if (s->evictDrainUntil && s->evictDrainUntil < deadline) deadline = s->evictDrainUntil;
            IOLockSleepDeadline(s->lock, &s->events, deadline, THREAD_UNINT);
        }
        if (idle || (s->evict && !s->stopping && evictNow(s))) {
            // Let go: what still waits and what is inside the codec handed over (dropped), the device session closed. The next
            // frame that is a sync sample starts a new one (queueFrame); those before it are dropped.
            const bool evicted = !idle;
            discardInputs(s, nullptr, 0);
            if (s->outstandingCount) {
                IOLockUnlock(s->lock);
                dropOutstanding(s);
                IOLockLock(s->lock);
            }
            s->pendingFrames = 0;
            s->idleClosed = true;
            s->resync = true;
            s->evict = false;
            s->evictDrainUntil = 0;
            // A dfin still waiting: every frame is handed over now.
            const bool finish = s->finishWanted;
            Header h;
            h.type = cc("dfin");
            memcpy(h.uuid, s->uuid, 16);
            h.request = s->finishRequest;
            s->finishWanted = s->draining = false;
            IOLockUnlock(s->lock);
            if (finish) replyStatus(s->client, h, 0);
            if (evicted) logEnd(s, "let go");
            closeDevice(s);
            IOLockLock(s->lock);
            break;
        }
        if (s->stopping) break;
        const Event e = s->events[s->eventTail++ % 64];
        IOLockUnlock(s->lock);
        if (e.kind == MediaEvtEvent && e.type == EventSourceChange) {
            if (!s->captureStreaming) setUpCapture(s);
        } else if (e.kind == MediaEvtDqbuf && e.type == TypeOutputMplane && e.index < OutputBuffers) {
            IOLockLock(s->lock);
            s->output[e.index].queued = false;
            IOLockUnlock(s->lock);
        } else if (e.kind == MediaEvtDqbuf && e.type == TypeCaptureMplane && e.index < s->captureCount) {
            s->capture[e.index].queued = false;
            bool handedOver = false;
            if (e.bytesused && !(e.flags & BufFlagError) && isOutstanding(s, e.sec)) {
                handedOver = emitFrame(s, e.index, e.sec, e.bytesused);
                forgetFrame(s, e.sec);
                IOLockLock(s->lock);
                if (s->pendingFrames) --s->pendingFrames;
                IOLockUnlock(s->lock);
            } else if ((e.flags & BufFlagError) && isOutstanding(s, e.sec)) {
                // The device could not decode it (after a drain: the references it let go): dropped, now.
                forgetFrame(s, e.sec);
                handOverDropped(s, e.sec);
                IOLockLock(s->lock);
                if (s->pendingFrames) --s->pendingFrames;
                IOLockUnlock(s->lock);
            }
            if (e.flags & BufFlagLast) endDrain(s);
            if (handedOver) dropSurface(s->capture[e.index]);
            if (!s->direct) {
                requeueCapture(s, e.index);
            } else {
                // Every slot without a surface (this one, or one an earlier Dbuf failed for) takes one and goes back.
                for (unsigned i = 0; i < s->captureCount; ++i) {
                    if (s->capture[i].queued) continue;
                    if (!s->capture[i].memory && !takeSurface(s, i)) { failed(82, int32_t(i)); continue; }
                    requeueCapture(s, i);
                }
            }
        } else if (e.kind == MediaEvtEvent && e.type == EventEos) {
            endDrain(s);
        } else if (e.kind == MediaEvtError) {
            // The device session is gone (the firmware failed it): let it go as when evicted, a new one at the next sync.
            failed(70, 0);
            IOLockLock(s->lock);
            s->evict = true;
            IOLockUnlock(s->lock);
        }
    }
    s->workerRunning = false;
    IOLockUnlock(s->lock);
    IOLockWakeup(s->lock, &s->workerRunning, false);
    thread_terminate(current_thread());
}

// --- The encoder ----------------------------------------------------------------------------------------------------------
// The exchange VideoToolbox's ParavirtualizedVideoEncoder has with a host (recorded by replaying a guest's messages through
// macOS's host session): +enc, eset/emul, esta (,vdi the size; the host sends Epba, the pixel buffers it
// takes), esup, epfr, efrm (,frm ,pts ,dur ,uui and the frame's IOSurface; answered at once, asynchronous), then for each coded
// frame Efds when the stream's parameter sets are new (,cod ,wdt ,hgt ,ext with the avcC/hvcC: the guest makes the description
// and answers with it) and Eemt (,frm ,err ,efl ,uui and ,sbf the sample buffer: atoms sbuf{opts stia sdat nsmp ssiz fdsc{mdia
// vdim codc extn{dict}} sary{aray{dict}}}, little endian inside, keys as strings or indices; a message to the guest is at most
// 16 KiB, so a larger one goes in pieces flagged 0x10, the last with the request), ecfr (answered once every frame given is
// handed over), einv, -enc. Over iris media's stateful encoder: OUTPUT tightly packed NV12, CAPTURE the bitstream (Annex B,
// the parameter sets alone in a buffer of their own or before a key frame), V4L2_ENC_CMD_STOP to drain.
void teardown(Session *s);

enum : uint32_t { IocEncoderCmd = 77, EncCmdStart = 0, EncCmdStop = 1, BufFlagKeyframe = 0x8 };
constexpr uint32_t MaxGuestMessage = 16384;

// Atoms written as FigAtomWriter does: 32-bit big-endian size, type, body.
class AtomOut {
public:
    AtomOut(uint8_t *buffer, uint32_t capacity) : out(buffer), cap(capacity) {}
    uint32_t begin(uint32_t type) { const uint32_t at = len; uint8_t h[8] = {}; put32(h + 4, type); bytes(h, 8); return at; }
    void end(uint32_t at) { if (good) put32(out + at, len - at); }
    void leaf(uint32_t type, const void *data, uint32_t n) { const uint32_t at = begin(type); bytes(data, n); end(at); }
    void bytes(const void *data, uint32_t n) {
        if (!good || len + n > cap) { good = false; return; }
        memcpy(out + len, data, n);
        len += n;
    }
    bool ok() const { return good; }
    uint32_t size() const { return len; }
private:
    uint8_t *out;
    uint32_t cap, len = 0;
    bool good = true;
};

// A property list of one object (a bool, an ASCII string or data), as the atom leaves carry them.
uint32_t tinyPlist(uint8_t *out, uint32_t capacity, char kind, const void *data, uint32_t n) {
    uint32_t at = 0;
    auto put = [&](const void *src, uint32_t count) { if (at + count <= capacity) memcpy(out + at, src, count); at += count; };
    auto byte = [&](uint8_t b) { put(&b, 1); };
    put("bplist00", 8);
    if (kind == 'b') byte(n ? 0x09 : 0x08);
    else {
        const uint8_t high = kind == 's' ? 0x50 : 0x40;
        if (n < 15) byte(uint8_t(high | n));
        else if (n < 256) { byte(uint8_t(high | 0x0f)); byte(0x10); byte(uint8_t(n)); }
        else { byte(uint8_t(high | 0x0f)); byte(0x11); byte(uint8_t(n >> 8)); byte(uint8_t(n)); }
        put(data, n);
    }
    const uint32_t table = at;
    byte(8);   // the one object's offset
    uint8_t trailer[32] = {};
    trailer[6] = 1;
    trailer[7] = 1;
    put64(trailer + 8, 1);
    put64(trailer + 24, table);
    put(trailer, 32);
    return at <= capacity ? at : 0;
}

void plistLeaf(AtomOut &a, uint32_t type, char kind, const void *data, uint32_t n) {
    uint8_t buffer[512];
    const uint32_t size = tinyPlist(buffer, sizeof buffer, kind, data, n);
    if (size) a.leaf(type, buffer, size);
    else a.leaf(type, nullptr, 0);
}
void stringKey(AtomOut &a, const char *key) { plistLeaf(a, cc("strk"), 's', key, uint32_t(strlen(key))); }

// A CMTime of a 'para' (value, timescale, flags, epoch; big endian) as CMTime's own memory (little endian).
void cmTimeLittle(const uint8_t *big, uint8_t *little) {
    const uint64_t value = be64(big), epoch = be64(big + 16);
    const uint32_t scale = be32(big + 8), flags = be32(big + 12);
    memcpy(little, &value, 8);
    memcpy(little + 8, &scale, 4);
    memcpy(little + 12, &flags, 4);
    memcpy(little + 16, &epoch, 8);
}

bool hevcSession(const Session *s) { return s->codec == cc("hvc1") || s->codec == cc("hev1"); }

// The avcC or hvcC of the session's parameter sets.
uint32_t configurationRecord(const Session *s, uint8_t *out, uint32_t capacity) {
    if (!hevcSession(s)) {
        if (s->spsSize < 4 || !s->ppsSize || 11 + s->spsSize + s->ppsSize > capacity) return 0;
        uint32_t at = 0;
        out[at++] = 1; out[at++] = s->sps[1]; out[at++] = s->sps[2]; out[at++] = s->sps[3];
        out[at++] = 0xff;   // 4-byte NAL lengths
        out[at++] = 0xe1;   // one SPS
        out[at++] = uint8_t(s->spsSize >> 8); out[at++] = uint8_t(s->spsSize);
        memcpy(out + at, s->sps, s->spsSize); at += s->spsSize;
        out[at++] = 1;
        out[at++] = uint8_t(s->ppsSize >> 8); out[at++] = uint8_t(s->ppsSize);
        memcpy(out + at, s->pps, s->ppsSize); at += s->ppsSize;
        return at;
    }
    // hvcC: the general profile, tier, level and their flags straight from the SPS's profile_tier_level (bytes 3-14 after the
    // two-byte NAL header and the byte of IDs), 4:2:0 8-bit, 4-byte NAL lengths, then the VPS, SPS and PPS arrays.
    if (s->spsSize < 15 || !s->vpsSize || !s->ppsSize || 23 + 15 + s->vpsSize + s->spsSize + s->ppsSize > capacity) return 0;
    uint32_t at = 0;
    out[at++] = 1;
    memcpy(out + at, s->sps + 3, 12); at += 12;   // profile space/tier/idc, compatibility flags, constraint flags, level
    out[at++] = 0xf0; out[at++] = 0x00;            // min_spatial_segmentation_idc 0
    out[at++] = 0xfc;                              // parallelismType 0
    out[at++] = 0xfd;                              // chroma_format_idc 1
    out[at++] = 0xf8;                              // bit_depth_luma 8
    out[at++] = 0xf8;                              // bit_depth_chroma 8
    out[at++] = 0; out[at++] = 0;                  // avgFrameRate 0
    out[at++] = 0x0f;                              // constantFrameRate 0, numTemporalLayers 1, temporalIdNested 1, length 4
    out[at++] = 3;
    const struct { uint8_t type; const uint8_t *nal; uint32_t size; } arrays[3] = {
        {32, s->vps, s->vpsSize}, {33, s->sps, s->spsSize}, {34, s->pps, s->ppsSize}};
    for (const auto &a : arrays) {
        out[at++] = uint8_t(0x80 | a.type);
        out[at++] = 0; out[at++] = 1;
        out[at++] = uint8_t(a.size >> 8); out[at++] = uint8_t(a.size);
        memcpy(out + at, a.nal, a.size); at += a.size;
    }
    return at;
}

// Efds: the guest makes the stream's format description (and answers with it, which this host does not need).
bool describeToGuest(Session *s) {
    uint8_t record[1024];
    const uint32_t recordSize = configurationRecord(s, record, sizeof record);
    if (!recordSize) { failed(80, int32_t(s->spsSize)); return false; }
    Plist *p = new Plist();
    uint8_t *message = static_cast<uint8_t *>(IOMallocZero(4096));
    bool sent = false;
    if (p && message) {
        const int atomKeys[1] = {p->string(hevcSession(s) ? "hvcC" : "avcC")};
        const int atomValues[1] = {p->data(record, recordSize)};
        const int keys[2] = {p->string("FormatName"), p->string("SampleDescriptionExtensionAtoms")};
        const int values[2] = {p->string(hevcSession(s) ? "HEVC" : "H.264"), p->dict(atomKeys, atomValues, 1)};
        const int extensions = p->dict(keys, values, 2);
        const uint64_t request = __atomic_fetch_add(&s->nextHostRequest, 1, __ATOMIC_RELAXED);
        Writer w(message, 4096);
        w.header(cc("Efds"), FlagHostRequest, s->uuid, request);
        w.s32(cc(",cod"), int32_t(s->codec));
        w.s32(cc(",wdt"), int32_t(s->width));
        w.s32(cc(",hgt"), int32_t(s->height));
        w.plist(cc(",ext"), *p, extensions);
        sent = hostRequest(s, w, message, request) && !s->replyError;
        if (!sent) failed(81, s->replyError);
    }
    delete p;
    if (message) IOFree(message, 4096);
    return sent;
}

// Eemt: one coded frame (length-prefixed NAL units), or a frame dropped (data null).
void emitEncoded(Session *s, const Session::Frame &f, const uint8_t *data, uint32_t size, bool key, int32_t error) {
    const uint32_t capacity = size + 4096;
    uint8_t *sbuf = static_cast<uint8_t *>(IOMalloc(capacity));
    uint8_t *message = static_cast<uint8_t *>(IOMalloc(MaxGuestMessage));
    if (!sbuf || !message) { if (sbuf) IOFree(sbuf, capacity); if (message) IOFree(message, MaxGuestMessage); return; }
    uint32_t sbufSize = 0;
    if (data) {
        AtomOut a(sbuf, capacity);
        const uint32_t top = a.begin(cc("sbuf"));
        uint8_t pts[24], duration[24], timing[72] = {};
        cmTimeLittle(f.pts, pts);
        cmTimeLittle(f.duration, duration);
        a.leaf(cc("opts"), pts, 24);
        memcpy(timing, duration, 24);
        memcpy(timing + 24, pts, 24);   // decode time: invalid (no reordering)
        a.leaf(cc("stia"), timing, 72);
        a.leaf(cc("sdat"), data, size);
        const uint32_t one = 1;
        a.leaf(cc("nsmp"), &one, 4);
        a.leaf(cc("ssiz"), &size, 4);
        const uint32_t fdsc = a.begin(cc("fdsc"));
        const uint32_t media = cc("vide"), codec = s->codec;
        const int32_t dimensions[2] = {int32_t(s->width), int32_t(s->height)};
        a.leaf(cc("mdia"), &media, 4);   // little endian, as CoreMedia keeps a FourCC
        a.leaf(cc("vdim"), dimensions, 8);
        a.leaf(cc("codc"), &codec, 4);
        uint8_t record[1024];
        const uint32_t recordSize = configurationRecord(s, record, sizeof record);
        const uint32_t extn = a.begin(cc("extn"));
        const uint32_t dict = a.begin(cc("dict"));
        const uint32_t entry = a.begin(cc("keyv"));
        stringKey(a, "SampleDescriptionExtensionAtoms");
        const uint32_t atoms = a.begin(cc("dict"));
        const uint32_t atomEntry = a.begin(cc("keyv"));
        stringKey(a, hevcSession(s) ? "hvcC" : "avcC");
        plistLeaf(a, cc("datv"), 'd', record, recordSize);
        a.end(atomEntry);
        a.end(atoms);
        a.end(entry);
        a.end(dict);
        a.end(extn);
        a.end(fdsc);
        const uint32_t sary = a.begin(cc("sary"));
        const uint32_t array = a.begin(cc("aray"));
        // A sync sample has no NotSync at all (WebRTC and others test for the key, not its value).
        const uint32_t attachments = a.begin(cc("dict"));
        if (!key) {
            const uint32_t notSync = a.begin(cc("keyv"));
            stringKey(a, "NotSync");
            plistLeaf(a, cc("bulv"), 'b', nullptr, 1);
            a.end(notSync);
        }
        const uint32_t depends = a.begin(cc("keyv"));
        stringKey(a, "DependsOnOthers");
        plistLeaf(a, cc("bulv"), 'b', nullptr, key ? 0 : 1);
        a.end(depends);
        a.end(attachments);
        a.end(array);
        a.end(sary);
        a.end(top);
        if (!a.ok()) { failed(82, int32_t(size)); error = -12902; }
        else sbufSize = a.size();
    }
    // The head parameters, then the sample buffer in pieces of what fits a message.
    const uint64_t request = __atomic_fetch_add(&s->nextHostRequest, 1, __ATOMIC_RELAXED);
    uint32_t sent = 0;
    bool first = true;
    for (;;) {
        Writer w(message, MaxGuestMessage);
        const uint32_t room = first ? MaxGuestMessage - 48 - 24 - 20 - 20 - 32 - 16 : MaxGuestMessage - 48 - 16;
        const uint32_t chunk = sbufSize - sent < room ? sbufSize - sent : room;
        const bool last = sent + chunk == sbufSize;
        w.header(cc("Eemt"), last ? FlagHostRequest : 0x10, s->uuid, last ? request : 0);
        if (first) {
            w.s64(cc(",frm"), f.frame);
            w.s32(cc(",err"), error);
            w.u32(cc(",efl"), error ? 3 : 1);   // kVTEncodeInfo_Asynchronous, | FrameDropped
            w.uuid(cc(",uui"), f.uuid);
        }
        if (chunk) {
            uint8_t prefix[16];
            put32(prefix, 16 + chunk);
            put32(prefix + 4, cc("para"));
            put32(prefix + 8, cc(",sbf"));
            put32(prefix + 12, cc("=sbf"));
            if (w.size() + 16 + chunk <= MaxGuestMessage) {
                memcpy(message + w.size(), prefix, 16);
                memcpy(message + w.size() + 16, sbuf + sent, chunk);
            }
        }
        const uint32_t length = w.size() + (chunk ? 16 + chunk : 0);
        sent += chunk;
        first = false;
        if (last) {
            // The final piece is the host's request: sent and waited for as one.
            IOLockLock(s->lock);
            s->awaitedRequest = request;
            s->replied = false;
            IOLockUnlock(s->lock);
            vt_host_replies = vt_host_replies + 1;
            s->client->send(s->client->transport, message, length);
            IOLockLock(s->lock);
            AbsoluteTime deadline;
            clock_interval_to_deadline(2000, kMillisecondScale, &deadline);
            while (!s->replied && !s->stopping)
                if (IOLockSleepDeadline(s->lock, &s->replied, deadline, THREAD_UNINT) == THREAD_TIMED_OUT) break;
            const bool replied = s->replied;
            s->awaitedRequest = 0;
            IOLockUnlock(s->lock);
            if (replied && !error) vt_host_frames_out = vt_host_frames_out + 1;
            if (!replied) failed(83, int32_t(request));
            break;
        }
        vt_host_replies = vt_host_replies + 1;
        s->client->send(s->client->transport, message, length);
    }
    IOFree(sbuf, capacity);
    IOFree(message, MaxGuestMessage);
}

// The oldest frame not yet fed, into a free OUTPUT buffer as tightly packed NV12 (the lock held, released for the copy).
bool feedFrame(Session *s) {
    unsigned index = OutputBuffers;
    for (unsigned i = 0; i < OutputBuffers; ++i) if (!s->output[i].queued) { index = i; break; }
    if (index == OutputBuffers) return false;
    Session::Frame *f = nullptr;
    for (unsigned i = s->frameTail; i != s->frameHead; ++i)
        if (!s->frames[i % 64].fed) { f = &s->frames[i % 64]; break; }
    if (!f) return false;
    f->fed = true;
    s->output[index].queued = true;
    const SurfaceView view = f->view;
    const int64_t frame = f->frame;
    IOLockUnlock(s->lock);
    Mapped &out = s->output[index];
    const uint32_t w = s->width, h = s->height, need = w * h * 3 / 2;
    IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(view.base, view.size, kIODirectionInOut, s->client->task);
    IOMemoryMap *map = memory && memory->prepare() == kIOReturnSuccess ? memory->createMappingInTask(kernel_task, 0, kIOMapAnywhere) : nullptr;
    const uint8_t *surface = map ? reinterpret_cast<const uint8_t *>(map->getVirtualAddress()) : nullptr;
    if (surface && need <= out.length) {
        for (uint32_t y = 0; y < h && y < view.plane[0].height; ++y)
            memcpy(out.bytes + uint64_t(y) * w, surface + view.plane[0].offset + y * view.plane[0].bytesPerRow, w);
        for (uint32_t y = 0; y < h / 2 && y < view.plane[1].height; ++y)
            memcpy(out.bytes + uint64_t(w) * h + uint64_t(y) * w, surface + view.plane[1].offset + y * view.plane[1].bytesPerRow, w);
    } else {
        failed(84, int32_t(need));
    }
    OSSafeReleaseNULL(map);
    if (memory) { memory->complete(); memory->release(); }
    V4l2Buffer b = {};
    b.index = index; b.type = TypeOutputMplane;
    b.sec = frame;
    b.plane.bytesused = need;
    const int r = queueBuffer(s, b, out);
    IOLockLock(s->lock);
    if (r) { failed(85, r); out.queued = false; }
    return true;
}

Session::Frame *findFrame(Session *s, int64_t frame) {
    for (unsigned i = s->frameTail; i != s->frameHead; ++i)
        if (s->frames[i % 64].frame == frame) return &s->frames[i % 64];
    return nullptr;
}

// A frame handed over: out of the ring (whose tail moves past every frame handed over).
void retireFrame(Session *s, Session::Frame *f) {
    IOLockLock(s->lock);
    f->frame = INT64_MIN;
    while (s->frameTail != s->frameHead && s->frames[s->frameTail % 64].frame == INT64_MIN) ++s->frameTail;
    IOLockUnlock(s->lock);
}

// A coded CAPTURE buffer: its parameter sets kept, its picture handed over.
void handleCoded(Session *s, unsigned index, uint32_t bytesused, uint32_t flags, int64_t frame) {
    const uint8_t *p = s->capture[index].bytes, *end = p + bytesused;
    uint8_t *avcc = static_cast<uint8_t *>(IOMalloc(bytesused + 64));
    if (!avcc) return;
    uint32_t avccSize = 0;
    bool picture = false, key = flags & BufFlagKeyframe;
    const bool hevc = hevcSession(s);
    while (p < end) {
        // The next start code, then the NAL unit up to the one after it.
        const uint8_t *start = p;
        while (start + 3 <= end && !(start[0] == 0 && start[1] == 0 && start[2] == 1)) ++start;
        if (start + 3 > end) break;
        const uint8_t *nal = start + 3, *next = nal;
        while (next + 3 <= end && !(next[0] == 0 && next[1] == 0 && (next[2] == 1 || (next[2] == 0 && next + 3 < end && next[3] == 1)))) ++next;
        if (next + 3 > end) next = end;
        const uint32_t n = uint32_t(next - nal);
        p = next;
        if (!n) continue;
        const unsigned type = hevc ? (nal[0] >> 1) & 0x3f : nal[0] & 0x1f;
        uint8_t *set = nullptr;
        uint32_t *setSize = nullptr;
        if (hevc ? type == 32 : false) { set = s->vps; setSize = &s->vpsSize; }
        else if (hevc ? type == 33 : type == 7) { set = s->sps; setSize = &s->spsSize; }
        else if (hevc ? type == 34 : type == 8) { set = s->pps; setSize = &s->ppsSize; }
        if (set) {
            if (n <= 256 && (n != *setSize || memcmp(set, nal, n))) {
                memcpy(set, nal, n);
                *setSize = n;
                s->describedToGuest = false;
            }
            continue;
        }
        if (hevc ? (type == 35) : (type == 9)) continue;   // access unit delimiters
        const bool vcl = hevc ? type < 32 : (type >= 1 && type <= 5);
        if (vcl) { picture = true; if (hevc ? (type >= 16 && type <= 21) : type == 5) key = true; }
        const uint8_t length[4] = {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)};
        memcpy(avcc + avccSize, length, 4);
        memcpy(avcc + avccSize + 4, nal, n);
        avccSize += 4 + n;
    }
    IOLockLock(s->lock);
    Session::Frame *f = picture ? findFrame(s, frame) : nullptr;
    IOLockUnlock(s->lock);
    if (picture && !f) failed(86, int32_t(frame));
    if (f) {
        if (!s->describedToGuest) s->describedToGuest = describeToGuest(s);
        emitEncoded(s, *f, avcc, avccSize, key, 0);
        retireFrame(s, f);
    }
    IOFree(avcc, bytesused + 64);
}

int encoderCommand(Session *s, uint32_t command) {
    uint32_t cmd[10] = {command};
    return ioctl(s, IocEncoderCmd, cmd, sizeof cmd, sizeof cmd);
}

// ecfr's answer, once every frame given is handed over: a codec that keeps frames back is drained; at the deadline the frames
// still out are handed over dropped. The lock held.
void answerEncodeFinish(Session *s) {
    if (!s->finishWanted) return;
    bool fedAll = true;
    for (unsigned i = s->frameTail; i != s->frameHead; ++i) if (!s->frames[i % 64].fed) fedAll = false;
    const bool pending = s->frameTail != s->frameHead;
    if (pending && fedAll && !s->draining && mach_absolute_time() < s->finishDeadline) {
        s->draining = true;
        IOLockUnlock(s->lock);
        const int r = encoderCommand(s, EncCmdStop);
        IOLockLock(s->lock);
        if (r) { failed(87, r); s->draining = false; }
        return;
    }
    if (pending && mach_absolute_time() < s->finishDeadline) return;
    while (s->frameTail != s->frameHead) {
        Session::Frame f = s->frames[s->frameTail % 64];
        ++s->frameTail;
        if (f.frame == INT64_MIN) continue;
        IOLockUnlock(s->lock);
        emitEncoded(s, f, nullptr, 0, false, -12911);
        IOLockLock(s->lock);
    }
    Header h;
    h.type = cc("ecfr");
    memcpy(h.uuid, s->uuid, 16);
    h.request = s->finishRequest;
    s->finishWanted = false;
    IOLockUnlock(s->lock);
    replyStatus(s->client, h, 0);
    IOLockLock(s->lock);
}

void endEncodeDrain(Session *s) {
    IOLockLock(s->lock);
    const bool draining = s->draining;
    s->draining = false;
    IOLockUnlock(s->lock);
    if (draining) {
        const int r = encoderCommand(s, EncCmdStart);
        if (r) failed(88, r);
    }
}

void encoderWorkerMain(void *argument, wait_result_t) {
    Session *s = static_cast<Session *>(argument);
    for (;;) {
        IOLockLock(s->lock);
        for (;;) {
            if (s->stopping || s->eventHead != s->eventTail) break;
            if (feedFrame(s)) continue;
            answerEncodeFinish(s);
            if (s->stopping || s->eventHead != s->eventTail) break;
            if (s->finishWanted) IOLockSleepDeadline(s->lock, &s->events, s->finishDeadline, THREAD_UNINT);
            else IOLockSleep(s->lock, &s->events, THREAD_UNINT);
        }
        if (s->stopping) break;
        const Event e = s->events[s->eventTail++ % 64];
        IOLockUnlock(s->lock);
        if (e.kind == MediaEvtDqbuf && e.type == TypeOutputMplane && e.index < OutputBuffers) {
            IOLockLock(s->lock);
            s->output[e.index].queued = false;
            IOLockUnlock(s->lock);
        } else if (e.kind == MediaEvtDqbuf && e.type == TypeCaptureMplane && e.index < s->captureCount) {
            s->capture[e.index].queued = false;
            if (e.bytesused && !(e.flags & BufFlagError)) handleCoded(s, e.index, e.bytesused, e.flags, e.sec);
            if (e.flags & BufFlagLast) endEncodeDrain(s);
            requeueCapture(s, e.index);
        } else if (e.kind == MediaEvtEvent && e.type == EventEos) {
            endEncodeDrain(s);
        } else if (e.kind == MediaEvtError) {
            failed(89, 0);
        }
    }
    s->workerRunning = false;
    IOLockUnlock(s->lock);
    IOLockWakeup(s->lock, &s->workerRunning, false);
    thread_terminate(current_thread());
}

// Epba: the pixel buffers the encoder takes (420v of the session's size). Sent, not waited for: this runs on the guest's thread.
void encoderPixelBufferAttributes(Session *s) {
    Plist *p = new Plist();
    uint8_t buffer[512];
    if (!p) return;
    const int format = p->integer(875704438);   // '420v'
    const int keys[6] = {p->string("Width"), p->string("Height"), p->string("PixelFormatType"), p->string("BytesPerRowAlignment"),
                         p->string("PlaneAlignment"), p->string("IOSurfaceProperties")};
    const int values[6] = {p->integer(s->width), p->integer(s->height), p->array(&format, 1), p->integer(64), p->integer(64),
                           p->dict(nullptr, nullptr, 0)};
    const int attributes = p->dict(keys, values, 6);
    Writer w(buffer, sizeof buffer);
    w.header(cc("Epba"), FlagHostRequest, s->uuid, __atomic_fetch_add(&s->nextHostRequest, 1, __ATOMIC_RELAXED));
    w.plist(cc(",pba"), *p, attributes);
    sendToGuest(s->client, w, buffer);
    delete p;
}

// esta: the encoder's iris media session, both queues streaming.
int startEncoder(Session *s, const uint8_t *m, uint32_t n) {
    if (!VtEncoder) return -12906;
    Param size;
    if (!param(m, n, cc(",vdi"), size) || size.size < 8) return -12902;
    const uint32_t width = be32(size.value), height = be32(size.value + 4);
    if (!width || !height || (width & 1) || (height & 1)) return -12902;
    teardown(s);
    s->width = width;
    s->height = height;
    s->spsSize = s->ppsSize = s->vpsSize = 0;
    s->describedToGuest = false;
    s->frameHead = s->frameTail = 0;
    s->device = encoderDevice();
    if (s->device == ~0u) return -12908;
    int r = iris_media_open(s->device, &s->v4l2);
    if (r) { failed(90, r); return -12911; }
    s->open = true;
    registerSession(s);
    s->fast = true;
    uint32_t subscription[8] = {EventEos, 0, 0, 0, 0, 0, 0, 0};
    ioctl(s, IocSubscribeEvent, subscription, sizeof subscription, 0);
    V4l2Format f = {};
    f.type = TypeCaptureMplane;
    f.width = width;
    f.height = height;
    f.pixelformat = hevcSession(s) ? cc("CVEH") : cc("462H");
    f.num_planes = 1;
    f.planes[0].sizeimage = width * height / 2 + 65536;
    r = ioctl(s, IocSFmt, &f, sizeof f, sizeof f);
    if (r) { failed(91, r); return -12911; }
    f = {};
    f.type = TypeOutputMplane;
    f.width = width;
    f.height = height;
    f.pixelformat = cc("21VN");   // V4L2_PIX_FMT_NV12
    f.num_planes = 1;
    f.planes[0].bytesperline = width;
    f.planes[0].sizeimage = width * height * 3 / 2;
    r = ioctl(s, IocSFmt, &f, sizeof f, sizeof f);
    if (r) { failed(92, r); return -12911; }
    uint32_t request[5] = {OutputBuffers, TypeOutputMplane, MemoryUserPtr, 0, 0};
    r = ioctl(s, IocReqbufs, request, sizeof request, sizeof request);
    if (r || request[0] < OutputBuffers) { failed(93, r ? r : int32_t(request[0])); return -12904; }
    for (unsigned i = 0; i < OutputBuffers; ++i)
        if (!mapBuffer(s, TypeOutputMplane, i, s->output[i])) return -12904;
    uint32_t captures[5] = {6, TypeCaptureMplane, MemoryUserPtr, 0, 0};
    r = ioctl(s, IocReqbufs, captures, sizeof captures, sizeof captures);
    if (r || !captures[0]) { failed(94, r); return -12904; }
    s->captureCount = captures[0] < MaxCaptureBuffers ? captures[0] : MaxCaptureBuffers;
    for (unsigned i = 0; i < s->captureCount; ++i) {
        if (!mapBuffer(s, TypeCaptureMplane, i, s->capture[i])) return -12904;
        requeueCapture(s, i);
    }
    uint32_t type = TypeOutputMplane;
    r = ioctl(s, IocStreamon, &type, 4, 0);
    if (r) { failed(95, r); return -12911; }
    s->outputStreaming = true;
    type = TypeCaptureMplane;
    r = ioctl(s, IocStreamon, &type, 4, 0);
    if (r) { failed(96, r); return -12911; }
    s->captureStreaming = true;
    s->stopping = false;
    s->workerRunning = true;
    if (kernel_thread_start(encoderWorkerMain, s, &s->worker) != KERN_SUCCESS) { s->workerRunning = false; return -12904; }
    thread_deallocate(s->worker);
    encoderPixelBufferAttributes(s);
    trace("encoder session %u %ux%u %c%c%c%c", s->v4l2, width, height, s->codec >> 24, s->codec >> 16, s->codec >> 8, s->codec);
    return 0;
}

// efrm: the frame's IOSurface looked up on the guest's thread, the copy left to the worker.
int queueEncodeFrame(Session *s, const uint8_t *m, uint32_t n, const uint32_t *surfaces, unsigned surfaceCount) {
    if (!VtEncoder) return -12903;
    int64_t frame = 0;
    Param pts, duration, uuid;
    if (!s->outputStreaming || !surfaceCount || !paramS64(m, n, cc(",frm"), frame) || !param(m, n, cc(",pts"), pts) || pts.size < 24 ||
        !param(m, n, cc(",uui"), uuid) || uuid.size < 16) return -12902;
    SurfaceView view;
    bool surface = false;
    IOLockLock(s->surfaceLock);
    for (unsigned i = 0; i < s->knownCount && !surface; ++i)
        if (s->known[i].id == surfaces[0]) { view = s->known[i].view; surface = true; }
    if (!surface) {
        if (!s->surfaces) s->surfaces = openSurfaceClient(s->client->task);
        if (!s->holder) s->holder = processSurfaceClient(s->client->pid);
        surface = s->surfaces && s->holder && lookupSurface(s->surfaces, surfaces[0], view, s->holder);
        if (surface && s->knownCount < 48) s->known[s->knownCount++] = {surfaces[0], view};
    }
    IOLockUnlock(s->surfaceLock);
    if (!surface) { failed(97, int32_t(surfaces[0])); return -12902; }
    IOLockLock(s->lock);
    if (s->frameHead - s->frameTail >= 64) { IOLockUnlock(s->lock); failed(98, 0); return -12911; }
    Session::Frame &f = s->frames[s->frameHead++ % 64];
    f.frame = frame;
    memcpy(f.uuid, uuid.value, 16);
    memcpy(f.pts, pts.value, 24);
    if (param(m, n, cc(",dur"), duration) && duration.size >= 24) memcpy(f.duration, duration.value, 24);
    else memset(f.duration, 0, 24);
    f.view = view;
    f.fed = false;
    IOLockUnlock(s->lock);
    IOLockWakeup(s->lock, &s->events, false);
    vt_host_frames_in = vt_host_frames_in + 1;
    return 0;
}

// --- Requests from the guest -----------------------------------------------------------------------------------------------
Session *findSession(Client *c, const uint8_t *uuid) {
    for (Session *s = c->sessions; s; s = s->next)
        if (!memcmp(s->uuid, uuid, 16)) return s;
    return nullptr;
}

// The iris media session and everything of the device's the session holds (teardown, or its worker when the session idles).
void closeDevice(Session *s) {
    if (s->open) {
        uint32_t type = TypeOutputMplane;
        if (s->outputStreaming) ioctl(s, IocStreamoff, &type, 4, 0);
        type = TypeCaptureMplane;
        if (s->captureStreaming) ioctl(s, IocStreamoff, &type, 4, 0);
        for (Mapped &m : s->output) unmapBuffer(s, m);
        if (s->direct) giveSurfacesBack(s);
        for (Mapped &m : s->capture) unmapBuffer(s, m);
        unregisterSession(s);
        iris_media_close(s->device, s->v4l2);
    }
    s->open = s->outputStreaming = s->captureStreaming = s->fast = s->direct = false;
    s->captureCount = 0;
}

void teardown(Session *s) {
    if (s->workerRunning) {
        IOLockLock(s->lock);
        s->stopping = true;
        IOLockWakeup(s->lock, &s->events, false);
        IOLockWakeup(s->lock, &s->replied, false);
        while (s->workerRunning) IOLockSleep(s->lock, &s->workerRunning, THREAD_UNINT);
        IOLockUnlock(s->lock);
        s->stopping = false;
    }
    closeDevice(s);
    s->decodeOrder = false;
    s->sentDemtCount = 0;
    s->idleClosed = s->finished = s->evict = s->resync = false;
    s->evictDrainUntil = 0;
    s->idleDrained = false;
    s->statIn = s->statOut = s->statDropped = 0;
    s->lastOutAt = 0;
    s->shed = 0;
    s->shedAfter = 0;
    while (s->inputTail != s->inputHead) {
        const Session::Input &input = s->inputs[s->inputTail++ % MaxInputs];
        IOFree(input.bytes, input.capacity);
    }
    s->pendingFrames = 0;
    s->outstandingCount = 0;
    s->lastShownUs = INT64_MIN;
    for (Session::FrameTime &t : s->frameTimes) t = {-1, -1};
    s->lastInputUs = INT64_MIN;
    s->minStepUs = INT64_MAX;
    s->steps = 0;
    s->skipLogged = false;
    s->frameHead = s->frameTail = 0;
    s->finishWanted = s->draining = false;
    if (s->surfaces) {
        IOLockLock(s->surfaceLock);
        s->surfaces->clientClose();
        OSSafeReleaseNULL(s->surfaces);
        s->knownCount = 0;
        IOLockUnlock(s->surfaceLock);
    }
    OSSafeReleaseNULL(s->holder);
}

// dsup/esup: the properties the codec has, by name, each {PropertyType, ReadWriteStatus} (as macOS's host lists its
// hardware codecs'). VideoToolbox answers a property missing here itself (kVTPropertyNotSupportedErr) and sends no dcop/ecop
// or dset/eset for it: only that the codec runs on hardware, which dcop/ecop answer, is listed.
void supportedProperties(Client *c, const Header &h) {
    Plist *p = new Plist();
    uint8_t buffer[384];
    if (!p) return;
    const int typeKeys[2] = {p->string("PropertyType"), p->string("ReadWriteStatus")};
    const int typeValues[2] = {p->string("Boolean"), p->string("ReadOnly")};
    const int names[1] = {p->string(h.type == cc("dsup") ? "UsingHardwareAcceleratedVideoDecoder" : "UsingHardwareAcceleratedVideoEncoder")};
    const int values[1] = {p->dict(typeKeys, typeValues, 2)};
    const int supported = p->dict(names, values, 1);
    Writer w(buffer, sizeof buffer);
    w.header(h.type, FlagReply, h.uuid, h.request);
    w.s32(cc(",err"), 0);
    w.plist(cc(",sup"), *p, supported);
    sendToGuest(c, w, buffer);
    delete p;
}

bool hevcOffered() {
    static int offered = -1;
    if (offered < 0) {
        int value = 0;
        PE_parse_boot_argn("vt-hevc", &value, sizeof value);
        offered = value != 0;
    }
    return offered != 0;
}

void decoderList(Client *c, const Header &h) {
    Plist *p = new Plist();
    uint8_t *buffer = static_cast<uint8_t *>(IOMallocZero(4096));
    if (p && buffer) {
        // H.264 and HEVC: what the iris media decoder takes (ENUM_FMT) and VideoToolbox can describe for it.
        static const char *const codecs[2][3] = {
            {"avc1", "com.unixnu.videodecoder.iris.avc", "UniXNU Video Decoder - AVC (iris)"},
            {"hvc1", "com.unixnu.videodecoder.iris.hevc", "UniXNU Video Decoder - HEVC (iris)"},
        };
        int entries[2];
        for (int i = 0; i < 2; ++i) {
            const int type = p->string(codecs[i][0]);
            const int keys[5] = {p->string("VTCodecType"), p->string("CMClassImplementationName"), p->string("IsPreferredInternalPlugin"),
                                 p->string("VTRating"), p->string("CMClassImplementationID")};
            const int values[5] = {p->array(&type, 1), p->string(codecs[i][2]), p->boolean(false), p->integer(400), p->string(codecs[i][1])};
            entries[i] = p->dict(keys, values, 5);
        }
        // HEVC is listed only with vt-hevc=1: the VPU refuses to start an HEVC session on this board so far, and a decoder
        // that is listed and fails leaves the stream undecoded, where an unlisted one leaves it to VideoToolbox's own.
        const int list = p->array(entries, decoderDevice() == ~0u ? 0 : hevcOffered() ? 2 : 1);
        Writer w(buffer, 4096);
        w.header(h.type, FlagReply, h.uuid, h.request);
        w.plist(cc(",dec"), *p, list);
        w.u32(cc(",ver"), 6);
        sendToGuest(c, w, buffer);
    }
    delete p;
    if (buffer) IOFree(buffer, 4096);
}

// encs: H.264 and HEVC encoders over the iris media encoder (VTCodecType a number here, as the Mac's host answers).
void encoderList(Client *c, const Header &h) {
    Plist *p = new Plist();
    uint8_t *buffer = static_cast<uint8_t *>(IOMallocZero(4096));
    if (p && buffer) {
        static const struct { uint32_t type; const char *name, *id, *description; } codecs[2] = {
            {cc("avc1"), "H.264", "com.unixnu.videoencoder.iris.avc", "UniXNU Video Encoder - AVC (iris)"},
            {cc("hvc1"), "HEVC", "com.unixnu.videoencoder.iris.hevc", "UniXNU Video Encoder - HEVC (iris)"},
        };
        int entries[2] = {};
        for (int i = 0; VtEncoder && i < 2; ++i) {
            const int keys[5] = {p->string("VTCodecType"), p->string("VTCodecName"), p->string("CMClassImplementationName"),
                                 p->string("VTRating"), p->string("CMClassImplementationID")};
            const int values[5] = {p->integer(codecs[i].type), p->string(codecs[i].name), p->string(codecs[i].description),
                                   p->integer(400), p->string(codecs[i].id)};
            entries[i] = p->dict(keys, values, 5);
        }
        const int list = p->array(entries, VtEncoder && encoderDevice() != ~0u ? 2 : 0);
        Writer w(buffer, 4096);
        w.header(h.type, FlagReply, h.uuid, h.request);
        w.plist(cc(",enc"), *p, list);
        w.u32(cc(",ver"), 6);
        sendToGuest(c, w, buffer);
    }
    delete p;
    if (buffer) IOFree(buffer, 4096);
}

void createDecoder(Client *c, const Header &h, const uint8_t *m, uint32_t n) {
    int32_t codec = 0;
    paramS32(m, n, cc(",cod"), codec);
    Session *s = findSession(c, h.uuid);
    if (!s) {
        s = new Session();
        if (!s || !(s->lock = IOLockAlloc()) || !(s->surfaceLock = IOLockAlloc())) {
            if (s && s->lock) IOLockFree(s->lock);
            delete s;
            replyStatus(c, h, -12904);
            return;
        }
        s->client = c;
        memcpy(s->uuid, h.uuid, 16);
        IOLockLock(c->listLock);
        s->next = c->sessions;
        c->sessions = s;
        IOLockUnlock(c->listLock);
        vt_host_sessions = vt_host_sessions + 1;
    }
    s->codec = uint32_t(codec);
    s->encoder = h.type == cc("+enc");
    Plist *p = new Plist();
    uint8_t buffer[256];
    if (p) {
        const int keys[1] = {p->string("VTHostBuildVersion")};
        const int values[1] = {p->string("UniXNU iris")};
        const int info = p->dict(keys, values, 1);
        Writer w(buffer, sizeof buffer);
        w.header(h.type, FlagReply, h.uuid, h.request);
        w.s32(cc(",err"), 0);
        w.u32(cc(",ver"), 6);
        w.plist(cc(",hin"), *p, info);
        sendToGuest(c, w, buffer);
    }
    delete p;
}

// Dpba: the pixel buffers the guest should make for this session's frames.
void pixelBufferAttributes(Session *s) {
    Plist *p = new Plist();
    uint8_t buffer[512];
    if (!p) return;
    // '420v', or 'x420' for a 10-bit stream: the player of a 10-bit (HDR) video takes nothing else and drops 420v frames
    // unseen (the lock screen's aerials stayed black), so the device's 8 bits are widened into it (copyFrame).
    const int format = p->integer(s->tenBit ? int64_t(cc("x420")) : 875704438);
    // A 10-bit stream's surfaces as tall as the device's padded rows (P3): the reference decoder's P010 puts the chroma right after a luma
    // plane of the height aligned to 32 (2176 for 2160). The device's own number once its CAPTURE format is known
    // (setUpCapture sends Dpba again if it differs), this guess till then.
    const int keys[7] = {p->string("Width"), p->string("Height"), p->string("PixelFormatType"), p->string("BytesPerRowAlignment"),
                         p->string("PlaneAlignment"), p->string("IOSurfaceProperties"), p->string("ExtendedPixelsBottom")};
    // Planes aligned to the host's pages for P3: CoreVideo puts the luma 512 bytes into the surface at 256, and the device takes
    // only page-aligned guest memory (udmabuf); at 4096 the luma starts at 4096 and the chroma right after its padded rows.
    const int values[7] = {p->integer(s->width), p->integer(s->height), p->array(&format, 1), p->integer(s->tenBit ? 64 : 128),
                           p->integer(directLayout() ? 4096 : 256),
                           p->dict(nullptr, nullptr, 0), p->integer(s->extendBottom)};
    const int attributes = p->dict(keys, values, s->extendBottom ? 7 : 6);
    Writer w(buffer, sizeof buffer);
    w.header(cc("Dpba"), 0, s->uuid, 0);
    w.plist(cc(",pba"), *p, attributes);
    sendToGuest(s->client, w, buffer);
    delete p;
}

// What the decoder is not given, so that VideoToolbox decodes it in software instead: a picture outside the coded sizes
// the device reports (ENUM_FRAMESIZES; Qualcomm's start at 96x96), or a stream that is not 4:2:0, since the CAPTURE queue is
// NV12 alone (H.264 High 10, 4:2:2 and 4:4:4 profiles, which the codec fails frame by frame; HEVC by the hvcC's chroma format
// and bit depths: Main 10 comes out as 8-bit NV12, which the 420v pixel buffers VideoToolbox is given want anyway).
bool decodableFormat(const ImageDescription &d) {
    Atom record;
    if (find(d.extensions, d.extensionsSize, cc("avcC"), record)) {
        if (record.size < 4) return false;
        const uint8_t profile = record.body[1];
        return profile == 66 || profile == 77 || profile == 100;   // Baseline, Main, High
    }
    if (find(d.extensions, d.extensionsSize, cc("hvcC"), record)) {
        if (record.size < 23) return false;
        const uint8_t chroma = record.body[16] & 3, lumaDepth = (record.body[17] & 7) + 8, chromaDepth = (record.body[18] & 7) + 8;
        return chroma == 1 && lumaDepth <= 10 && chromaDepth <= 10;
    }
    return false;
}

bool decodableSize(Session *s, uint32_t pixelformat) {
    uint32_t sizes[11] = {0, pixelformat};   // struct v4l2_frmsizeenum: index, pixel_format, type, stepwise, reserved
    const int r = ioctl(s, IocEnumFramesizes, sizes, sizeof sizes, sizeof sizes);
    if (r) return true;   // a device that does not say lets the codec decide
    const uint32_t minWidth = sizes[3], maxWidth = sizes[4], minHeight = sizes[6], maxHeight = sizes[7];
    const bool fits = s->width >= minWidth && s->width <= maxWidth && s->height >= minHeight && s->height <= maxHeight;
    if (!fits) trace("%ux%u outside %u..%u x %u..%u", s->width, s->height, minWidth, maxWidth, minHeight, maxHeight);
    return fits;
}

void clearReplay(Session *s) {
    for (unsigned i = 0; i < s->replayCount; ++i) IOFree(s->replay[i].bytes, s->replay[i].size);
    s->replayCount = 0;
    s->replaySize = 0;
    s->replayable = false;
}

// The saved frames given to the device again ahead of the next (the lock held, the device session just started again): decoded
// for the frames after them to refer to, none handed over (not outstanding, emitFrame and feedInput pass them by).
void replayInputs(Session *s) {
    for (unsigned i = 0; i < s->replayCount; ++i) {
        const Session::Saved &saved = s->replay[i];
        const uint32_t prefix = s->parametersSent ? 0 : s->parameterSetsSize;
        uint8_t *bytes = static_cast<uint8_t *>(IOMalloc(prefix + saved.size));
        if (!bytes) return;
        memcpy(bytes, s->parameterSets, prefix);
        memcpy(bytes + prefix, saved.bytes, saved.size);
        s->parametersSent = true;
        s->inputs[s->inputHead++ % MaxInputs] = {saved.frame, bytes, prefix + saved.size, prefix + saved.size, saved.ptsUs, saved.tid};
    }
}

void forgetStartMessage(Session *s) {
    clearReplay(s);
    if (s->startMessage) IOFree(s->startMessage, s->startSize);
    s->startMessage = nullptr;
    s->startSize = 0;
}

void keepStartMessage(Session *s, const uint8_t *m, uint32_t n) {
    if (s->startMessage == m) return;
    forgetStartMessage(s);
    if (!(s->startMessage = static_cast<uint8_t *>(IOMalloc(n)))) return;
    memcpy(s->startMessage, m, n);
    s->startSize = n;
}

int startSession(Session *s, const uint8_t *m, uint32_t n, bool fresh = true);

// A frame for a session whose device session went idle (workerMain): a new one, started as the last dsta did. A stream goes on
// from a sync sample after a dfin (a seek, a loop); a frame before one decodes wrong until the next.
void resumeSession(Session *s) {
    if (!s->startMessage) return;
    // The same stream: the layers kept stay as they were (frames above them were never given, and nothing can refer to them).
    const int64_t minStepUs = s->minStepUs, lastInputUs = s->lastInputUs;
    const unsigned steps = s->steps;
    const int error = startSession(s, s->startMessage, s->startSize, false);
    if (error) failed(90, error);
    s->minStepUs = minStepUs;
    s->lastInputUs = lastInputUs;
    s->steps = steps;
}

// Room for `self`'s device session (SoftDeviceSessions, HardDeviceSessions): the least recently fed other sessions told to let
// theirs go, and waited for -- briefly, and by looking in gTable only: the session may be another process's, gone at any time
// outside gTableLock. Its worker lets go at its next turn and waits on nothing of this thread's. `force`: the device refused
// `self`, the least recently fed other session goes whatever the counts. Whether any went.
bool makeRoom(Session *self, bool force = false) {
    bool any = false;
    uint64_t idleTicks;
    nanoseconds_to_absolutetime(uint64_t(EvictIdleMs) * 1000000, &idleTicks);
    for (;;) {
        const uint64_t now = mach_absolute_time();
        IOLockLock(gTableLock);
        unsigned open = 0;
        Session *oldest = nullptr, *oldestIdle = nullptr, *oldestDone = nullptr;
        for (Session *s : gTable) {
            if (!s || s == self || !s->open || !s->workerRunning || s->evict) continue;
            ++open;
            if (!oldest || s->lastInputAt < oldest->lastInputAt) oldest = s;
            const bool drainable = s->outstandingCount <= EvictFrames && s->inputHead == s->inputTail;
            if (drainable && (!oldestDone || s->lastInputAt < oldestDone->lastInputAt)) oldestDone = s;
            // Of those idle for EvictIdleMs with every frame fed and at most EvictFrames to come out (drained before it goes),
            // one whose stream finished (dfin) first, then the least recently fed. Not one with more: the player reads a clip in
            // at once and shows it over seconds -- the clip on screen, cut (11-04: its frames failed, the player killed).
            if (now - s->lastInputAt >= idleTicks && drainable &&
                (!oldestIdle || (s->finished && !oldestIdle->finished) ||
                 (s->finished == oldestIdle->finished && s->lastInputAt < oldestIdle->lastInputAt)))
                oldestIdle = s;
        }
        // Never one with more than EvictFrames still to hand over, but for a device refusal: the player shows a clip seconds
        // after reading it in, and a clip on screen whose frames all came back without pixels killed it (11-04 on the reference device, v3:
        // dropped or failed alike). Past HardDeviceSessions, the least recently fed of those, idle or not (it starts again).
        Session *victim = force && !any ? oldest : open >= HardDeviceSessions ? oldestDone : open >= SoftDeviceSessions ? oldestIdle : nullptr;
        if (victim) {
            IOLockLock(victim->lock);
            victim->evict = true;
            IOLockUnlock(victim->lock);
            IOLockWakeup(victim->lock, &victim->events, false);
            IOLockWakeup(victim->lock, &victim->replied, false);
            IOLog("vt %u let go (%u open, last %llu ms)\n", victim->v4l2, open,
                  (now - victim->lastInputAt) * EvictIdleMs / idleTicks);
        }
        IOLockUnlock(gTableLock);
        if (!victim) return any;
        any = true;
        for (unsigned waited = 0; waited < 500 + EvictDrainMs; waited += 2) {
            bool registered = false;
            IOLockLock(gTableLock);
            for (Session *s : gTable) registered = registered || s == victim;
            IOLockUnlock(gTableLock);
            if (!registered) break;
            IOSleep(2);
        }
    }
}

// `fresh`: a dsta's (a device refusal then makes room by force, once); not a session started again after it was let go, which
// would take the device from another as often as both get a sync sample.
int startSession(Session *s, const uint8_t *m, uint32_t n, bool fresh) {
    Param fds;
    ImageDescription d;
    if (!param(m, n, cc(",fds"), fds) || !imageDescription(fds.value, fds.size, d)) return -12902;
    if (d.codec != cc("avc1") && d.codec != cc("hvc1") && d.codec != cc("hev1")) return -12906;
    if (d.codec != cc("avc1") && !hevcOffered()) return -12906;   // not listed: VideoToolbox decodes it itself
    if (!decodableFormat(d)) return -12910;   // kVTVideoDecoderUnsupportedDataFormatErr
    if (!parameterSets(s, d)) return -12909;
    teardown(s);
    s->lastInputAt = mach_absolute_time();
    s->width = d.width;
    s->height = d.height;
    s->parametersSent = false;
    s->pendingFrames = 0;
    s->device = decoderDevice();
    if (s->device == ~0u) return -12906;
    makeRoom(s);
    int r = iris_media_open(s->device, &s->v4l2);
    if (r) { failed(1, r); return -12911; }
    s->open = true;
    // A session that does not start lets its device session go at once (VideoToolbox decodes in software then; held, it was a
    // device session no one would close until the dinv).
    auto refuse = [s](int error) { closeDevice(s); return error; };
    registerSession(s);
    s->fast = true;
    if (!decodableSize(s, d.codec == cc("avc1") ? cc("462H") : cc("CVEH"))) { teardown(s); return -12910; }
    uint32_t subscription[8] = {EventSourceChange, 0, 0, 0, 0, 0, 0, 0};
    r = ioctl(s, IocSubscribeEvent, subscription, sizeof subscription, 0);
    if (r) failed(2, r);
    subscription[0] = EventEos;
    ioctl(s, IocSubscribeEvent, subscription, sizeof subscription, 0);
    V4l2Format f = {};
    f.type = TypeOutputMplane;
    f.width = d.width;
    f.height = d.height;
    f.pixelformat = d.codec == cc("avc1") ? cc("462H") : cc("CVEH");   // V4L2_PIX_FMT_H264 'H264', _HEVC 'HEVC' (little endian)
    f.num_planes = 1;
    f.planes[0].sizeimage = OutputBytes;
    r = ioctl(s, IocSFmt, &f, sizeof f, sizeof f);
    if (r) { failed(3, r); return refuse(-12911); }
    // Decode order (display delay 0): every frame out as soon as decoded, as Apple's host gives them (a device without it says
    // EINVAL: display order, every dfrm answered at once as before).
    uint32_t control[2] = {CidDecDisplayDelayEnable, 1};
    s->decodeOrder = !ioctl(s, IocSCtrl, control, sizeof control, sizeof control);
    control[0] = CidDecDisplayDelay;
    control[1] = 0;
    if (s->decodeOrder) s->decodeOrder = !ioctl(s, IocSCtrl, control, sizeof control, sizeof control);
    uint32_t request[5] = {OutputBuffers, TypeOutputMplane, MemoryUserPtr, 0, 0};
    r = ioctl(s, IocReqbufs, request, sizeof request, sizeof request);
    if (r || request[0] < OutputBuffers) { failed(4, r ? r : int32_t(request[0])); return refuse(-12904); }
    for (unsigned i = 0; i < OutputBuffers; ++i)
        if (!mapBuffer(s, TypeOutputMplane, i, s->output[i])) return refuse(-12904);
    uint32_t type = TypeOutputMplane;
    r = ioctl(s, IocStreamon, &type, 4, 0);
    if (r) {
        failed(5, r);
        refuse(0);
        if (fresh && makeRoom(s, true)) return startSession(s, m, n, false);
        return -12911;
    }
    s->outputStreaming = true;
    s->stopping = false;
    s->workerRunning = true;
    if (kernel_thread_start(workerMain, s, &s->worker) != KERN_SUCCESS) { s->workerRunning = false; return refuse(-12904); }
    thread_deallocate(s->worker);
    s->extendBottom = directLayout() ? ((s->height + 31) & ~31u) - s->height : 0;
    pixelBufferAttributes(s);
    IOLog("vt: session %u %ux%u %c%c%c%c\n", s->v4l2, s->width, s->height, d.codec >> 24, d.codec >> 16, d.codec >> 8, d.codec);
    return 0;
}

// The presentation time of a sample buffer's first sample, in microseconds, from its stia (CMSampleTimingInfo: duration,
// presentation and decode CMTime, each value int64, timescale int32, flags uint32, epoch int64, little endian); -1 if none.
int64_t presentationUs(const Atom &sbuf) {
    Atom stia;
    if (!find(sbuf.body, sbuf.size, cc("stia"), stia) || stia.size < 48) return -1;
    int64_t value;
    int32_t timescale;
    uint32_t flags;
    memcpy(&value, stia.body + 24, 8);
    memcpy(&timescale, stia.body + 32, 4);
    memcpy(&flags, stia.body + 36, 4);
    if (!(flags & 1) || timescale <= 0 || value < 0) return -1;   // kCMTimeFlags_Valid
    return value / timescale * 1000000 + value % timescale * 1000000 / timescale;
}

// Whether a sample is a sync sample: its first VCL NAL unit an HEVC IRAP picture (types 16-23) or an H.264 IDR one (5).
bool syncSample(const Session *s, const uint8_t *p, const uint8_t *end) {
    while (uint32_t(end - p) > s->nalLength) {
        uint32_t length = 0;
        for (uint32_t i = 0; i < s->nalLength; ++i) length = length << 8 | p[i];
        p += s->nalLength;
        if (!length || length > uint32_t(end - p)) return false;
        if (hevcSession(s)) {
            const unsigned type = (p[0] >> 1) & 0x3f;
            if (type < 32) return type >= 16 && type <= 23;
        } else {
            const unsigned type = p[0] & 0x1f;
            if (type >= 1 && type <= 5) return type == 5;
        }
        p += length;
    }
    return false;
}

// dfrm: the frame's NAL units as Annex B, kept for the worker (selector 3 never waits for an OUTPUT buffer).
int queueFrame(Session *s, const uint8_t *m, uint32_t n);

// A dfrm: a synchronous one, in decode order, answered after its frame's Demt (answerFrame: Deferred); otherwise answered by the
// caller with what queueFrame says, at once.
constexpr int Deferred = 1;
int queueFrameAnswered(Session *s, const uint8_t *m, uint32_t n, uint64_t request, bool sync) {
    int64_t frame = 0;
    paramS64(m, n, cc(",frm"), frame);
    IOLockLock(s->lock);
    const bool deferred = sync && s->decodeOrder && s->syncReplyCount < MaxSyncReplies;
    if (deferred) s->syncReplies[s->syncReplyCount++] = {frame, request, 0, false};
    IOLockUnlock(s->lock);
    const int error = queueFrame(s, m, n);
    if (!deferred && !sync && !error) {
        // Asynchronous: answered once fewer than AsyncInFlight of the session's frames are out.
        IOLockLock(s->lock);
        const bool held = s->decodeOrder && asyncOut(s) > AsyncInFlight && s->syncReplyCount < MaxSyncReplies;
        if (held) s->syncReplies[s->syncReplyCount++] = {frame, request, 0, true};
        IOLockUnlock(s->lock);
        if (!held) return 0;
        releaseAsync(s);   // a frame handed over meanwhile
        return Deferred;
    }
    if (!deferred) return error;
    if (error) answerFrame(s, frame, error);   // never queued: answered now (nothing if a Demt answered it already)
    return Deferred;
}

int queueFrame(Session *s, const uint8_t *m, uint32_t n) {
    IOLockLock(s->lock);   // the worker's idle close reads them
    s->lastInputAt = mach_absolute_time();
    s->finished = false;
    s->idleDrained = false;
    IOLockUnlock(s->lock);
    int64_t frame = 0;
    paramS64(m, n, cc(",frm"), frame);
    Param sbf;
    Atom sbuf, sdat;
    if (!param(m, n, cc(",sbf"), sbf) || !find(sbf.value, sbf.size, cc("sbuf"), sbuf) || !find(sbuf.body, sbuf.size, cc("sdat"), sdat))
        return -12902;
    const bool sync = syncSample(s, sdat.body, sdat.body + sdat.size);
    // Its device session let go, or behind: nothing till a sync sample, which (re)starts the device session as the dsta did.
    // A session let go with every frame since its last sync sample kept starts again at any frame, from those.
    if (s->resync) {
        if (!sync && !(s->idleClosed && s->replayable && s->replayCount)) {
            handOverDropped(s, frame, false);
            vt_host_frames_in = vt_host_frames_in + 1;
            return 0;
        }
        if (s->idleClosed) {
            if (s->lastInputAt >= s->resumeAfter) resumeSession(s);
            if (!s->outputStreaming) {   // the device would not: a sync sample a second later tries again
                s->idleClosed = s->resync = true;
                if (s->lastInputAt >= s->resumeAfter) {
                    uint64_t second;
                    nanoseconds_to_absolutetime(1000000000ULL, &second);
                    s->resumeAfter = s->lastInputAt + second;
                }
                handOverDropped(s, frame, false);
                vt_host_frames_in = vt_host_frames_in + 1;
                return 0;
            }
            if (!sync) {
                IOLockLock(s->lock);
                replayInputs(s);
                IOLockUnlock(s->lock);
            }
            IOLog("vt %u again at %lld (%u replayed)\n", s->v4l2, frame,
                  sync ? 0 : s->replayCount);
        }
        s->resync = false;
    }
    if (!s->outputStreaming) return -12902;
    const uint32_t capacity = sdat.size + (s->nalLength < 4 ? sdat.size : 0) + (s->parametersSent ? 0 : s->parameterSetsSize) + 64;
    uint8_t *bytes = static_cast<uint8_t *>(IOMalloc(capacity));
    if (!bytes) return -12904;
    uint32_t at = 0;
    const uint8_t start[4] = {0, 0, 0, 1};
    if (!s->parametersSent) {
        memcpy(bytes, s->parameterSets, s->parameterSetsSize);
        at = s->parameterSetsSize;
        s->parametersSent = true;
    }
    const uint32_t prefix = at;
    const uint8_t *p = sdat.body, *end = sdat.body + sdat.size;
    // The temporal layer of the frame's first VCL NAL unit (HEVC types 0-31; nuh_temporal_id_plus1 the low 3 bits of byte 2).
    uint8_t tid = 0;
    bool vcl = !hevcSession(s);
    while (uint32_t(end - p) > s->nalLength) {
        uint32_t length = 0;
        for (uint32_t i = 0; i < s->nalLength; ++i) length = length << 8 | p[i];
        p += s->nalLength;
        if (length > uint32_t(end - p) || at + 4 + length > capacity) { failed(7, int32_t(length)); break; }
        if (!vcl && length >= 2 && ((p[0] >> 1) & 0x3f) < 32) { vcl = true; tid = (p[1] & 7) ? uint8_t((p[1] & 7) - 1) : 0; }
        memcpy(bytes + at, start, 4);
        memcpy(bytes + at + 4, p, length);
        at += 4 + length;
        p += length;
    }
    IOLockLock(s->lock);
    const int64_t ptsUs = presentationUs(sbuf);
    if (ptsUs >= 0 && s->lastInputUs != INT64_MIN) {
        const int64_t step = ptsUs > s->lastInputUs ? ptsUs - s->lastInputUs : s->lastInputUs - ptsUs;
        if (step > 0 && step < 1000000) { if (step < s->minStepUs) s->minStepUs = step; ++s->steps; }
    }
    if (ptsUs >= 0) s->lastInputUs = ptsUs;
    if (tid > keptLayer(s)) {
        // Z0 here and now: handed over as dropped on the player's own thread, which is held back by it -- queued, the 7 in 8
        // the device never sees outran the worker and filled MaxInputs (-12911) at the player's 4000-5000 frames a second.
        // VideoToolbox takes frames back in any order (decode order is not output order).
        IOLockUnlock(s->lock);
        IOFree(bytes, capacity);
        handOverDropped(s, frame, false);
        vt_host_frames_skipped = vt_host_frames_skipped + 1;
        vt_host_frames_in = vt_host_frames_in + 1;
        return 0;
    }
    if (!s->decodeOrder && s->inputHead - s->inputTail >= MaxBacklog && hevcSession(s) && s->temporalLayers >= 2 && keptLayer(s) > 0 &&
        s->lastInputAt >= s->shedAfter) {
        // Behind: one temporal layer more given up; feedInput hands over the frames of it already waiting, as dropped.
        ++s->shed;
        uint64_t wait;
        nanoseconds_to_absolutetime(uint64_t(ShedMs) * 1000000, &wait);
        s->shedAfter = s->lastInputAt + wait;
        const unsigned kept = keptLayer(s), waiting = s->inputHead - s->inputTail;
        IOLockUnlock(s->lock);
        IOLog("vt %u behind (%u): keeps layers 0-%u\n", s->v4l2, waiting, kept);
        IOLockLock(s->lock);
    }
    if (s->inputHead - s->inputTail >= MaxInputs) {
        IOLockUnlock(s->lock);
        IOFree(bytes, capacity);
        failed(6, 0);
        return -12911;
    }
    // Kept for a start again: from a sync sample on, while all fit.
    if (sync) { clearReplay(s); s->replayable = true; }
    if (s->replayable) {
        uint8_t *copy = nullptr;
        if (s->replayCount < MaxReplay && s->replaySize + (at - prefix) <= MaxReplayBytes &&
            (copy = static_cast<uint8_t *>(IOMalloc(at - prefix)))) {
            memcpy(copy, bytes + prefix, at - prefix);
            s->replay[s->replayCount++] = {frame, copy, at - prefix, ptsUs, tid};
            s->replaySize += at - prefix;
        } else {
            clearReplay(s);
        }
    }
    s->inputs[s->inputHead++ % MaxInputs] = {frame, bytes, at, capacity, ptsUs, tid};
    ++s->pendingFrames;
    ++s->statIn;
    if (s->outstandingCount < MaxInputs) s->outstanding[s->outstandingCount++] = frame;
    IOLockUnlock(s->lock);
    IOLockWakeup(s->lock, &s->events, false);
    vt_host_frames_in = vt_host_frames_in + 1;
    return 0;
}

}  // namespace

extern "C" {

// Whether the board has a media decoder or encoder: the transport publishes Apple's driver only then.
bool vt_host_available(void) { return decoderDevice() != ~0u || encoderDevice() != ~0u; }

void *vt_host_client_open(void *transport, bool (*send)(void *, const uint8_t *, uint32_t), task_t task, int pid) {
    if (!gTableLock) {
        IOLock *lock = IOLockAlloc();
        if (!OSCompareAndSwapPtr(nullptr, lock, &gTableLock)) IOLockFree(lock);
    }
    if (!gSgLock) {
        IOLock *lock = IOLockAlloc();
        if (!OSCompareAndSwapPtr(nullptr, lock, &gSgLock)) IOLockFree(lock);
    }
    Client *c = new Client();
    if (!c) return nullptr;
    c->transport = transport;
    c->send = send;
    c->task = task;
    c->pid = pid;
    c->lock = IOLockAlloc();
    c->listLock = IOLockAlloc();
    c->sessions = nullptr;
    return c;
}

void vt_host_client_close(void *host) {
    Client *c = static_cast<Client *>(host);
    if (!c) return;
    for (;;) {
        IOLockLock(c->listLock);
        Session *s = c->sessions;
        if (s) c->sessions = s->next;
        IOLockUnlock(c->listLock);
        if (!s) break;
        teardown(s);
        answerAllFrames(s, -12903);
        IOLockFree(s->lock);
        IOLockFree(s->surfaceLock);
        forgetStartMessage(s);
        delete s;
    }
    if (c->lock) IOLockFree(c->lock);
    if (c->listLock) IOLockFree(c->listLock);
    delete c;
}

// One message from the guest, on the guest's thread, with the IDs of the IOSurfaces that came with it.
int vt_host_message(void *host, const uint8_t *m, uint32_t n, const uint32_t *surfaces, unsigned surfaceCount) {
    Client *c = static_cast<Client *>(host);
    Header h;
    if (!c || !header(m, n, h)) return -1;
    // The guest's replies to the host's requests (Dbuf, Demt).
    if (h.flags & FlagGuestReply) {
        IOLockLock(c->listLock);
        Session *s = findSession(c, h.uuid);
        if (!s) { IOLockUnlock(c->listLock); return 0; }
        int32_t error = 0;
        paramS32(m, n, cc(",err"), error);
        Param uuid;
        SurfaceView view;
        bool surface = false;
        if (h.type == cc("Dbuf") && !error && surfaceCount && param(m, n, cc(",uui"), uuid) && uuid.size >= 16) {
            IOLockLock(s->surfaceLock);
            for (unsigned i = 0; i < s->knownCount && !surface; ++i)
                if (s->known[i].id == surfaces[0]) { view = s->known[i].view; surface = true; }
            if (!surface) {
                if (!s->surfaces) s->surfaces = openSurfaceClient(c->task);
                surface = s->surfaces && !s->holder && lookupSurface(s->surfaces, surfaces[0], view);
                // A pool another process made (WebKit's: the task may not look it up by ID, kIOReturnNotPermitted): through the
                // process's own client, as an efrm's frame.
                if (!surface && s->surfaces) {
                    if (!s->holder) s->holder = processSurfaceClient(c->pid);
                    surface = s->holder && lookupSurface(s->surfaces, surfaces[0], view, s->holder);
                }
                if (surface && s->knownCount < 48) s->known[s->knownCount++] = {surfaces[0], view};
            }
            IOLockUnlock(s->surfaceLock);
            if (!surface) failed(42, int32_t(surfaces[0]));
        }
        IOLockLock(s->lock);
        if (s->awaitedRequest == h.request) {
            s->replyError = error;
            s->replyHasSurface = surface;
            if (surface) { s->replySurface = view; memcpy(s->replyUuid, uuid.value, 16); }
            s->replied = true;
        }
        IOLockUnlock(s->lock);
        IOLockWakeup(s->lock, &s->replied, false);
        if (h.type == cc("Demt")) {
            answerFrame(s, 0, 0, h.request);   // a dropped frame's sent from here (sendDemt)
            forgetDemt(s, h.request);
            releaseAsync(s);
        }
        IOLockUnlock(c->listLock);
        return 0;
    }
    IOLockLock(c->lock);
    Session *s = findSession(c, h.uuid);
    switch (h.type) {
    case cc("decs"): decoderList(c, h); break;
    case cc("encs"): encoderList(c, h); break;
    case cc("+enc"): createDecoder(c, h, m, n); break;
    case cc("eset"): case cc("emul"): case cc("epfr"):
        replyStatus(c, h, 0);
        break;
    case cc("esta"): replyStatus(c, h, s ? startEncoder(s, m, n) : -12903); break;
    case cc("efrm"): {
        const int error = s ? queueEncodeFrame(s, m, n, surfaces, surfaceCount) : -12903;
        uint8_t buffer[128];
        Writer w(buffer, sizeof buffer);
        w.header(h.type, FlagReply, h.uuid, h.request);
        w.s32(cc(",err"), error);
        w.u32(cc(",inf"), 1);
        sendToGuest(c, w, buffer);
        break;
    }
    case cc("ecfr"):
        if (s && s->workerRunning) {
            IOLockLock(s->lock);
            s->finishWanted = true;
            s->finishRequest = h.request;
            clock_interval_to_deadline(2000, kMillisecondScale, &s->finishDeadline);
            IOLockUnlock(s->lock);
            IOLockWakeup(s->lock, &s->events, false);
        } else {
            replyStatus(c, h, 0);
        }
        break;
    case cc("einv"):
        if (s) teardown(s);
        replyStatus(c, h, 0);
        break;
    case cc("-enc"):
        if (s) {
            teardown(s);
            IOLockLock(c->listLock);
            for (Session **at = &c->sessions; *at; at = &(*at)->next)
                if (*at == s) { *at = s->next; break; }
            IOLockUnlock(c->listLock);
            IOLockFree(s->lock);
            IOLockFree(s->surfaceLock);
            forgetStartMessage(s);
            delete s;
        }
        break;
    case cc("+dec"): createDecoder(c, h, m, n); break;
    case cc("dset"): replyStatus(c, h, 0); break;
    case cc("dmul"): replyStatus(c, h, 0); break;
    case cc("dsta"): {
        const int error = s ? startSession(s, m, n) : -12903;
        if (!error) keepStartMessage(s, m, n);
        replyStatus(c, h, error);
        break;
    }
    case cc("dsup"): case cc("esup"):
        supportedProperties(c, h);
        break;
    case cc("dcop"): case cc("ecop"): {
        // Properties the session asks of the codec. The one it has is that it runs on hardware; anything else (an HEVC encoder
        // session asks MVHEVCVideoLayerIDs, the layers of a multiview stream) is kVTPropertyNotSupportedErr, as a codec without it
        // answers.
        Param key;
        char name[96] = "";
        if (param(m, n, cc(",key"), key)) plistString(key.value, key.size, name, sizeof name);
        if (!strcmp(name, h.type == cc("dcop") ? "UsingHardwareAcceleratedVideoDecoder" : "UsingHardwareAcceleratedVideoEncoder")) {
            Plist *p = new Plist();
            uint8_t buffer[192];
            if (p) {
                const int yes = p->boolean(true);
                Writer w(buffer, sizeof buffer);
                w.header(h.type, FlagReply, h.uuid, h.request);
                w.s32(cc(",err"), 0);
                w.plist(cc(",val"), *p, yes);
                sendToGuest(c, w, buffer);
            }
            delete p;
        } else {
            replyStatus(c, h, -12900);
        }
        break;
    }
    case cc("dfrm"): {
        // ,dfl: kVTDecodeFrame_EnableAsynchronousDecompression the low bit; absent, 0 (synchronous).
        int32_t flags = 0;
        paramS32(m, n, cc(",dfl"), flags);
        const bool sync = !(flags & 1);
        if (s && !s->dflLogged) {
            s->dflLogged = true;
            IOLog("vt %u first dfrm ,dfl %d (%s, %s order)\n", s->v4l2, flags, sync ? "sync" : "async",
                  s->decodeOrder ? "decode" : "display");
        }
        const int error = s ? queueFrameAnswered(s, m, n, h.request, sync) : -12903;
        if (error == Deferred) break;
        uint8_t buffer[128];
        Writer w(buffer, sizeof buffer);
        w.header(h.type, FlagReply, h.uuid, h.request);
        w.s32(cc(",err"), error);
        w.u32(cc(",inf"), sync ? 0 : 1);
        sendToGuest(c, w, buffer);
        break;
    }
    case cc("dfin"):
        if (s && s->workerRunning) {
            IOLockLock(s->lock);
            s->finishWanted = true;
            s->finishRequest = h.request;
            clock_interval_to_deadline(FinishMs, kMillisecondScale, &s->finishDeadline);
            s->pendingAtDeadline = s->pendingFrames;
            IOLockUnlock(s->lock);
            IOLockWakeup(s->lock, &s->events, false);
        } else {
            replyStatus(c, h, 0);
        }
        break;
    case cc("dinv"):
        if (s) logEnd(s, "dinv");
        if (s) teardown(s);
        if (s) answerAllFrames(s, -12903);   // kVTInvalidSessionErr: never coming (not at startSession's teardown: a resume's)
        replyStatus(c, h, 0);
        break;
    case cc("-dec"):
        if (s) {
            logEnd(s, "-dec");
            teardown(s);
            answerAllFrames(s, -12903);
            IOLockLock(c->listLock);
            for (Session **at = &c->sessions; *at; at = &(*at)->next)
                if (*at == s) { *at = s->next; break; }
            IOLockUnlock(c->listLock);
            IOLockFree(s->lock);
            IOLockFree(s->surfaceLock);
            forgetStartMessage(s);
            delete s;
        }
        break;
    default:
        vt_host_unknown = vt_host_unknown + 1;
        vt_host_last_unknown = h.type;
        if (h.flags & FlagRequest) replyStatus(c, h, -12900);
        break;
    }
    IOLockUnlock(c->lock);
    return 0;
}

}  // extern "C"
