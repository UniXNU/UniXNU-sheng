/* SPDX-License-Identifier: GPL-2.0-only */
/* sources: src/IrisSession.c src/IrisHfi.c src/IrisBuffers.c deps/linux/drivers/media/platform/qcom/iris/iris_vpu_buffer.c */
/* The session state machine against a recording firmware: the decoder's open, input start, first settings change, output start,
 * buffers, drain and close; the encoder's start with client capabilities; and the mainline order of the commands. */
#include "QcomIrisSession.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct sent {uint32_t type, port, payload0, flags, size; struct qcom_iris_hfi_buffer buffer;};
static struct sent sent[4096];
static unsigned count, allocated, released;
static struct qcom_iris_frame outputs[64], inputs[64];
static unsigned output_count, input_count;
static struct qcom_iris_stream_info info;
static bool changed;

static void deliver(struct qcom_iris_session *s, uint32_t type, uint32_t port, uint32_t flags, const void *payload, uint32_t size);
static int send(void *c, const void *m, uint32_t n)
{
    (void)c;
    assert(qcom_iris_hfi_valid(m, n));
    for (const struct qcom_iris_hfi_packet *p = qcom_iris_hfi_next(m, NULL); p; p = qcom_iris_hfi_next(m, p)) {
        assert(count < 4096);
        struct sent *e = &sent[count++];
        e->type = p->type; e->port = p->port; e->flags = p->flags; e->size = p->size;
        e->payload0 = p->size > 32 ? p->payload[0] : 0;
        if (p->type == HFI_CMD_BUFFER) memcpy(&e->buffer, p->payload, sizeof e->buffer);
    }
    return 0;
}
static uint64_t next_addr = 0x30000000;
static int allocate(void *c, uint32_t bytes, uint64_t *addr, void **handle)
{
    (void)c;
    *addr = next_addr;
    next_addr += (bytes + 4095) & ~4095u;
    *handle = (void *)(uintptr_t)++allocated;
    return 0;
}
static void release(void *c, void *h) {(void)c; (void)h; ++released;}
/* the firmware answers the command being waited for */
static bool wait(void *c, struct qcom_iris_session *s, unsigned ms)
{
    (void)c; (void)ms;
    const struct sent *last = &sent[count - 1];
    deliver(s, last->type, last->port, HFI_FW_FLAGS_SUCCESS, NULL, 0);
    return s->responded;
}
static void signal(void *c, struct qcom_iris_session *s) {(void)c; (void)s;}
static void input_done(void *c, struct qcom_iris_session *s, const struct qcom_iris_frame *f) {(void)c; (void)s; inputs[input_count++] = *f;}
static void output_done(void *c, struct qcom_iris_session *s, const struct qcom_iris_frame *f) {(void)c; (void)s; outputs[output_count++] = *f;}
static void settings_change(void *c, struct qcom_iris_session *s, const struct qcom_iris_stream_info *i) {(void)c; (void)s; info = *i; changed = true;}
static void failed(void *c, struct qcom_iris_session *s, uint32_t e) {(void)c; (void)s; printf("failed %#x\n", e); assert(0);}
static const struct qcom_iris_session_ops ops = {NULL, send, allocate, release, wait, signal, input_done, output_done, settings_change, failed};

/* a firmware message of one or more packets */
static uint8_t message[4096];
static struct qcom_iris_hfi_writer writer;
static void begin(struct qcom_iris_session *s) {qcom_iris_hfi_begin(&writer, message, sizeof message, s->session_id, 1);}
static void add(uint32_t type, uint32_t port, uint32_t flags, uint32_t info_type, const void *payload, uint32_t size)
{
    qcom_iris_hfi_add(&writer, type, flags, info_type, port, 0, payload, size);
}
static void finish(struct qcom_iris_session *s) {assert(qcom_iris_hfi_valid(message, qcom_iris_hfi_size(&writer))); qcom_iris_session_handle(s, message);}
static void deliver(struct qcom_iris_session *s, uint32_t type, uint32_t port, uint32_t flags, const void *payload, uint32_t size)
{
    begin(s);
    add(type, port, flags, size ? HFI_PAYLOAD_U32 : HFI_PAYLOAD_NONE, payload, size);
    finish(s);
}
static void buffer_done(struct qcom_iris_session *s, uint32_t type, uint32_t port, uint32_t index, uint64_t addr, uint32_t size,
                        uint32_t flags, uint64_t timestamp, uint32_t picture)
{
    struct qcom_iris_hfi_buffer b = {0};
    b.type = type; b.index = index; b.base_address = addr; b.data_size = size; b.flags = flags; b.timestamp = timestamp;
    begin(s);
    if (picture) add(HFI_PROP_PICTURE_TYPE, port, 0, HFI_PAYLOAD_U32, &picture, 4);
    add(HFI_CMD_BUFFER, port, HFI_FW_FLAGS_SUCCESS, HFI_PAYLOAD_STRUCTURE, &b, sizeof b);
    finish(s);
}
static int find(unsigned from, uint32_t type, uint32_t port)
{
    for (unsigned i = from; i < count; ++i) if (sent[i].type == type && (port == ~0u || sent[i].port == port)) return (int)i;
    return -1;
}

