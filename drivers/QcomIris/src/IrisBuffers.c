/* SPDX-License-Identifier: GPL-2.0-only */
/* The session view the pinned calculator reads, and the iris_utils.c / iris_buffer.c helpers it calls: iris_get_buffer_size is the
 * mainline function with its format size helpers (NV12, QC08C, decoder and encoder bitstream). */
#include "iris_instance.h"
#include "iris_vpu_buffer.h"
#include "QcomIrisBuffers.h"

#define PIXELS_4K 4096
#define Y_STRIDE_ALIGN 128
#define UV_STRIDE_ALIGN 128
#define Y_SCANLINE_ALIGN 32
#define UV_SCANLINE_ALIGN 16
#define UV_SCANLINE_ALIGN_QC08C 32
#define META_STRIDE_ALIGNED 64
#define Y_STRIDE_ALIGN_P010 256
#define UV_STRIDE_ALIGN_P010 256
#define Y_SCANLINE_ALIGN_QC10C 16
#define META_SCANLINE_ALIGNED 16

int iris_get_mbpf(struct iris_inst *inst)
{
    struct v4l2_format *inp_f = inst->fmt_src;
    u32 height = max(inp_f->fmt.pix_mp.height, inst->crop.height);
    u32 width = max(inp_f->fmt.pix_mp.width, inst->crop.width);
    return DIV_ROUND_UP(height, 16) * DIV_ROUND_UP(width, 16);
}

bool iris_split_mode_enabled(struct iris_inst *inst)
{
    u32 f = inst->fmt_dst->fmt.pix_mp.pixelformat;
    return f == V4L2_PIX_FMT_NV12 || f == V4L2_PIX_FMT_QC08C || f == V4L2_PIX_FMT_P010 || f == V4L2_PIX_FMT_QC10C;
}

bool is_rotation_90_or_270(struct iris_inst *inst)
{
    return inst->fw_caps[ROTATION].value == 90 || inst->fw_caps[ROTATION].value == 270;
}

static u32 iris_yuv_buffer_size_nv12(struct iris_inst *inst)
{
    u32 y_plane, uv_plane, y_stride, uv_stride, y_scanlines, uv_scanlines;
    struct v4l2_format *f = inst->domain == DECODER ? inst->fmt_dst : inst->fmt_src;
    y_stride = ALIGN(f->fmt.pix_mp.width, Y_STRIDE_ALIGN);
    uv_stride = ALIGN(f->fmt.pix_mp.width, UV_STRIDE_ALIGN);
    y_scanlines = ALIGN(f->fmt.pix_mp.height, Y_SCANLINE_ALIGN);
    uv_scanlines = ALIGN((f->fmt.pix_mp.height + 1) >> 1, UV_SCANLINE_ALIGN);
    y_plane = y_stride * y_scanlines;
    uv_plane = uv_stride * uv_scanlines;
    return ALIGN(y_plane + uv_plane, PIXELS_4K);
}

static u32 iris_yuv_buffer_size_qc08c(struct iris_inst *inst)
{
    u32 y_plane, uv_plane, y_stride, uv_stride;
    u32 uv_meta_stride, uv_meta_plane;
    u32 y_meta_stride, y_meta_plane;
    struct v4l2_format *f = inst->domain == DECODER ? inst->fmt_dst : inst->fmt_src;
    y_meta_stride = ALIGN(DIV_ROUND_UP(f->fmt.pix_mp.width, META_STRIDE_ALIGNED >> 1), META_STRIDE_ALIGNED);
    y_meta_plane = y_meta_stride * ALIGN(DIV_ROUND_UP(f->fmt.pix_mp.height, META_SCANLINE_ALIGNED >> 1), META_SCANLINE_ALIGNED);
    y_meta_plane = ALIGN(y_meta_plane, PIXELS_4K);
    y_stride = ALIGN(f->fmt.pix_mp.width, Y_STRIDE_ALIGN);
    y_plane = ALIGN(y_stride * ALIGN(f->fmt.pix_mp.height, Y_SCANLINE_ALIGN), PIXELS_4K);
    uv_meta_stride = ALIGN(DIV_ROUND_UP(f->fmt.pix_mp.width / 2, META_STRIDE_ALIGNED >> 2), META_STRIDE_ALIGNED);
    uv_meta_plane = uv_meta_stride * ALIGN(DIV_ROUND_UP(f->fmt.pix_mp.height / 2, META_SCANLINE_ALIGNED >> 1),
                                           META_SCANLINE_ALIGNED);
    uv_meta_plane = ALIGN(uv_meta_plane, PIXELS_4K);
    uv_stride = ALIGN(f->fmt.pix_mp.width, UV_STRIDE_ALIGN);
    uv_plane = ALIGN(uv_stride * ALIGN(f->fmt.pix_mp.height / 2, UV_SCANLINE_ALIGN_QC08C), PIXELS_4K);
    return ALIGN(y_meta_plane + y_plane + uv_meta_plane + uv_plane, PIXELS_4K);
}

