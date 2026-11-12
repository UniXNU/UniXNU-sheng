/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of the iris decoder as a V4L2 decoder device (kernel/IrisMedia.cpp), with the device and the session stood in for:
 * the calls a decoder client makes, in its order, against a session that records what it is asked and reports frames through the
 * session's callbacks. Built by tools/check.sh with tests/kernel-stub in place of the kernel's headers. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kern/thread.h>
#include "QcomIrisDevice.h"
#include "IrisMedia.h"
extern "C" {
#include "QcomIrisBuffers.h"
#include "QcomIrisSession.h"
}

#define CHECK(x) do { if (!(x)) { printf("media: FAILED %s:%d %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

/* ---- the kernel ---- */
static jmp_buf idle;
static void (*threadMain)(void *, wait_result_t);
int IOLockSleep(IOLock *, void *, int) { longjmp(idle, 1); }
int kernel_thread_start(void (*main)(void *, wait_result_t), void *, thread_t *thread) { threadMain = main; *thread = nullptr; return 0; }

/* ---- the device ---- */
static IOLock deviceLock;
static struct qcom_iris_core core;
static unsigned maps, unmaps;
bool QcomIrisDeviceReady(void) { return true; }
IOLock *QcomIrisDeviceLock(void) { return &deviceLock; }
struct qcom_iris_core *QcomIrisDeviceCore(void) { return &core; }
int QcomIrisDeviceMap(IOMemoryDescriptor *memory, uint64_t offset, uint64_t length, QcomIrisBuffer *b) {
    b->memory = memory; b->offset = offset; b->bytes = length; b->iova = 0x10000000u + 0x1000000u * ++maps;
    return 0;
}
void QcomIrisDeviceRelease(QcomIrisBuffer *) { ++unmaps; }
int QcomIrisDeviceSend(const void *, uint32_t) { return 0; }
bool QcomIrisDeviceWait(struct qcom_iris_session *, unsigned) { return true; }
void QcomIrisDeviceSignal(struct qcom_iris_session *) {}
int QcomIrisDeviceAllocateInternal(uint32_t, uint64_t *, void **) { return -1; }
void QcomIrisDeviceReleaseInternal(void *) {}

/* ---- the core and the session ---- */
static struct qcom_iris_session *session;
static const struct qcom_iris_session_ops *ops;
static struct { unsigned open, close, format, startIn, startOut, stopIn, stopOut, drain, resume, queuedIn, queuedOut; } asked;
static struct { uint32_t index; uint64_t address; uint32_t size, data; uint64_t timestamp; } lastIn, lastOut;
extern "C" {
int qcom_iris_core_get(struct qcom_iris_core *) { return 0; }
void qcom_iris_core_put(struct qcom_iris_core *) {}
int qcom_iris_core_init(struct qcom_iris_core *c) { c->state = QcomIrisCoreInit; return 0; }
uint32_t qcom_iris_core_session_id(struct qcom_iris_core *) { return 7; }
int qcom_iris_core_add_session(struct qcom_iris_core *, struct qcom_iris_session *) { return 0; }
void qcom_iris_core_remove_session(struct qcom_iris_core *, struct qcom_iris_session *) {}
void qcom_iris_session_init(struct qcom_iris_session *s, const struct qcom_iris_session_ops *o, uint32_t *, uint32_t *, uint32_t id, bool,
                            enum qcom_iris_codec codec, uint32_t pipes) {
    memset(s, 0, sizeof *s);
    s->ops = o; s->session_id = id; s->codec = codec; s->num_vpp_pipe = pipes; s->state = QcomIrisDeinit;
    session = s; ops = o;
}
int qcom_iris_session_open(struct qcom_iris_session *s) { ++asked.open; s->state = QcomIrisInit; return 0; }
int qcom_iris_session_close(struct qcom_iris_session *s) { ++asked.close; s->state = QcomIrisDeinit; return 0; }
int qcom_iris_decoder_set_format(struct qcom_iris_session *, uint32_t, uint32_t) { ++asked.format; return 0; }
static int64_t outputOrder = -1;
int qcom_iris_session_set_cap(struct qcom_iris_session *, enum qcom_iris_cap id, int64_t value) {
    if (id != QcomIrisCapOutputOrder) return -22;
    outputOrder = value;
    return 0;
}
int qcom_iris_decoder_start_input(struct qcom_iris_session *s) { ++asked.startIn; s->state = QcomIrisInputStreaming; return 0; }
int qcom_iris_decoder_start_output(struct qcom_iris_session *s, uint32_t, uint32_t) { ++asked.startOut; s->state = QcomIrisStreaming; return 0; }
int qcom_iris_decoder_drain(struct qcom_iris_session *) { ++asked.drain; return 0; }
int qcom_iris_decoder_resume(struct qcom_iris_session *s) { ++asked.resume; s->sub_state &= ~QcomIrisSubOutputPause; return 0; }
int qcom_iris_session_queue_input(struct qcom_iris_session *, uint32_t index, uint64_t address, uint32_t size, uint32_t, uint32_t data,
                                  uint64_t timestamp, uint32_t) {
    ++asked.queuedIn; lastIn = {index, address, size, data, timestamp};
    return 0;
}
int qcom_iris_session_queue_output(struct qcom_iris_session *, uint32_t index, uint64_t address, uint32_t size) {
    ++asked.queuedOut; lastOut = {index, address, size, 0, 0};
    return 0;
}
int qcom_iris_session_stop(struct qcom_iris_session *s, bool input) {
    if (input) { ++asked.stopIn; s->state = s->state == QcomIrisStreaming ? QcomIrisOutputStreaming : QcomIrisInit; }
    else { ++asked.stopOut; s->state = s->state == QcomIrisStreaming ? QcomIrisInputStreaming : QcomIrisInit; }
    return 0;
}
uint32_t qcom_iris_buffer_size(const struct qcom_iris_buffer_query *q, enum qcom_iris_buffer type) {
    return type == QcomIrisBufInput ? 1000000 : q->dst_width * q->dst_height * 3 / 2;
}
}