static void decoder(void)
{
    static struct qcom_iris_session s;
    uint32_t header_id = 0, packet_id = 0;
    qcom_iris_session_init(&s, &ops, &header_id, &packet_id, 1, false, QcomIrisCodecH264, 4);
    assert(!qcom_iris_session_open(&s) && s.state == QcomIrisInit);
    assert(sent[0].type == HFI_CMD_OPEN && sent[0].payload0 == 1 && sent[1].type == HFI_PROP_CODEC &&
           sent[1].payload0 == HFI_CODEC_DECODE_AVC && sent[2].type == HFI_PROP_DEC_DEFAULT_HEADER && sent[2].payload0 == 0);
    assert(!qcom_iris_decoder_set_format(&s, 1920, 1080));
    const unsigned before = count;
    assert(!qcom_iris_decoder_start_input(&s) && s.state == QcomIrisInputStreaming);
    /* config params first (bitstream resolution aligned to 16), then the capabilities, persist, input internal buffers, START */
    const int res = find(before, HFI_PROP_BITSTREAM_RESOLUTION, HFI_PORT_BITSTREAM);
    assert(res == (int)before && sent[res].payload0 == (1920u << 16 | 1088));
    const int stage = find(before, HFI_PROP_STAGE, HFI_PORT_NONE), pipe = find(before, HFI_PROP_PIPE, HFI_PORT_NONE);
    assert(stage > res && sent[stage].payload0 == 2 && pipe > stage && sent[pipe].payload0 == 4);
    const int persist = find(before, HFI_CMD_BUFFER, ~0u);
    assert(persist > pipe && sent[persist].buffer.type == HFI_BUFFER_PERSIST && sent[persist].port == HFI_PORT_NONE);
    const int comv = find(before, HFI_PROP_COMV_BUFFER_COUNT, HFI_PORT_BITSTREAM);
    assert(comv > persist && sent[comv + 1].type == HFI_CMD_BUFFER && sent[comv + 1].buffer.type == HFI_BUFFER_COMV);
    const int subscribe = find(before, HFI_CMD_SUBSCRIBE_MODE, HFI_PORT_BITSTREAM);
    const int start = find(before, HFI_CMD_START, HFI_PORT_BITSTREAM);
    assert(subscribe > comv && start > subscribe && sent[subscribe].payload0 == HFI_MODE_PORT_SETTINGS_CHANGE);
    assert(find(before, HFI_CMD_PAUSE, ~0u) < 0);

    /* the firmware parsed the stream: 1920x1088 coded, 8 pixels cropped at the bottom, 18 output buffers */
    begin(&s);
    const uint32_t resolution = 1920u << 16 | 1088, crop[2] = {0, 8}, depth = (8u << 16) | 8, frames = HFI_BITMASK_FRAME_MBS_ONLY_FLAG, min = 18;
    add(HFI_PROP_BITSTREAM_RESOLUTION, HFI_PORT_BITSTREAM, 0, HFI_PAYLOAD_U32, &resolution, 4);
    add(HFI_PROP_CROP_OFFSETS, HFI_PORT_BITSTREAM, 0, HFI_PAYLOAD_64_PACKED, crop, 8);
    add(HFI_PROP_LUMA_CHROMA_BIT_DEPTH, HFI_PORT_BITSTREAM, 0, HFI_PAYLOAD_U32, &depth, 4);
    add(HFI_PROP_CODED_FRAMES, HFI_PORT_BITSTREAM, 0, HFI_PAYLOAD_U32, &frames, 4);
    add(HFI_PROP_BUFFER_FW_MIN_OUTPUT_COUNT, HFI_PORT_BITSTREAM, 0, HFI_PAYLOAD_U32, &min, 4);
    add(HFI_CMD_SETTINGS_CHANGE, HFI_PORT_BITSTREAM, HFI_FW_FLAGS_SUCCESS, HFI_PAYLOAD_NONE, NULL, 0);
    finish(&s);
    assert(changed && info.supported && info.coded_width == 1920 && info.coded_height == 1088 && info.crop_height == 1080);
    assert(info.output_width == 1920 && info.output_height == 1088 && info.output_min_count == 18 && info.output_size > 1920 * 1088);
    assert((s.sub_state & (QcomIrisSubFirstIpsc | QcomIrisSubInputPause)) == (QcomIrisSubFirstIpsc | QcomIrisSubInputPause));

    /* output start: input internal buffers released and made anew, RESUME with SETTINGS_CHANGE, the output subscription, START raw */
    const unsigned out_before = count;
    assert(!qcom_iris_decoder_start_output(&s, 1920, 1088) && s.state == QcomIrisStreaming && !s.sub_state);
    const int stride = find(out_before, HFI_PROP_LINEAR_STRIDE_SCANLINE, HFI_PORT_RAW);
    assert(stride >= 0 && sent[stride].payload0 == (1920u << 16 | 1088));
    const int released_bin = find(out_before, HFI_CMD_BUFFER, HFI_PORT_BITSTREAM);
    assert(released_bin > stride && (sent[released_bin].buffer.flags & HFI_BUF_HOST_FLAG_RELEASE));
    const int resume = find(out_before, HFI_CMD_RESUME, HFI_PORT_BITSTREAM);
    assert(resume > released_bin && sent[resume].payload0 == HFI_CMD_SETTINGS_CHANGE);
    const int raw_start = find(out_before, HFI_CMD_START, HFI_PORT_RAW);
    assert(raw_start > resume);
    unsigned dpb = 0;
    for (unsigned i = raw_start; i < count; ++i) if (sent[i].type == HFI_CMD_BUFFER && sent[i].buffer.type == HFI_BUFFER_DPB) ++dpb;
    assert(dpb == 18);
    /* the firmware gives the old input internal buffers back: they are destroyed */
    const unsigned released_before = released;
    for (unsigned i = out_before; i < count; ++i)
        if (sent[i].type == HFI_CMD_BUFFER && (sent[i].buffer.flags & HFI_BUF_HOST_FLAG_RELEASE))
            buffer_done(&s, sent[i].buffer.type, sent[i].port, sent[i].buffer.index, sent[i].buffer.base_address, 0,
                        HFI_BUF_FW_FLAG_RELEASE_DONE, 0, 0);
    assert(released > released_before);

    /* buffers: an input aligned to 256 bytes, an output; done with a picture type */
    assert(!qcom_iris_session_queue_input(&s, 3, 0x50000000, 1000, 0, 999, 33, 0));
    assert(sent[count - 1].buffer.buffer_size == 1024 && sent[count - 1].buffer.type == HFI_BUFFER_BITSTREAM && sent[count - 1].buffer.timestamp == 33);
    assert(!qcom_iris_session_queue_output(&s, 5, 0x60000000, info.output_size));
    assert(sent[count - 1].port == HFI_PORT_RAW && (sent[count - 1].buffer.flags & HFI_BUF_HOST_FLAGS_CB_NON_SECURE));
    buffer_done(&s, HFI_BUFFER_BITSTREAM, HFI_PORT_BITSTREAM, 3, 0x50000000, 0, 0, 33, 0);
    buffer_done(&s, HFI_BUFFER_RAW, HFI_PORT_RAW, 5, 0x60000000, 3133440, 0, 33, HFI_GEN2_PICTURE_IDR);
    assert(input_count == 1 && inputs[0].index == 3 && output_count == 1 && outputs[0].index == 5 && outputs[0].keyframe && !outputs[0].error);

    /* drain: DRAIN on the bitstream port, the last output, then resume both sides */
    assert(!qcom_iris_decoder_drain(&s) && (s.sub_state & QcomIrisSubDrain));
    assert(sent[count - 1].type == HFI_CMD_DRAIN && sent[count - 1].port == HFI_PORT_BITSTREAM);
    deliver(&s, HFI_CMD_DRAIN, HFI_PORT_BITSTREAM, HFI_FW_FLAGS_SUCCESS, NULL, 0);
    buffer_done(&s, HFI_BUFFER_RAW, HFI_PORT_RAW, 6, 0x61000000, 0, HFI_BUF_FW_FLAG_LAST, 0, 0);
    assert(outputs[1].last && !outputs[1].error && (s.sub_state & QcomIrisSubDrainLast));
    const unsigned resume_before = count;
    assert(!qcom_iris_decoder_resume(&s) && !s.sub_state);
    assert(sent[resume_before].type == HFI_CMD_RESUME && sent[resume_before].payload0 == HFI_CMD_DRAIN &&
           sent[resume_before + 1].type == HFI_CMD_RESUME && sent[resume_before + 1].port == HFI_PORT_RAW);

    /* stop the output side, then close: every internal buffer released */
    assert(!qcom_iris_session_stop(&s, false) && s.state == QcomIrisInputStreaming);
    assert(!qcom_iris_session_close(&s) && s.state == QcomIrisDeinit && released == allocated);
    printf("session: decoder open, input, settings change, output, buffers, drain, stop, close: ok (%u commands)\n", count);
}

