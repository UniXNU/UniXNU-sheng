/* SPDX-License-Identifier: GPL-2.0-only */
/* The part of the mainline iris instance the pinned buffer calculator (iris_vpu_buffer.c) reads: the session's formats, codec,
 * domain, crop, firmware capabilities and buffer counts, and the platform's pipes and limits. Field names and enum values follow
 * iris_instance.h, iris_core.h, iris_buffer.h, iris_platform_common.h and the V4L2 uapi headers of the pinned Linux release. */
#ifndef QCOM_IRIS_SHIM_INSTANCE_H
#define QCOM_IRIS_SHIM_INSTANCE_H
#include <linux/types.h>

#define v4l2_fourcc(a, b, c, d) ((u32)(a) | ((u32)(b) << 8) | ((u32)(c) << 16) | ((u32)(d) << 24))
#define V4L2_PIX_FMT_NV12 v4l2_fourcc('N', 'V', '1', '2')
#define V4L2_PIX_FMT_P010 v4l2_fourcc('P', '0', '1', '0')
#define V4L2_PIX_FMT_H264 v4l2_fourcc('H', '2', '6', '4')
#define V4L2_PIX_FMT_VP9 v4l2_fourcc('V', 'P', '9', '0')
#define V4L2_PIX_FMT_HEVC v4l2_fourcc('H', 'E', 'V', 'C')
#define V4L2_PIX_FMT_AV1 v4l2_fourcc('A', 'V', '0', '1')
#define V4L2_PIX_FMT_QC08C v4l2_fourcc('Q', '0', '8', 'C')
#define V4L2_PIX_FMT_QC10C v4l2_fourcc('Q', '1', '0', 'C')
#define V4L2_MPEG_VIDEO_BITRATE_MODE_VBR 0
#define V4L2_MPEG_VIDEO_BITRATE_MODE_CBR 1
#define V4L2_MPEG_VIDEO_BITRATE_MODE_CQ 2
#define VIDEO_MAX_FRAME 32

enum domain_type {ENCODER = BIT(0), DECODER = BIT(1)};
enum iris_buffer_type {
    BUF_INPUT = 1, BUF_OUTPUT, BUF_BIN, BUF_ARP, BUF_COMV, BUF_NON_COMV, BUF_LINE, BUF_DPB, BUF_PERSIST, BUF_SCRATCH_1,
    BUF_SCRATCH_2, BUF_VPSS, BUF_PARTIAL, BUF_TYPE_MAX,
};
enum stage_type {STAGE_1 = 1, STAGE_2 = 2};
enum platform_inst_fw_cap_type {
    PROFILE_H264 = 1, PROFILE_HEVC, PROFILE_VP9, LEVEL_H264, LEVEL_HEVC, LEVEL_VP9, PROFILE_AV1, LEVEL_AV1, TIER_AV1, DRAP,
    FILM_GRAIN, SUPER_BLOCK, ENH_LAYER_COUNT, INPUT_BUF_HOST_MAX_COUNT, OUTPUT_BUF_HOST_MAX_COUNT, STAGE, PIPE, POC, CODED_FRAMES,
    BIT_DEPTH, RAP_FRAME, TIER, HEADER_MODE, PREPEND_SPSPPS_TO_IDR, BITRATE, BITRATE_PEAK, BITRATE_MODE, FRAME_SKIP_MODE,
    FRAME_RC_ENABLE, GOP_SIZE, ENTROPY_MODE, MIN_FRAME_QP_H264, MIN_FRAME_QP_HEVC, MAX_FRAME_QP_H264, MAX_FRAME_QP_HEVC,
    I_FRAME_MIN_QP_H264, I_FRAME_MIN_QP_HEVC, P_FRAME_MIN_QP_H264, P_FRAME_MIN_QP_HEVC, B_FRAME_MIN_QP_H264, B_FRAME_MIN_QP_HEVC,
    I_FRAME_MAX_QP_H264, I_FRAME_MAX_QP_HEVC, P_FRAME_MAX_QP_H264, P_FRAME_MAX_QP_HEVC, B_FRAME_MAX_QP_H264, B_FRAME_MAX_QP_HEVC,
    I_FRAME_QP_H264, I_FRAME_QP_HEVC, P_FRAME_QP_H264, P_FRAME_QP_HEVC, B_FRAME_QP_H264, B_FRAME_QP_HEVC, ROTATION, HFLIP, VFLIP,
    IR_TYPE, IR_PERIOD, LTR_COUNT, USE_LTR, MARK_LTR, B_FRAME, LAYER_ENABLE, LAYER_TYPE_H264, LAYER_TYPE_HEVC, LAYER_COUNT_H264,
    LAYER_COUNT_HEVC, LAYER0_BITRATE_H264, LAYER1_BITRATE_H264, LAYER2_BITRATE_H264, LAYER3_BITRATE_H264, LAYER4_BITRATE_H264,
    LAYER5_BITRATE_H264, LAYER0_BITRATE_HEVC, LAYER1_BITRATE_HEVC, LAYER2_BITRATE_HEVC, LAYER3_BITRATE_HEVC, LAYER4_BITRATE_HEVC,
    LAYER5_BITRATE_HEVC, REQUEST_SYNC_FRAME, TIME_DELTA_BASED_RC, INST_FW_CAP_MAX,
};

struct v4l2_pix_format_mplane {u32 width, height, pixelformat;};
struct v4l2_format {struct {struct v4l2_pix_format_mplane pix_mp;} fmt;};
struct iris_hfi_rect_desc {u32 left, top, width, height;};
struct platform_inst_fw_cap {s64 value;};
struct iris_buffers {u32 min_count, size;};
struct platform_inst_caps {u32 max_frame_width, max_frame_height, max_mbpf;};
struct iris_platform_data {u32 num_vpp_pipe; struct platform_inst_caps *inst_caps;};
struct iris_core {const struct iris_platform_data *iris_platform_data;};
struct iris_inst {
    struct iris_core *core;
    struct v4l2_format *fmt_src, *fmt_dst;
    enum domain_type domain;
    struct iris_hfi_rect_desc crop;
    struct platform_inst_fw_cap fw_caps[INST_FW_CAP_MAX];
    struct iris_buffers buffers[BUF_TYPE_MAX];
    u32 fw_min_count;
    u32 codec;
    u32 hfi_rc_type, hfi_layer_type, hfi_layer_count;
};

/* iris_utils.c and iris_buffer.c helpers the calculator calls (IrisBuffers.c). */
int iris_get_mbpf(struct iris_inst *inst);
bool iris_split_mode_enabled(struct iris_inst *inst);
bool is_rotation_90_or_270(struct iris_inst *inst);
int iris_get_buffer_size(struct iris_inst *inst, enum iris_buffer_type buffer_type);
#endif