/* P010: two bytes a sample, both strides the width x 2 aligned to 256, the luma rows aligned to 32 and the chroma rows to 16. */
static u32 iris_yuv_buffer_size_p010(struct iris_inst *inst)
{
    struct v4l2_format *f = inst->domain == DECODER ? inst->fmt_dst : inst->fmt_src;
    u32 y_stride = ALIGN(f->fmt.pix_mp.width * 2, Y_STRIDE_ALIGN_P010);
    u32 uv_stride = ALIGN(f->fmt.pix_mp.width * 2, UV_STRIDE_ALIGN_P010);
    u32 y_scanlines = ALIGN(f->fmt.pix_mp.height, Y_SCANLINE_ALIGN);
    u32 uv_scanlines = ALIGN((f->fmt.pix_mp.height + 1) >> 1, UV_SCANLINE_ALIGN);
    return ALIGN(y_stride * y_scanlines + uv_stride * uv_scanlines, PIXELS_4K);
}

/* QC10C, the compressed format of P010: the reference pictures of a 10-bit stream. */
static u32 iris_yuv_buffer_size_qc10c(struct iris_inst *inst)
{
    u32 y_plane, uv_plane, y_stride, uv_stride;
    u32 uv_meta_stride, uv_meta_plane;
    u32 y_meta_stride, y_meta_plane;
    struct v4l2_format *f = inst->domain == DECODER ? inst->fmt_dst : inst->fmt_src;
    y_meta_stride = ALIGN(DIV_ROUND_UP(f->fmt.pix_mp.width, 48), META_STRIDE_ALIGNED);
    y_meta_plane = y_meta_stride * ALIGN(DIV_ROUND_UP(f->fmt.pix_mp.height, 4), META_SCANLINE_ALIGNED);
    y_meta_plane = ALIGN(y_meta_plane, PIXELS_4K);
    y_stride = ALIGN(f->fmt.pix_mp.width * 4 / 3, Y_STRIDE_ALIGN_P010);
    y_plane = ALIGN(y_stride * ALIGN(f->fmt.pix_mp.height, Y_SCANLINE_ALIGN_QC10C), PIXELS_4K);
    uv_meta_stride = ALIGN(DIV_ROUND_UP((f->fmt.pix_mp.width + 1) >> 1, 24), META_STRIDE_ALIGNED);
    uv_meta_plane = uv_meta_stride * ALIGN(DIV_ROUND_UP((f->fmt.pix_mp.height + 1) >> 1, 4), META_SCANLINE_ALIGNED);
    uv_meta_plane = ALIGN(uv_meta_plane, PIXELS_4K);
    uv_stride = ALIGN(f->fmt.pix_mp.width * 4 / 3, UV_STRIDE_ALIGN_P010);
    uv_plane = ALIGN(uv_stride * ALIGN((f->fmt.pix_mp.height + 1) >> 1, UV_SCANLINE_ALIGN), PIXELS_4K);
    return ALIGN(y_meta_plane + y_plane + uv_meta_plane + uv_plane, PIXELS_4K);
}

static u32 iris_dec_bitstream_buffer_size(struct iris_inst *inst)
{
    struct platform_inst_caps *caps = inst->core->iris_platform_data->inst_caps;
    u32 base_res_mbs = NUM_MBS_4K;
    u32 frame_size, num_mbs;
    u32 div_factor = 2;
    num_mbs = iris_get_mbpf(inst);
    if (num_mbs > NUM_MBS_4K) {
        div_factor = 4;
        base_res_mbs = caps->max_mbpf;
    } else if (inst->codec == V4L2_PIX_FMT_VP9) {
        div_factor = 1;
    }
    frame_size = base_res_mbs * (16 * 16) * 3 / 2 / div_factor;
    return ALIGN(frame_size, PIXELS_4K);
}

static u32 iris_enc_bitstream_buffer_size(struct iris_inst *inst)
{
    u32 aligned_width, aligned_height, bitstream_size, yuv_size;
    int bitrate_mode = (int)inst->fw_caps[BITRATE_MODE].value, frame_rc = (int)inst->fw_caps[FRAME_RC_ENABLE].value;
    struct v4l2_format *f = inst->fmt_dst;
    aligned_width = ALIGN(f->fmt.pix_mp.width, 32);
    aligned_height = ALIGN(f->fmt.pix_mp.height, 32);
    bitstream_size = aligned_width * aligned_height * 3;
    yuv_size = (aligned_width * aligned_height * 3) >> 1;
    if (aligned_width * aligned_height > (4096 * 2176))
        bitstream_size = (bitstream_size >> 3);
    else if (aligned_width * aligned_height > (1280 * 720))
        bitstream_size = (bitstream_size >> 2);
    if ((!frame_rc || bitrate_mode == V4L2_MPEG_VIDEO_BITRATE_MODE_CQ) && bitstream_size < yuv_size)
        bitstream_size = (bitstream_size << 1);
    return ALIGN(bitstream_size, 4096);
}

