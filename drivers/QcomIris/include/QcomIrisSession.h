/* SPDX-License-Identifier: GPL-2.0-only */
/* One iris HFI gen 2 session (decoder or encoder) on SM8550: the mainline session sequences of iris_hfi_gen2_command.c and
 * iris_hfi_gen2_response.c, the instance states of iris_state.c, the decoder and encoder flows of iris_vdec.c, iris_venc.c and
 * iris_common.c, and the SM8550 capability tables with their setters (iris_hfi_gen2.c, iris_ctrls.c). The session owns its internal
 * buffers through the core's allocator and reports frames through callbacks; the client owns input and output buffers. Not thread
 * safe: the core serializes every call and every message for a session. */
#ifndef QCOM_IRIS_SESSION_H
#define QCOM_IRIS_SESSION_H
#include <stdbool.h>
#include <stdint.h>
#include "QcomIrisBuffers.h"
#include "QcomIrisHfi.h"
#ifdef __cplusplus
extern "C" {
#endif
/* enum platform_inst_fw_cap_type (iris_platform_common.h) */
enum qcom_iris_cap {
    QcomIrisCapProfileH264 = 1, QcomIrisCapProfileHevc, QcomIrisCapProfileVp9, QcomIrisCapLevelH264, QcomIrisCapLevelHevc,
    QcomIrisCapLevelVp9, QcomIrisCapProfileAv1, QcomIrisCapLevelAv1, QcomIrisCapTierAv1, QcomIrisCapDrap, QcomIrisCapFilmGrain,
    QcomIrisCapSuperBlock, QcomIrisCapEnhLayerCount, QcomIrisCapInputBufHostMaxCount, QcomIrisCapOutputBufHostMaxCount,
    QcomIrisCapStage, QcomIrisCapPipe, QcomIrisCapPoc, QcomIrisCapCodedFrames, QcomIrisCapBitDepth, QcomIrisCapRapFrame,
    QcomIrisCapTier, QcomIrisCapHeaderMode, QcomIrisCapPrependSpsPpsToIdr, QcomIrisCapBitrate, QcomIrisCapBitratePeak,
    QcomIrisCapBitrateMode, QcomIrisCapFrameSkipMode, QcomIrisCapFrameRcEnable, QcomIrisCapGopSize, QcomIrisCapEntropyMode,
    QcomIrisCapMinFrameQpH264, QcomIrisCapMinFrameQpHevc, QcomIrisCapMaxFrameQpH264, QcomIrisCapMaxFrameQpHevc,
    QcomIrisCapIFrameMinQpH264, QcomIrisCapIFrameMinQpHevc, QcomIrisCapPFrameMinQpH264, QcomIrisCapPFrameMinQpHevc,
    QcomIrisCapBFrameMinQpH264, QcomIrisCapBFrameMinQpHevc, QcomIrisCapIFrameMaxQpH264, QcomIrisCapIFrameMaxQpHevc,
    QcomIrisCapPFrameMaxQpH264, QcomIrisCapPFrameMaxQpHevc, QcomIrisCapBFrameMaxQpH264, QcomIrisCapBFrameMaxQpHevc,
    QcomIrisCapIFrameQpH264, QcomIrisCapIFrameQpHevc, QcomIrisCapPFrameQpH264, QcomIrisCapPFrameQpHevc, QcomIrisCapBFrameQpH264,
    QcomIrisCapBFrameQpHevc, QcomIrisCapRotation, QcomIrisCapHflip, QcomIrisCapVflip, QcomIrisCapIrType, QcomIrisCapIrPeriod,
    QcomIrisCapLtrCount, QcomIrisCapUseLtr, QcomIrisCapMarkLtr, QcomIrisCapBFrame, QcomIrisCapLayerEnable, QcomIrisCapLayerTypeH264,
    QcomIrisCapLayerTypeHevc, QcomIrisCapLayerCountH264, QcomIrisCapLayerCountHevc, QcomIrisCapLayer0BitrateH264,
    QcomIrisCapLayer1BitrateH264, QcomIrisCapLayer2BitrateH264, QcomIrisCapLayer3BitrateH264, QcomIrisCapLayer4BitrateH264,
    QcomIrisCapLayer5BitrateH264, QcomIrisCapLayer0BitrateHevc, QcomIrisCapLayer1BitrateHevc, QcomIrisCapLayer2BitrateHevc,
    QcomIrisCapLayer3BitrateHevc, QcomIrisCapLayer4BitrateHevc, QcomIrisCapLayer5BitrateHevc, QcomIrisCapRequestSyncFrame,
    QcomIrisCapTimeDeltaBasedRc, QcomIrisCapOutputOrder, QcomIrisCapMax,
};
/* enum platform_inst_fw_cap_flags */
enum {
    QcomIrisCapDynamic = 1u << 0, QcomIrisCapMenu = 1u << 1, QcomIrisCapInputPort = 1u << 2, QcomIrisCapOutputPort = 1u << 3,
    QcomIrisCapClientSet = 1u << 4, QcomIrisCapBitmask = 1u << 5, QcomIrisCapVolatile = 1u << 6,
};
/* V4L2 values the capabilities take (v4l2-controls.h); HFI gen 2 takes them as they are. */
enum {
    QcomIrisH264ProfileBaseline = 0, QcomIrisH264ProfileConstrainedBaseline = 1, QcomIrisH264ProfileMain = 2,
    QcomIrisH264ProfileHigh = 4, QcomIrisH264ProfileConstrainedHigh = 17,
    QcomIrisHevcProfileMain = 0, QcomIrisHevcProfileMain10 = 2,
    QcomIrisBitrateModeVbr = 0, QcomIrisBitrateModeCbr = 1, QcomIrisBitrateModeCq = 2,
    QcomIrisEntropyCavlc = 0, QcomIrisEntropyCabac = 1,
};

enum qcom_iris_session_state {QcomIrisDeinit = 1, QcomIrisInit, QcomIrisInputStreaming, QcomIrisOutputStreaming, QcomIrisStreaming, QcomIrisError};
/* enum iris_inst_sub_state */
enum {
    QcomIrisSubFirstIpsc = 1u << 0, QcomIrisSubDrc = 1u << 1, QcomIrisSubDrcLast = 1u << 2, QcomIrisSubDrain = 1u << 3,
    QcomIrisSubDrainLast = 1u << 4, QcomIrisSubInputPause = 1u << 5, QcomIrisSubOutputPause = 1u << 6,
};

/* A decoded (decoder) or coded (encoder) output buffer the firmware handed back, or an input buffer it is done with. */
struct qcom_iris_frame {
    uint32_t index;                  /* the client's index */
    uint64_t device_addr;
    uint32_t data_offset, data_size;
    uint64_t timestamp;
    uint32_t hfi_flags;              /* HFI_BUF_FW_FLAG_* */
    uint32_t picture_type;           /* HFI_GEN2_PICTURE_* (0 when the firmware sent none) */
    bool error, last, keyframe;
};
/* What a settings change says about the stream (iris_hfi_gen2_read_input_subcr_params). */
struct qcom_iris_stream_info {
    uint32_t coded_width, coded_height;       /* the bitstream resolution */
    uint32_t crop_left, crop_top, crop_width, crop_height;
    uint32_t output_width, output_height;     /* the output format: width aligned to 128, height to 32; NV12, or P010 (two bytes a
                                               * sample, rows of the width x 2 aligned to 256) when bit_depth says 10 */
    uint32_t output_size;                     /* bytes of one output buffer */
    uint32_t output_min_count;                /* buffers the client must give */
    uint32_t bit_depth, color_info, profile, level;
    bool supported;                           /* 8 or 10 bits, progressive */
};

struct qcom_iris_session;
struct qcom_iris_session_ops {
    void *context;
    /* Writes a message to the command queue and raises the interrupt. */
    int (*send)(void *context, const void *message, uint32_t size);
    /* An internal buffer the firmware alone uses: device address and an opaque handle. */
    int (*allocate)(void *context, uint32_t bytes, uint64_t *device_addr, void **handle);
    void (*release)(void *context, void *handle);
    /* Waits for the session's response (close, stop): returns true when signal() was called, false at the timeout. */
    bool (*wait)(void *context, struct qcom_iris_session *session, unsigned milliseconds);
    void (*signal)(void *context, struct qcom_iris_session *session);
    /* Events, from the message handler. */
    void (*input_done)(void *context, struct qcom_iris_session *, const struct qcom_iris_frame *);
    void (*output_done)(void *context, struct qcom_iris_session *, const struct qcom_iris_frame *);
    void (*settings_change)(void *context, struct qcom_iris_session *, const struct qcom_iris_stream_info *);
    void (*failed)(void *context, struct qcom_iris_session *, uint32_t hfi_error);
};

struct qcom_iris_cap_value {
    int64_t min, max, value;
    uint32_t hfi_id, flags;
    uint8_t setter;                  /* index into the session's setter table; 0: none */
    uint8_t valid;
};
struct qcom_iris_subscription {
    uint32_t bitstream_resolution, crop_offsets[2], bit_depth, coded_frames, fw_min_count, pic_order_cnt, color_info, profile, level,
        tier;
};
enum {QcomIrisInternalMax = 64};
struct qcom_iris_internal {
    uint32_t type;                   /* enum qcom_iris_buffer */
    uint32_t index, size;
    uint64_t device_addr;
    void *handle;
    bool queued, pending_release;
};

struct qcom_iris_session {
    const struct qcom_iris_session_ops *ops;
    uint32_t *header_id, *packet_id;  /* the core's counters */
    uint32_t session_id;
    bool encoder;
    enum qcom_iris_codec codec;
    enum qcom_iris_session_state state;
    uint32_t sub_state;
    uint32_t num_vpp_pipe;
    /* decoder: src the bitstream, dst NV12; encoder: src NV12 (raw), dst the bitstream */
    uint32_t src_width, src_height, dst_width, dst_height;
    uint32_t crop_left, crop_top, crop_width, crop_height;
    uint32_t enc_raw_width, enc_raw_height, enc_stride, enc_scanline;
    uint32_t frame_rate, operating_rate;
    uint32_t color_primaries, transfer, matrix, full_range;   /* HFI values; 0 reserved: none given */
    bool color_described;
    struct qcom_iris_cap_value caps[QcomIrisCapMax];
    uint32_t hfi_rc_type, hfi_layer_type, hfi_layer_count;
    struct qcom_iris_subscription src_subcr, dst_subcr;
    bool ipsc_properties_set, opsc_properties_set;
    uint32_t fw_min_count, output_min_count;
    struct qcom_iris_internal internal[QcomIrisInternalMax];
    struct {uint32_t picture_type; bool data_corrupt, overflow, no_output;} frame_info;
    bool responded;                  /* a waited-for response arrived */
    uint32_t last_error;
    uint8_t packet[4096];            /* the session's command buffer (inst_hfi_gen2->packet) */
};

/* iris_vdec_inst_init / iris_venc_inst_init and iris_session_init_caps. num_vpp_pipe: 4 on SM8550. */
void qcom_iris_session_init(struct qcom_iris_session *, const struct qcom_iris_session_ops *, uint32_t *header_id, uint32_t *packet_id,
                            uint32_t session_id, bool encoder, enum qcom_iris_codec codec, uint32_t num_vpp_pipe);
/* iris_hfi_gen2_session_open: OPEN, the codec and (decoder) no default header. */
int qcom_iris_session_open(struct qcom_iris_session *);
/* iris_hfi_gen2_session_close, waiting for the firmware's answer; then every internal buffer is released. */
int qcom_iris_session_close(struct qcom_iris_session *);

/* Decoder: the coded size from the stream's format description (iris_vdec_s_fmt on the OUTPUT queue). */
int qcom_iris_decoder_set_format(struct qcom_iris_session *, uint32_t coded_width, uint32_t coded_height);
/* iris_vdec_streamon_input */
int qcom_iris_decoder_start_input(struct qcom_iris_session *);
/* iris_vdec_streamon_output: after the first settings change (or a resolution change), with the client's output size. */
int qcom_iris_decoder_start_output(struct qcom_iris_session *, uint32_t output_width, uint32_t output_height);
/* iris_vdec_stop_cmd (drain) and iris_vdec_start_cmd (resume after the drain's last buffer). */
int qcom_iris_decoder_drain(struct qcom_iris_session *);
int qcom_iris_decoder_resume(struct qcom_iris_session *);

/* Encoder: the raw input format (NV12, stride and scanlines of the surface), the coded size and frame rate. */
int qcom_iris_encoder_set_format(struct qcom_iris_session *, uint32_t width, uint32_t height, uint32_t stride, uint32_t scanline,
                                 uint32_t frame_rate);
int qcom_iris_encoder_start_input(struct qcom_iris_session *);
int qcom_iris_encoder_start_output(struct qcom_iris_session *);
int qcom_iris_encoder_drain(struct qcom_iris_session *);
int qcom_iris_encoder_resume(struct qcom_iris_session *);
/* A capability set by the client (CAP_FLAG_CLIENT_SET), and sent at once when the session streams and it is dynamic. */
int qcom_iris_session_set_cap(struct qcom_iris_session *, enum qcom_iris_cap, int64_t value);
/* REQUEST_SYNC_FRAME: the next frame a key frame. */
int qcom_iris_encoder_request_sync_frame(struct qcom_iris_session *);

/* iris_hfi_gen2_session_queue_buffer for the client's buffers. */
int qcom_iris_session_queue_input(struct qcom_iris_session *, uint32_t index, uint64_t device_addr, uint32_t buffer_size,
                                  uint32_t data_offset, uint32_t data_size, uint64_t timestamp, uint32_t hfi_flags);
int qcom_iris_session_queue_output(struct qcom_iris_session *, uint32_t index, uint64_t device_addr, uint32_t buffer_size);
/* iris_session_streamoff: STOP on a port, waiting for the firmware; the firmware hands every buffer of the port back first. */
int qcom_iris_session_stop(struct qcom_iris_session *, bool input);

/* iris_hfi_gen2_handle_session_response for a validated message of this session. */
void qcom_iris_session_handle(struct qcom_iris_session *, const void *message);
/* A system error: the session goes to the error state (iris_hfi_gen2_handle_system_error). */
void qcom_iris_session_fail(struct qcom_iris_session *, uint32_t hfi_error);
#ifdef __cplusplus
}
#endif
#endif
