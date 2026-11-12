/* SPDX-License-Identifier: GPL-2.0-only */
/* The iris decoder as a stateful V4L2 decoder device (IrisMedia.h). One table of sessions under the device's lock
 * (QcomIrisDeviceLock, the core's), which every session call needs. The firmware's callbacks arrive with that lock held, so they
 * only note what happened in a ring; the event thread hands the ring's entries to the sink with no lock held, which lets the sink
 * take the client's own locks and the client call back in. */
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include <string.h>
#include "QcomIrisDevice.h"
#include "IrisMedia.h"
extern "C" {
#include "QcomIrisBuffers.h"
#include "QcomIrisSession.h"
}

/* For the boot log: [0] sessions opened, [1] closed, [2] source changes, [3] streams the session cannot give, [4] sessions the
 * firmware failed, [5] its last error, [6] notes dropped at a full ring, [7] the last stream's bit depth word, [8] its coded
 * width << 16 | height, [9] bitstream buffers queued, [10] picture buffers queued, [11] pictures returned with data, [12] buffers
 * returned with the error flag, [13] the last ioctl that failed other than the display delay control (code << 16 | errno). */
extern "C" { extern volatile uint32_t iris_media_stats[14]; }
volatile uint32_t iris_media_stats[14];

namespace {
enum : uint32_t {
    IocGFmt = 4, IocSFmt = 5, IocReqbufs = 8, IocQuerybuf = 9, IocQbuf = 15, IocStreamon = 18, IocStreamoff = 19, IocGCtrl = 27,
    IocSCtrl = 28, IocEnumFramesizes = 74, IocSubscribeEvent = 90, IocDecoderCmd = 96,
    TypeCaptureMplane = 9, TypeOutputMplane = 10, MemoryUserPtr = 2,
    EventEos = 2, EventSourceChange = 5,
    BufFlagKeyframe = 0x8, BufFlagError = 0x40, BufFlagLast = 0x00100000,
    CidMinBuffersForCapture = 0x00980927,
    DecCmdStart = 0, DecCmdStop = 1,
    EvtError = 0, EvtDqbuf = 1, EvtEvent = 2,
    FourccH264 = 0x34363248, FourccHevc = 0x43564548, FourccVp9 = 0x30395056, FourccNv12 = 0x3231564e, FourccP010 = 0x30313050,
    FrmsizeStepwise = 3,
};
/* Linux errno values, as the ioctls return them. */
enum : int { Enoent = 2, Eio = 5, Enomem = 12, Ebusy = 16, Enodev = 19, Einval = 22, Enotty = 25 };

struct V4l2Buffer {   /* struct v4l2_buffer, then one struct v4l2_plane */
    uint32_t index, type, bytesused, flags, field, pad0;
    int64_t sec, usec;
    uint8_t timecode[16];
    uint32_t sequence, memory;
    uint64_t m;
    uint32_t length, reserved2, request_fd, pad1;
    struct { uint32_t bytesused, length; uint64_t m; uint32_t data_offset, reserved[11]; } plane;
};
static_assert(sizeof(V4l2Buffer) == 88 + 64, "v4l2_buffer + v4l2_plane");
struct V4l2Format {   /* struct v4l2_format with v4l2_pix_format_mplane */
    uint32_t type, pad;
    uint32_t width, height, pixelformat, field, colorspace;
    struct { uint32_t sizeimage, bytesperline; uint16_t reserved[6]; } planes[8];
    uint8_t num_planes, flags, ycbcr, quantization, xfer, reserved[7];
    uint8_t rest[200 - 192];
};
static_assert(sizeof(V4l2Format) == 208, "v4l2_format");

constexpr unsigned MaxSessions = 8, MaxInputs = 8, MaxCaptures = 32, RingSize = 256;
constexpr uint32_t MinWidth = 96, MinHeight = 96, MaxWidth = 8192, MaxHeight = 4352;   /* platform_inst_cap_sm8550 */

struct Slot {
    QcomIrisBuffer map;          /* the VPU's mapping of the client's memory */
    IOMemoryDescriptor *memory;  /* what is mapped (retained by the mapping) */
    uint64_t user;               /* the client's plane address, echoed when the buffer comes back */
    uint32_t length;
    bool mapped, queued;
};
struct Media {
    bool used, added, opened, failed;
    uint32_t id;
    struct qcom_iris_session iris;
    struct qcom_iris_session_ops ops;
    enum qcom_iris_codec codec;
    bool codecSet;
    uint32_t width, height, inSize;
    bool inputOn, outputOn, haveInfo, draining, sourceChange, eos;
    struct qcom_iris_stream_info info;
    uint32_t inCount, outCount;
    Slot in[MaxInputs], out[MaxCaptures];
};
struct Note {   /* what the event thread hands to the sink */
    uint32_t kind, session;
    V4l2Buffer buffer;
    uint32_t event;
};

Media gTable[MaxSessions];
uint32_t gNextId = 1;
Note gRing[RingSize];
uint32_t gHead, gTail;          /* under the device's lock */
bool gThreadStarted;
void (*gSink)(void *, const uint8_t *, uint32_t);
void *gSinkContext;

IOLock *lock() { return QcomIrisDeviceLock(); }
Media *find(uint32_t id) {
    for (Media &m : gTable) if (m.used && m.id == id) return &m;
    return nullptr;
}
Media *ofIris(struct qcom_iris_session *is) {
    return reinterpret_cast<Media *>(reinterpret_cast<char *>(is) - __builtin_offsetof(Media, iris));
}
uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

/* The VPU reads and writes memory past the CPU's caches: the lines over the first `length` bytes of a buffer are written back and
 * dropped before the VPU reads a bitstream the CPU wrote, and when memory is first given for pictures (so nothing of the CPU's is
 * written back over them later). A client that reads a picture drops its own lines over it first. Through the memory descriptor,
 * whose pages need not be mapped in the kernel (a client's surface). */
void cacheSync(IOMemoryDescriptor *memory, uint64_t length) {
    if (memory && length) memory->performOperation(kIOMemoryIncoherentIOFlush, 0, length);
}

/* The lock held: a note for the event thread. A full ring drops the note and fails the session, which the client then closes. */
void note(const Note &n) {
    if (gHead - gTail >= RingSize) {
        Media *m = find(n.session);
        if (m) m->failed = true;
        iris_media_stats[6] = iris_media_stats[6] + 1;
        return;
    }
    gRing[gHead++ % RingSize] = n;
    IOLockWakeup(lock(), &gRing, true);
}
void noteEvent(Media *m, uint32_t kind, uint32_t event) {
    Note n = {};
    n.kind = kind; n.session = m->id; n.event = event;
    note(n);
}
void noteBuffer(Media *m, uint32_t type, uint32_t index, const Slot &slot, uint32_t bytesused, uint32_t flags, uint64_t timestamp) {
    Note n = {};
    n.kind = EvtDqbuf; n.session = m->id;
    V4l2Buffer &b = n.buffer;
    b.index = index; b.type = type; b.flags = flags; b.memory = MemoryUserPtr; b.length = 1;
    b.sec = int64_t(timestamp);
    b.plane.bytesused = bytesused; b.plane.length = slot.length; b.plane.m = slot.user;
    note(n);
}

void eventThread(void *, wait_result_t) {
    IOLock *l = lock();
    IOLockLock(l);
    for (;;) {
        while (gHead == gTail) IOLockSleep(l, &gRing, THREAD_UNINT);
        const Note n = gRing[gTail++ % RingSize];
        void (*sink)(void *, const uint8_t *, uint32_t) = gSink;
        void *context = gSinkContext;
        IOLockUnlock(l);
        if (sink) {
            uint8_t bytes[8 + sizeof(V4l2Buffer)];
            memcpy(bytes, &n.kind, 4);
            memcpy(bytes + 4, &n.session, 4);
            uint32_t size = 8;
            if (n.kind == EvtDqbuf) {
                memcpy(bytes + 8, &n.buffer, sizeof n.buffer);
                size += sizeof n.buffer;
            }
            else if (n.kind == EvtEvent) { memcpy(bytes + 8, &n.event, 4); size += 4; }
            sink(context, bytes, size);
        }
        IOLockLock(l);
    }
}

/* ---- the firmware's callbacks (the lock held, the interrupt thread) ---- */
void inputDone(void *, struct qcom_iris_session *is, const struct qcom_iris_frame *f) {
    Media *m = ofIris(is);
    if (f->index >= m->inCount) return;
    Slot &slot = m->in[f->index];
    slot.queued = false;
    noteBuffer(m, TypeOutputMplane, f->index, slot, 0, f->error ? uint32_t(BufFlagError) : 0, f->timestamp);
}
void outputDone(void *, struct qcom_iris_session *is, const struct qcom_iris_frame *f) {
    Media *m = ofIris(is);
    if (f->index >= m->outCount) return;
    Slot &slot = m->out[f->index];
    slot.queued = false;
    uint32_t flags = 0;
    if (f->error) flags |= BufFlagError;
    if (f->keyframe) flags |= BufFlagKeyframe;
    if (f->last) flags |= BufFlagLast;
    if (f->data_size) iris_media_stats[11] = iris_media_stats[11] + 1;
    if (f->error) iris_media_stats[12] = iris_media_stats[12] + 1;
    if (f->last && m->draining) {
        m->draining = false;
        m->eos = true;
    }
    noteBuffer(m, TypeCaptureMplane, f->index, slot, f->data_size, flags, f->timestamp);
    if (f->last && m->eos) { m->eos = false; noteEvent(m, EvtEvent, EventEos); }
}
void settingsChange(void *, struct qcom_iris_session *is, const struct qcom_iris_stream_info *info) {
    Media *m = ofIris(is);
    m->info = *info;
    iris_media_stats[2] = iris_media_stats[2] + 1;
    iris_media_stats[7] = info->bit_depth;
    iris_media_stats[8] = info->coded_width << 16 | (info->coded_height & 0xffff);
    if (!info->supported) {
        iris_media_stats[3] = iris_media_stats[3] + 1;   /* not 8 or 10 bits progressive: the client decodes this stream another way */
        m->failed = true;
        noteEvent(m, EvtError, 0);
        return;
    }
    m->haveInfo = true;
    noteEvent(m, EvtEvent, EventSourceChange);
}
void sessionFailed(void *, struct qcom_iris_session *is, uint32_t error) {
    Media *m = ofIris(is);
    iris_media_stats[4] = iris_media_stats[4] + 1;
    iris_media_stats[5] = error;
    m->failed = true;
    noteEvent(m, EvtError, 0);
}

/* ---- the session ops' plumbing ---- */
int opSend(void *, const void *message, uint32_t size) { return QcomIrisDeviceSend(message, size); }
int opAllocate(void *, uint32_t bytes, uint64_t *address, void **handle) { return QcomIrisDeviceAllocateInternal(bytes, address, handle); }
void opRelease(void *, void *handle) { QcomIrisDeviceReleaseInternal(handle); }
bool opWait(void *, struct qcom_iris_session *is, unsigned ms) { return QcomIrisDeviceWait(is, ms) || is->responded; }
void opSignal(void *, struct qcom_iris_session *is) { QcomIrisDeviceSignal(is); }

void unmapSlot(Slot &slot) {
    if (slot.mapped) QcomIrisDeviceRelease(&slot.map);
    slot = Slot();
}
void unmapQueue(Media *m, bool input) {
    Slot *slots = input ? m->in : m->out;
    const unsigned count = input ? MaxInputs : MaxCaptures;
    for (unsigned i = 0; i < count; ++i) unmapSlot(slots[i]);
}

/* iris_open: the core initialized (powered for it, then left to autosuspend), then the session opened for the codec. */
int openIris(Media *m) {
    struct qcom_iris_core *core = QcomIrisDeviceCore();
    if (!core) return Enodev;
    int r = qcom_iris_core_get(core);
    if (r) return Eio;
    if (core->state != QcomIrisCoreInit) r = qcom_iris_core_init(core);
    qcom_iris_core_put(core);
    if (r) return Eio;
    m->ops.context = m;
    m->ops.send = opSend; m->ops.allocate = opAllocate; m->ops.release = opRelease; m->ops.wait = opWait; m->ops.signal = opSignal;
    m->ops.input_done = inputDone; m->ops.output_done = outputDone; m->ops.settings_change = settingsChange; m->ops.failed = sessionFailed;
    qcom_iris_session_init(&m->iris, &m->ops, &core->header_id, &core->packet_id, qcom_iris_core_session_id(core), false, m->codec,
                           core->num_vpp_pipe);
    if (qcom_iris_core_add_session(core, &m->iris)) return Ebusy;
    m->added = true;
    if (qcom_iris_session_open(&m->iris)) return Eio;
    m->opened = true;
    return 0;
}
void closeIris(Media *m) {
    struct qcom_iris_core *core = QcomIrisDeviceCore();
    if (m->opened) {
        if (m->iris.state == QcomIrisStreaming || m->iris.state == QcomIrisOutputStreaming) qcom_iris_session_stop(&m->iris, false);
        if (m->iris.state == QcomIrisStreaming || m->iris.state == QcomIrisInputStreaming) qcom_iris_session_stop(&m->iris, true);
        qcom_iris_session_close(&m->iris);
        m->opened = false;
    }
    if (m->added && core) { qcom_iris_core_remove_session(core, &m->iris); m->added = false; }
}

/* The firmware's bit depth word: luma << 16 | chroma. */
bool tenBit(const Media *m) { return (m->info.bit_depth & 0xffff) == 10; }

uint32_t outputBytes(const Media *m) {
    const struct qcom_iris_buffer_query q = {.encoder = false, .codec = m->codec, .src_width = m->info.coded_width,
                                             .src_height = m->info.coded_height, .dst_width = m->info.output_width,
                                             .dst_height = m->info.output_height, .num_vpp_pipe = m->iris.num_vpp_pipe,
                                             .ten_bit = tenBit(m)};
    const uint32_t bytes = qcom_iris_buffer_size(&q, QcomIrisBufOutput);
    return bytes > m->info.output_size ? bytes : m->info.output_size;
}

/* ---- the ioctls (the lock held) ---- */
int setFormat(Media *m, V4l2Format *f) {
    if (f->type != TypeOutputMplane) return Einval;   /* the CAPTURE format is the firmware's (NV12 at its alignment) */
    if (m->inputOn || m->opened) return Ebusy;
    if (f->pixelformat == FourccH264) m->codec = QcomIrisCodecH264;
    else if (f->pixelformat == FourccHevc) m->codec = QcomIrisCodecHevc;
    else if (f->pixelformat == FourccVp9) m->codec = QcomIrisCodecVp9;
    else return Einval;
    if (f->width < MinWidth || f->height < MinHeight || f->width > MaxWidth || f->height > MaxHeight) return Einval;
    m->codecSet = true;
    m->width = f->width;
    m->height = f->height;
    int r = openIris(m);
    if (!r && qcom_iris_decoder_set_format(&m->iris, m->width, m->height)) r = Einval;
    if (r) { closeIris(m); m->codecSet = false; return r; }
    const struct qcom_iris_buffer_query q = {.encoder = false, .codec = m->codec, .src_width = m->width, .src_height = m->height,
                                             .dst_width = alignUp(m->width, 128), .dst_height = alignUp(m->height, 32),
                                             .num_vpp_pipe = m->iris.num_vpp_pipe};
    m->inSize = alignUp(qcom_iris_buffer_size(&q, QcomIrisBufInput), 4096);
    f->num_planes = 1;
    f->planes[0].sizeimage = m->inSize;
    f->planes[0].bytesperline = 0;
    return 0;
}
int getFormat(Media *m, V4l2Format *f) {
    const uint32_t type = f->type;
    memset(f, 0, sizeof *f);
    f->type = type;
    f->num_planes = 1;
    if (type == TypeOutputMplane) {
        if (!m->codecSet) return Einval;
        f->width = m->width; f->height = m->height;
        f->pixelformat = m->codec == QcomIrisCodecH264 ? FourccH264 : m->codec == QcomIrisCodecHevc ? FourccHevc : FourccVp9;
        f->planes[0].sizeimage = m->inSize;
        return 0;
    }
    if (type != TypeCaptureMplane || !m->haveInfo) return Einval;
    f->width = m->info.output_width;      /* the stride: the width aligned to 128 */
    f->height = m->info.output_height;    /* the scanlines: the height aligned to 32, the chroma plane after them */
    f->pixelformat = tenBit(m) ? FourccP010 : FourccNv12;
    f->planes[0].bytesperline = tenBit(m) ? alignUp(m->info.output_width * 2, 256) : m->info.output_width;
    f->planes[0].sizeimage = outputBytes(m);
    return 0;
}
int requestBuffers(Media *m, uint32_t *request) {   /* struct v4l2_requestbuffers: count, type, memory, capabilities, flags */
    const bool input = request[1] == TypeOutputMplane;
    if (!input && request[1] != TypeCaptureMplane) return Einval;
    if (request[2] != MemoryUserPtr) return Einval;
    if (input ? m->inputOn : m->outputOn) return Ebusy;
    unmapQueue(m, input);
    uint32_t count = request[0];
    if (input) {
        if (count > MaxInputs) count = MaxInputs;
        m->inCount = count;
    } else {
        if (count && !m->haveInfo) return Einval;
        if (count && count < m->info.output_min_count) count = m->info.output_min_count;
        if (count > MaxCaptures) count = MaxCaptures;
        m->outCount = count;
    }
    request[0] = count;
    return 0;
}
int queryBuffer(Media *m, V4l2Buffer *b) {
    const bool input = b->type == TypeOutputMplane;
    if (!input && b->type != TypeCaptureMplane) return Einval;
    if (b->index >= (input ? m->inCount : m->outCount)) return Einval;
    b->memory = MemoryUserPtr;
    b->length = 1;
    b->plane.length = input ? m->inSize : outputBytes(m);
    return 0;
}
int streamOn(Media *m, uint32_t type) {
    if (type == TypeOutputMplane) {
        if (!m->opened) return Einval;
        if (m->inputOn) return 0;
        if (const int e = qcom_iris_decoder_start_input(&m->iris)) return e < 0 ? -e : Eio;   /* the session's reason, for the stats row */
        m->inputOn = true;
        return 0;
    }
    if (type != TypeCaptureMplane || !m->haveInfo) return Einval;
    if (m->outputOn) return 0;
    if (const int e = qcom_iris_decoder_start_output(&m->iris, m->info.output_width, m->info.output_height)) return e < 0 ? -e : Eio;
    m->outputOn = true;
    /* iris_vb2_start_streaming: the buffers queued before the port streamed go to the firmware now. */
    for (unsigned i = 0; i < m->outCount; ++i) {
        Slot &slot = m->out[i];
        if (!slot.mapped || !slot.queued) continue;
        if (qcom_iris_session_queue_output(&m->iris, i, slot.map.iova + (slot.map.offset & 4095), slot.length)) { slot.queued = false; return Eio; }
    }
    return 0;
}
int streamOff(Media *m, uint32_t type) {
    const bool input = type == TypeOutputMplane;
    if (!input && type != TypeCaptureMplane) return Einval;
    bool &on = input ? m->inputOn : m->outputOn;
    if (on && m->opened && qcom_iris_session_stop(&m->iris, input)) m->failed = true;
    on = false;
    Slot *slots = input ? m->in : m->out;
    const unsigned count = input ? m->inCount : m->outCount;
    for (unsigned i = 0; i < count; ++i) slots[i].queued = false;   /* the firmware gave every buffer of the port back */
    if (!input) m->draining = false;
    return 0;
}
int decoderCommand(Media *m, const uint32_t *cmd) {
    if (!m->opened) return Einval;
    if (cmd[0] == DecCmdStop) {
        if (!m->inputOn || !m->outputOn) return Einval;   /* nothing decoded yet: the client ends the drain itself */
        if (m->draining) return 0;
        if (qcom_iris_decoder_drain(&m->iris)) return Eio;
        m->draining = true;
        return 0;
    }
    if (cmd[0] == DecCmdStart) {
        if (qcom_iris_decoder_resume(&m->iris)) return Eio;
        /* iris_vdec_start_cmd: the output buffers the client holds queued go back to the firmware. */
        for (unsigned i = 0; i < m->outCount; ++i) {
            Slot &slot = m->out[i];
            if (slot.mapped && slot.queued) qcom_iris_session_queue_output(&m->iris, i, slot.map.iova + (slot.map.offset & 4095), slot.length);
        }
        return 0;
    }
    return Einval;
}
int getControl(Media *m, uint32_t *control) {   /* struct v4l2_control: id, value */
    if (control[0] != CidMinBuffersForCapture || !m->haveInfo) return Einval;
    control[1] = m->info.output_min_count;
    return 0;
}
int enumFramesizes(uint32_t *sizes) {   /* struct v4l2_frmsizeenum: index, pixel_format, type, stepwise (6), reserved */
    if (sizes[0]) return Einval;
    if (sizes[1] != FourccH264 && sizes[1] != FourccHevc && sizes[1] != FourccVp9) return Einval;
    sizes[2] = FrmsizeStepwise;
    sizes[3] = MinWidth; sizes[4] = MaxWidth; sizes[5] = 1;
    sizes[6] = MinHeight; sizes[7] = MaxHeight; sizes[8] = 1;
    return 0;
}
}   // namespace