int iris_get_buffer_size(struct iris_inst *inst, enum iris_buffer_type buffer_type)
{
    if (inst->domain == DECODER) {
        switch (buffer_type) {
        case BUF_INPUT: return (int)iris_dec_bitstream_buffer_size(inst);
        case BUF_OUTPUT:
            return (int)(inst->fmt_dst->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_P010 ? iris_yuv_buffer_size_p010(inst)
                                                                                   : iris_yuv_buffer_size_nv12(inst));
        case BUF_DPB:   /* a 10-bit stream's reference pictures are QC10C, an 8-bit one's QC08C */
            return (int)(inst->fmt_dst->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_P010 ? iris_yuv_buffer_size_qc10c(inst)
                                                                                   : iris_yuv_buffer_size_qc08c(inst));
        default: return 0;
        }
    }
    switch (buffer_type) {
    case BUF_INPUT: return (int)iris_yuv_buffer_size_nv12(inst);
    case BUF_OUTPUT: return (int)iris_enc_bitstream_buffer_size(inst);
    default: return 0;
    }
}

struct view {
    struct iris_inst inst;
    struct iris_core core;
    struct iris_platform_data platform;
    struct platform_inst_caps caps;
    struct v4l2_format src, dst;
};
static void view(struct view *v, const struct qcom_iris_buffer_query *q)
{
    static const u32 codecs[] = {V4L2_PIX_FMT_H264, V4L2_PIX_FMT_HEVC, V4L2_PIX_FMT_VP9};
    __builtin_memset(v, 0, sizeof *v);
    v->caps.max_mbpf = q->max_mbpf;
    v->caps.max_frame_width = 8192;   /* platform_inst_cap_sm8550 */
    v->caps.max_frame_height = 8192;
    v->platform.num_vpp_pipe = q->num_vpp_pipe;
    v->platform.inst_caps = &v->caps;
    v->core.iris_platform_data = &v->platform;
    v->inst.core = &v->core;
    v->inst.domain = q->encoder ? ENCODER : DECODER;
    v->inst.codec = (unsigned)q->codec < ARRAY_SIZE(codecs) ? codecs[q->codec] : V4L2_PIX_FMT_H264;
    v->src.fmt.pix_mp.width = q->src_width;
    v->src.fmt.pix_mp.height = q->src_height;
    v->dst.fmt.pix_mp.width = q->dst_width;
    v->dst.fmt.pix_mp.height = q->dst_height;
    if (q->encoder) {
        v->src.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
        v->dst.fmt.pix_mp.pixelformat = v->inst.codec;
    } else {
        v->src.fmt.pix_mp.pixelformat = v->inst.codec;
        v->dst.fmt.pix_mp.pixelformat = q->ten_bit ? V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12;
    }
    v->inst.fmt_src = &v->src;
    v->inst.fmt_dst = &v->dst;
    v->inst.crop.width = q->crop_width;
    v->inst.crop.height = q->crop_height;
    v->inst.fw_min_count = q->fw_min_count;
    v->inst.buffers[BUF_OUTPUT].min_count = q->output_min_count;
    v->inst.fw_caps[STAGE].value = q->stage;
    v->inst.fw_caps[B_FRAME].value = q->b_frames;
    v->inst.fw_caps[LTR_COUNT].value = q->ltr_count;
    v->inst.fw_caps[BITRATE_MODE].value = q->bitrate_mode;
    v->inst.fw_caps[FRAME_RC_ENABLE].value = q->frame_rc;
    v->inst.fw_caps[ROTATION].value = q->rotation;
    v->inst.hfi_rc_type = q->rc_type;
    v->inst.hfi_layer_type = q->layer_type;
    v->inst.hfi_layer_count = q->layer_count;
}

uint32_t qcom_iris_buffer_size(const struct qcom_iris_buffer_query *q, enum qcom_iris_buffer type)
{
    struct view v;
    if (!q || type < QcomIrisBufInput || type >= QcomIrisBufTypeMax) return 0;
    view(&v, q);
    if (type == QcomIrisBufInput || type == QcomIrisBufOutput) return (uint32_t)iris_get_buffer_size(&v.inst, (enum iris_buffer_type)type);
    uint32_t size = iris_vpu_buf_size(&v.inst, (enum iris_buffer_type)type);
    /* The HEVC decoder's persist buffer of the firmware the device ships (vpu30_4v) also holds 768 bytes of three-dimension
     * user data for each of its 32 pictures, which the pinned formula leaves out: with the smaller buffer that firmware answers
     * the start command with HFI_ERROR_INSUFFICIENT_RESOURCES (run118). Every other decoder size of the pinned formulas is at
     * least what that firmware's own driver gives it. */
    if (!q->encoder && q->codec == QcomIrisCodecHevc && type == QcomIrisBufPersist) size = ALIGN(size + NUM_HW_PIC_BUF * 768, DMA_ALIGNMENT);
    return size;
}

uint32_t qcom_iris_buffer_count(const struct qcom_iris_buffer_query *q, enum qcom_iris_buffer type)
{
    struct view v;
    if (!q || type < QcomIrisBufInput || type >= QcomIrisBufTypeMax) return 0;
    view(&v, q);
    return (uint32_t)iris_vpu_buf_count(&v.inst, (enum iris_buffer_type)type);
}