/* ---- the client ---- */
enum : uint32_t { IocGFmt = 4, IocSFmt = 5, IocReqbufs = 8, IocQuerybuf = 9, IocStreamon = 18, IocStreamoff = 19, IocGCtrl = 27, IocSCtrl = 28,
                  IocEnumFramesizes = 74, IocSubscribeEvent = 90, IocDecoderCmd = 96, Capture = 9, Output = 10 };
struct V4l2Buffer {
    uint32_t index, type, bytesused, flags, field, pad0;
    int64_t sec, usec;
    uint8_t timecode[16];
    uint32_t sequence, memory;
    uint64_t m;
    uint32_t length, reserved2, request_fd, pad1;
    struct { uint32_t bytesused, length; uint64_t m; uint32_t data_offset, reserved[11]; } plane;
};
struct V4l2Format {
    uint32_t type, pad;
    uint32_t width, height, pixelformat, field, colorspace;
    struct { uint32_t sizeimage, bytesperline; uint16_t reserved[6]; } planes[8];
    uint8_t num_planes, flags, ycbcr, quantization, xfer, reserved[7];
    uint8_t rest[8];
};
struct Seen { uint32_t kind, session, event; V4l2Buffer buffer; };
static Seen seen[64];
static unsigned seenCount;
static void sink(void *, const uint8_t *event, uint32_t size) {
    Seen &s = seen[seenCount++ % 64];
    memset(&s, 0, sizeof s);
    memcpy(&s.kind, event, 4);
    memcpy(&s.session, event + 4, 4);
    if (s.kind == 1 && size >= 8 + sizeof(V4l2Buffer)) memcpy(&s.buffer, event + 8, sizeof(V4l2Buffer));
    if (s.kind == 2 && size >= 12) memcpy(&s.event, event + 8, 4);
}
/* The event thread until it has nothing left to hand over. */
static void pump(void) {
    CHECK(threadMain);
    if (!setjmp(idle)) threadMain(nullptr, 0);
    deviceLock.held = 0;   /* the thread slept holding the lock, as IOLockSleep's caller does */
}
static int io(uint32_t id, uint32_t code, void *payload, uint32_t size) { return iris_media_ioctl(IrisMediaDecoder, id, code, payload, size, size); }