static void encoder(void)
{
    static struct qcom_iris_session s;
    uint32_t header_id = 0, packet_id = 0;
    count = 0;
    qcom_iris_session_init(&s, &ops, &header_id, &packet_id, 2, true, QcomIrisCodecHevc, 4);
    assert(!qcom_iris_session_open(&s));
    assert(sent[1].payload0 == HFI_CODEC_ENCODE_HEVC && count == 2);   /* no default header for an encoder */
    assert(qcom_iris_encoder_set_format(&s, 1920, 1080, 1984, 1088, 30) == -22);   /* the stride is not a multiple of 128 */
    assert(!qcom_iris_encoder_set_format(&s, 1920, 1080, 2048, 1088, 60));
    assert(!qcom_iris_session_set_cap(&s, QcomIrisCapBitrate, 8000000));
    assert(!qcom_iris_session_set_cap(&s, QcomIrisCapBitrateMode, QcomIrisBitrateModeCbr));
    assert(qcom_iris_session_set_cap(&s, QcomIrisCapBitrate, 300000000) == -22);   /* above the table's maximum */
    const unsigned before = count;
    assert(!qcom_iris_encoder_start_input(&s) && s.state == QcomIrisInputStreaming);
    const int stride = find(before, HFI_PROP_LINEAR_STRIDE_SCANLINE, HFI_PORT_RAW), raw = find(before, HFI_PROP_RAW_RESOLUTION, HFI_PORT_RAW);
    assert(stride > raw && sent[stride].payload0 == (2048u << 16 | 1088) && sent[raw].payload0 == (1920u << 16 | 1080));
    const int bitrate = find(before, HFI_PROP_TOTAL_BITRATE, ~0u), rc = find(before, HFI_PROP_RATE_CONTROL, ~0u);
    const int peak = find(before, HFI_PROP_TOTAL_PEAK_BITRATE, ~0u);
    assert(sent[bitrate].payload0 == 8000000 && sent[rc].payload0 == HFI_RC_CBR_CFR && sent[peak].payload0 == 8000000);
    const int arp = find(before, HFI_CMD_BUFFER, ~0u);
    assert(arp > rc && sent[arp].buffer.type == HFI_BUFFER_ARP);
    const unsigned out_before = count;
    assert(!qcom_iris_encoder_start_output(&s) && s.state == QcomIrisStreaming);
    const int frame_rate = find(out_before, HFI_PROP_FRAME_RATE, HFI_PORT_BITSTREAM);
    assert(frame_rate >= 0 && sent[frame_rate].payload0 == 60u << 16);
    const int res = find(out_before, HFI_PROP_BITSTREAM_RESOLUTION, HFI_PORT_BITSTREAM);
    assert(sent[res].payload0 == (1920u << 16 | 1088));   /* aligned to 32 for HEVC */
    assert(find(out_before, HFI_CMD_START, HFI_PORT_BITSTREAM) > res);
    /* a dynamic capability while streaming goes out at once */
    const unsigned dyn = count;
    assert(!qcom_iris_session_set_cap(&s, QcomIrisCapBitrate, 6000000) && count == dyn + 1 && sent[dyn].payload0 == 6000000);
    assert(!qcom_iris_encoder_request_sync_frame(&s) && sent[count - 1].type == HFI_PROP_REQUEST_SYNC_FRAME);
    assert(!qcom_iris_session_close(&s) && released == allocated);
    printf("session: encoder open, format, capabilities, input and output start, dynamic bitrate, close: ok\n");
}

int main(void)
{
    decoder();
    encoder();
    return 0;
}
