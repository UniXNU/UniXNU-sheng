/* SPDX-License-Identifier: GPL-2.0-only */
/* Buffer sizes and counts of an iris (VPU 3) session, from the pinned mainline calculator (iris_vpu_buffer.c, VPU 3 entry points
 * iris_vpu_buf_size and iris_vpu_buf_count) and the format sizes of iris_buffer.c. */
#ifndef QCOM_IRIS_BUFFERS_H
#define QCOM_IRIS_BUFFERS_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* enum iris_buffer_type */
enum qcom_iris_buffer {
    QcomIrisBufInput = 1, QcomIrisBufOutput, QcomIrisBufBin, QcomIrisBufArp, QcomIrisBufComv, QcomIrisBufNonComv, QcomIrisBufLine,
    QcomIrisBufDpb, QcomIrisBufPersist, QcomIrisBufScratch1, QcomIrisBufScratch2, QcomIrisBufVpss, QcomIrisBufPartial,
    QcomIrisBufTypeMax,
};
enum qcom_iris_codec {QcomIrisCodecH264 = 0, QcomIrisCodecHevc, QcomIrisCodecVp9};
/* What the calculator reads of a session. Decoder: src is the coded size (the bitstream resolution), dst the NV12 output format
 * (width aligned to 128, height to 32). Encoder: src the NV12 input format (the same alignment), dst the coded size. */
struct qcom_iris_buffer_query {
    bool encoder;
    enum qcom_iris_codec codec;
    uint32_t src_width, src_height, dst_width, dst_height;
    uint32_t crop_width, crop_height;
    uint32_t fw_min_count;        /* the firmware's minimum output count from its settings change, 0 before one */
    uint32_t output_min_count;    /* buffers[BUF_OUTPUT].min_count */
    uint32_t num_vpp_pipe;        /* 4 on SM8550 */
    bool ten_bit;                 /* decoder: a 10-bit stream, output P010 and reference pictures QC10C */
    uint32_t max_mbpf;            /* platform_inst_cap_sm8550 max_mbpf */
    uint32_t stage;               /* fw_caps[STAGE] */
    uint32_t b_frames, ltr_count; /* encoder fw_caps[B_FRAME], fw_caps[LTR_COUNT] */
    uint32_t rc_type;             /* encoder hfi_rc_type (enum hfi_rate_control) */
    uint32_t layer_type, layer_count;
    uint32_t bitrate_mode, frame_rc;
    uint32_t rotation;
};
uint32_t qcom_iris_buffer_size(const struct qcom_iris_buffer_query *, enum qcom_iris_buffer);
uint32_t qcom_iris_buffer_count(const struct qcom_iris_buffer_query *, enum qcom_iris_buffer);
#ifdef __cplusplus
}
#endif
#endif