static void stream(uint32_t depth, uint32_t fourcc, uint32_t rowBytes) {
    memset(&asked, 0, sizeof asked);
    seenCount = 0;
    uint32_t id = 0;
    CHECK(iris_media_open(IrisMediaDecoder, &id) == 0 && id);
    uint32_t sizes[11] = {0, 0x34363248};
    CHECK(io(id, IocEnumFramesizes, sizes, sizeof sizes) == 0 && sizes[3] == 96 && sizes[4] == 8192);
    uint32_t subscription[8] = {5};
    CHECK(io(id, IocSubscribeEvent, subscription, sizeof subscription) == 0);
    V4l2Format f = {};
    f.type = Output; f.width = 1920; f.height = 1080; f.pixelformat = 0x34363248; f.num_planes = 1; f.planes[0].sizeimage = 2 << 20;
    CHECK(io(id, IocSFmt, &f, sizeof f) == 0 && asked.open == 1 && asked.format == 1 && f.planes[0].sizeimage == 1003520);
    /* The display delay enabled at 0 is decode order; any other delay, or the control off, the firmware's display order. */
    uint32_t control[2] = {0x00990b8e, 1};
    CHECK(io(id, IocSCtrl, control, sizeof control) == 0 && outputOrder == 1);
    control[0] = 0x00990b8d; control[1] = 2;
    CHECK(io(id, IocSCtrl, control, sizeof control) == 0 && outputOrder == 0);
    control[1] = 0;
    CHECK(io(id, IocSCtrl, control, sizeof control) == 0 && outputOrder == 1);
    control[0] = 0x00980900;
    CHECK(io(id, IocSCtrl, control, sizeof control) != 0);
    uint32_t request[5] = {4, Output, 2, 0, 0};
    CHECK(io(id, IocReqbufs, request, sizeof request) == 0 && request[0] == 4);
    V4l2Buffer b = {};
    b.type = Output; b.index = 1;
    CHECK(io(id, IocQuerybuf, &b, sizeof b) == 0 && b.plane.length == 1003520);
    uint32_t type = Output;
    CHECK(io(id, IocStreamon, &type, 4) == 0 && asked.startIn == 1);
    IOMemoryDescriptor input;
    input.length = 1003520;
    b.plane.bytesused = 5000; b.plane.m = 0x1111; b.sec = 42;
    CHECK(iris_media_qbuf(IrisMediaDecoder, id, &b, &input) == 0 && asked.queuedIn == 1 && lastIn.index == 1 && lastIn.data == 5000 &&
          lastIn.timestamp == 42 && input.flushes == 1 && input.flushed == 5000);
    CHECK(iris_media_qbuf(IrisMediaDecoder, id, &b, &input) != 0);   /* still the firmware's */
    /* no CAPTURE format before the firmware reports the stream */
    V4l2Format c = {};
    c.type = Capture;
    CHECK(io(id, IocGFmt, &c, sizeof c) != 0);
    uint32_t stop[18] = {1};
    CHECK(io(id, IocDecoderCmd, stop, sizeof stop) != 0 && asked.drain == 0);   /* nothing decoded yet */

    /* the firmware: the stream's format, then the input buffer back */
    struct qcom_iris_stream_info info = {};
    info.coded_width = 1920; info.coded_height = 1080; info.crop_width = 1920; info.crop_height = 1080;
    info.output_width = 1920; info.output_height = 1088; info.output_min_count = 6;
    info.output_size = depth == 10 ? 6266880 : 3133440;
    info.bit_depth = depth << 16 | depth; info.supported = true;
    ops->settings_change(ops->context, session, &info);
    struct qcom_iris_frame done = {};
    done.index = 1; done.timestamp = 42;
    ops->input_done(ops->context, session, &done);
    pump();
    CHECK(seenCount == 2 && seen[0].kind == 2 && seen[0].event == 5 && seen[0].session == id);
    CHECK(seen[1].kind == 1 && seen[1].buffer.type == Output && seen[1].buffer.index == 1 && seen[1].buffer.plane.m == 0x1111);
    CHECK(iris_media_qbuf(IrisMediaDecoder, id, &b, &input) == 0 && asked.queuedIn == 2 && maps == unmaps + 1);   /* mapped once */

    /* the client sets the CAPTURE queue up */
    CHECK(io(id, IocGFmt, &c, sizeof c) == 0 && c.width == 1920 && c.height == 1088 && c.pixelformat == fourcc &&
          c.planes[0].bytesperline == rowBytes && c.planes[0].sizeimage == info.output_size);
    uint32_t minimum[2] = {0x00980927, 0};
    CHECK(io(id, IocGCtrl, minimum, sizeof minimum) == 0 && minimum[1] == 6);
    uint32_t captures[5] = {8, Capture, 2, 0, 0};
    CHECK(io(id, IocReqbufs, captures, sizeof captures) == 0 && captures[0] == 8);
    IOMemoryDescriptor picture[2];
    for (unsigned i = 0; i < 2; ++i) {
        picture[i].length = info.output_size;
        V4l2Buffer p = {};
        p.type = Capture; p.index = i;
        CHECK(io(id, IocQuerybuf, &p, sizeof p) == 0 && p.plane.length == info.output_size);
        p.plane.m = 0x2000 + i;
        CHECK(iris_media_qbuf(IrisMediaDecoder, id, &p, &picture[i]) == 0);   /* held: the port does not stream yet */
        CHECK(picture[i].flushes == 1 && picture[i].flushed == info.output_size);
    }
    CHECK(asked.queuedOut == 0);
    type = Capture;
    CHECK(io(id, IocStreamon, &type, 4) == 0 && asked.startOut == 1 && asked.queuedOut == 2);

    /* a picture, then a drain whose last buffer ends it */
    struct qcom_iris_frame out = {};
    out.index = 0; out.data_size = info.output_size; out.timestamp = 42; out.keyframe = true;
    seenCount = 0;
    ops->output_done(ops->context, session, &out);
    pump();
    CHECK(seenCount == 1 && seen[0].kind == 1 && seen[0].buffer.type == Capture && seen[0].buffer.index == 0 && seen[0].buffer.sec == 42 &&
          seen[0].buffer.plane.bytesused == info.output_size && seen[0].buffer.plane.m == 0x2000 && (seen[0].buffer.flags & 0x8));
    CHECK(picture[0].flushes == 1);   /* reading the picture is the client's: it drops its own lines first */
    V4l2Buffer again = {};
    again.type = Capture; again.index = 0; again.plane.m = 0x2000; again.plane.length = info.output_size;
    CHECK(iris_media_qbuf(IrisMediaDecoder, id, &again, &picture[0]) == 0 && asked.queuedOut == 3 && picture[0].flushes == 1);
    CHECK(io(id, IocDecoderCmd, stop, sizeof stop) == 0 && asked.drain == 1);
    struct qcom_iris_frame last = {};
    last.index = 1; last.last = true;
    session->sub_state |= QcomIrisSubOutputPause;
    seenCount = 0;
    ops->output_done(ops->context, session, &last);
    pump();
    CHECK(seenCount == 2 && seen[0].kind == 1 && (seen[0].buffer.flags & 0x00100000) && seen[0].buffer.plane.bytesused == 0 &&
          seen[1].kind == 2 && seen[1].event == 2);
    again.index = 1; again.plane.m = 0x2001;
    CHECK(iris_media_qbuf(IrisMediaDecoder, id, &again, &picture[1]) == 0 && asked.queuedOut == 3);   /* held while the output pauses */
    uint32_t start[18] = {0};
    CHECK(io(id, IocDecoderCmd, start, sizeof start) == 0 && asked.resume == 1 && asked.queuedOut == 5);   /* both held buffers again */

    /* a failure reaches the client, and the session then answers only the stream's end */
    seenCount = 0;
    ops->failed(ops->context, session, 1);
    pump();
    CHECK(seenCount == 1 && seen[0].kind == 0);
    CHECK(io(id, IocGFmt, &c, sizeof c) != 0);
    type = Output;
    CHECK(io(id, IocStreamoff, &type, 4) == 0 && asked.stopIn == 1);
    const unsigned before = unmaps;
    CHECK(iris_media_close(IrisMediaDecoder, id) == 0 && asked.close == 1 && unmaps == before + 3);
    CHECK(iris_media_close(IrisMediaDecoder, id) != 0);
}