unsigned iris_media_devices(void) { return IrisMediaDevices; }
bool iris_media_is_up(unsigned device) { return device == IrisMediaDecoder && QcomIrisDeviceReady(); }
const char *iris_media_card(unsigned device) { return device == IrisMediaDecoder ? "iris decoder" : ""; }

void iris_media_set_sink(unsigned device, void (*sink)(void *, const uint8_t *, uint32_t), void *context) {
    if (device != IrisMediaDecoder || !QcomIrisDeviceReady()) return;
    IOLockLock(lock());
    gSink = sink;
    gSinkContext = context;
    const bool start = !gThreadStarted;
    gThreadStarted = true;
    IOLockUnlock(lock());
    if (start) {
        thread_t thread;
        if (kernel_thread_start(eventThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    }
}

int iris_media_open(unsigned device, uint32_t *session) {
    if (!iris_media_is_up(device) || !session) return Enodev;
    IOLockLock(lock());
    Media *m = nullptr;
    for (Media &candidate : gTable) if (!candidate.used) { m = &candidate; break; }
    if (m) {
        memset(m, 0, sizeof *m);
        m->used = true;
        m->id = gNextId++;
        if (!gNextId) gNextId = 1;
        *session = m->id;
        iris_media_stats[0] = iris_media_stats[0] + 1;
    }
    IOLockUnlock(lock());
    return m ? 0 : Ebusy;
}

int iris_media_close(unsigned device, uint32_t session) {
    if (device != IrisMediaDecoder) return Enodev;
    IOLockLock(lock());
    Media *m = find(session);
    if (m) {
        closeIris(m);
        unmapQueue(m, true);
        unmapQueue(m, false);
        m->used = false;
        iris_media_stats[1] = iris_media_stats[1] + 1;
    }
    IOLockUnlock(lock());
    return m ? 0 : Enoent;
}

int iris_media_ioctl(unsigned device, uint32_t session, uint32_t code, void *payload, uint32_t out, uint32_t in) {
    if (device != IrisMediaDecoder || !payload) return Enodev;
    const uint32_t size = out > in ? out : in;
    IOLockLock(lock());
    Media *m = find(session);
    int r = Enotty;
    if (!m) r = Enoent;
    else if (m->failed && code != IocStreamoff) r = Eio;
    else switch (code) {
    case IocSFmt: r = size >= sizeof(V4l2Format) ? setFormat(m, static_cast<V4l2Format *>(payload)) : Einval; break;
    case IocGFmt: r = size >= sizeof(V4l2Format) ? getFormat(m, static_cast<V4l2Format *>(payload)) : Einval; break;
    case IocReqbufs: r = size >= 20 ? requestBuffers(m, static_cast<uint32_t *>(payload)) : Einval; break;
    case IocQuerybuf: r = size >= sizeof(V4l2Buffer) ? queryBuffer(m, static_cast<V4l2Buffer *>(payload)) : Einval; break;
    case IocStreamon: r = size >= 4 ? streamOn(m, *static_cast<uint32_t *>(payload)) : Einval; break;
    case IocStreamoff: r = size >= 4 ? streamOff(m, *static_cast<uint32_t *>(payload)) : Einval; break;
    case IocGCtrl: r = size >= 8 ? getControl(m, static_cast<uint32_t *>(payload)) : Einval; break;
    case IocSCtrl: r = Einval; break;   /* no display delay control: the firmware gives pictures in display order */
    case IocEnumFramesizes: r = size >= 44 ? enumFramesizes(static_cast<uint32_t *>(payload)) : Einval; break;
    case IocSubscribeEvent: r = 0; break;   /* source changes and the end of a drain are always reported */
    case IocDecoderCmd: r = size >= 4 ? decoderCommand(m, static_cast<uint32_t *>(payload)) : Einval; break;
    default: break;
    }
    if (r && code != IocSCtrl) iris_media_stats[13] = code << 16 | uint32_t(r);   /* the display delay control is always refused */
    IOLockUnlock(lock());
    return r;
}

int iris_media_qbuf(unsigned device, uint32_t session, void *v4l2_buffer, IOMemoryDescriptor *memory) {
    if (device != IrisMediaDecoder || !v4l2_buffer || !memory) return Enodev;
    V4l2Buffer *b = static_cast<V4l2Buffer *>(v4l2_buffer);
    IOLockLock(lock());
    Media *m = find(session);
    int r = 0;
    if (!m) r = Enoent;
    else if (m->failed) r = Eio;
    else {
        const bool input = b->type == TypeOutputMplane;
        const unsigned count = input ? m->inCount : b->type == TypeCaptureMplane ? m->outCount : 0;
        const uint32_t need = input ? m->inSize : m->haveInfo ? outputBytes(m) : 0;
        if (b->index >= count || !need || b->plane.length < need || memory->getLength() < need) r = Einval;
        else {
            Slot &slot = (input ? m->in : m->out)[b->index];
            if (slot.queued) r = Ebusy;
            else {
                if (slot.mapped && slot.memory != memory) unmapSlot(slot);
                if (!slot.mapped) {
                    if (QcomIrisDeviceMap(memory, 0, need, &slot.map)) r = Enomem;
                    else {
                        slot.mapped = true;
                        slot.memory = memory;
                        /* newly given memory: nothing of the CPU's may be written back over what the VPU writes later */
                        if (!input) cacheSync(memory, need);
                    }
                }
                if (!r) {
                    slot.user = b->plane.m;
                    slot.length = need;
                    const uint64_t address = slot.map.iova + (slot.map.offset & 4095);
                    if (input) {
                        if (!m->inputOn || b->plane.bytesused > need) r = Einval;
                        else if ((cacheSync(memory, b->plane.bytesused), false)) r = Einval;
                        else if (qcom_iris_session_queue_input(&m->iris, b->index, address, need, 0, b->plane.bytesused, uint64_t(b->sec), 0)) r = Eio;
                        else { slot.queued = true; iris_media_stats[9] = iris_media_stats[9] + 1; }
                    } else if (!m->outputOn || (m->iris.sub_state & QcomIrisSubOutputPause)) {
                        slot.queued = true;   /* held until the port streams or the decoder resumes */
                    } else if (qcom_iris_session_queue_output(&m->iris, b->index, address, need)) r = Eio;
                    else { slot.queued = true; iris_media_stats[10] = iris_media_stats[10] + 1; }
                }
            }
        }
    }
    IOLockUnlock(lock());
    return r;
}