int main(void) {
    CHECK(iris_media_devices() == 1 && iris_media_is_up(IrisMediaDecoder) && !strcmp(iris_media_card(IrisMediaDecoder), "iris decoder"));
    iris_media_set_sink(IrisMediaDecoder, sink, nullptr);
    stream(8, 0x3231564e, 1920);     /* NV12 */
    stream(10, 0x30313050, 3840);    /* P010: two bytes a sample */
    /* a stream the session cannot give: the client hears of the failure */
    uint32_t id = 0;
    CHECK(iris_media_open(IrisMediaDecoder, &id) == 0);
    V4l2Format f = {};
    f.type = Output; f.width = 1920; f.height = 1080; f.pixelformat = 0x43564548; f.num_planes = 1;
    CHECK(io(id, IocSFmt, &f, sizeof f) == 0);
    struct qcom_iris_stream_info info = {};
    info.supported = false;
    seenCount = 0;
    ops->settings_change(ops->context, session, &info);
    pump();
    CHECK(seenCount == 1 && seen[0].kind == 0);
    CHECK(iris_media_close(IrisMediaDecoder, id) == 0);
    f.width = 64;
    CHECK(iris_media_open(IrisMediaDecoder, &id) == 0 && io(id, IocSFmt, &f, sizeof f) != 0 && iris_media_close(IrisMediaDecoder, id) == 0);
    printf("media: decoder device: formats, buffers, queueing, source change, pictures, drain and resume, failure, close; NV12 and P010: ok\n");
    return 0;
}
