/* SPDX-License-Identifier: GPL-2.0-only */
#include "QcomIrisSession.h"
#include "iris_hfi_gen1_defines.h"   /* HFI_RATE_CONTROL_OFF / _CQ, which iris_set_layer_type compares against */

#define EINVAL_ 22
#define ETIMEDOUT_ 60
#define ENOMEM_ 12
#define ENOSPC_ 28
#define ALIGN_UP(x, a) (((x) + (a) - 1) / (a) * (a))
#define HW_RESPONSE_TIMEOUT_MS 1000
#define BITRATE_MAX 245000000
#define CABAC_MAX_BITRATE 160000000
#define CAVLC_MAX_BITRATE 220000000
#define BIT_DEPTH_8 ((8u << 16) | 8u)
#define BIT_DEPTH_10 ((10u << 16) | 10u)
#define UNSPECIFIED_COLOR_FORMAT 5

/* ---- capability tables (iris_hfi_gen2.c inst_fw_cap_sm8550_dec / _enc) and their setters (iris_ctrls.c) ---- */
enum setter {
    SetNone = 0, SetU32Enum, SetU32, SetStage, SetPipe, SetProfile, SetLevel, SetHeaderModeGen2, SetBitrateGen2, SetPeakBitrate,
    SetBitrateModeGen2, SetTimeDeltaRc, SetEntropyGen2, SetMinQp, SetMaxQp, SetFrameQp, SetRotation, SetFlip, SetIrPeriodGen2,
    SetLtrCountGen2, SetUseAndMarkLtr, SetLayerType, SetLayerCountGen2, SetLayerBitrate, SetReqSyncFrame,
};
struct cap_def {uint8_t id; int64_t min, max, value; uint32_t hfi_id, flags; uint8_t setter;};
#define OUT QcomIrisCapOutputPort
#define IN QcomIrisCapInputPort
#define DYN QcomIrisCapDynamic
#define MENU QcomIrisCapMenu
static const struct cap_def dec_caps[] = {
    {QcomIrisCapProfileH264, 0, 17, 4, HFI_PROP_PROFILE, OUT | MENU, SetU32Enum},
    {QcomIrisCapProfileHevc, 0, 2, 0, HFI_PROP_PROFILE, OUT | MENU, SetU32Enum},
    {QcomIrisCapProfileAv1, 0, 0, 0, HFI_PROP_PROFILE, OUT | MENU, SetU32Enum},
    {QcomIrisCapProfileVp9, 0, 2, 0, HFI_PROP_PROFILE, OUT | MENU, SetU32Enum},
    {QcomIrisCapLevelH264, 0, 19, 18, HFI_PROP_LEVEL, OUT | MENU, SetU32Enum},
    {QcomIrisCapLevelHevc, 0, 12, 11, HFI_PROP_LEVEL, OUT | MENU, SetU32Enum},
    {QcomIrisCapLevelVp9, 0, 11, 11, HFI_PROP_LEVEL, OUT | MENU, SetU32Enum},
    {QcomIrisCapLevelAv1, 0, 17, 17, HFI_PROP_LEVEL, OUT | MENU, SetU32Enum},
    {QcomIrisCapTier, 0, 1, 1, HFI_PROP_TIER, OUT | MENU, SetU32Enum},
    {QcomIrisCapTierAv1, 0, 1, 0, HFI_PROP_TIER, IN, SetU32},
    {QcomIrisCapDrap, 0, 1, 0, HFI_PROP_AV1_DRAP_CONFIG, IN, SetU32},
    {QcomIrisCapFilmGrain, 0, 1, 0, HFI_PROP_AV1_FILM_GRAIN_PRESENT, QcomIrisCapVolatile, SetNone},
    {QcomIrisCapSuperBlock, 0, 1, 0, HFI_PROP_AV1_SUPER_BLOCK_ENABLED, 0, SetNone},
    {QcomIrisCapEnhLayerCount, 0, 1, 0, HFI_PROP_AV1_OP_POINT, IN, SetU32},
    {QcomIrisCapInputBufHostMaxCount, 64, 256, 64, HFI_PROP_BUFFER_HOST_MAX_COUNT, IN, SetU32},
    {QcomIrisCapStage, 1, 2, 2, HFI_PROP_STAGE, 0, SetStage},
    {QcomIrisCapPipe, 0, 0, 0, HFI_PROP_PIPE, 0, SetPipe},
    {QcomIrisCapPoc, 0, 2, 1, HFI_PROP_PIC_ORDER_CNT_TYPE, 0, SetNone},
    {QcomIrisCapCodedFrames, 0, 0, 0, HFI_PROP_CODED_FRAMES, 0, SetNone},
    {QcomIrisCapBitDepth, BIT_DEPTH_8, BIT_DEPTH_10, BIT_DEPTH_8, HFI_PROP_LUMA_CHROMA_BIT_DEPTH, 0, SetNone},
    {QcomIrisCapRapFrame, 0, 1, 1, HFI_PROP_DEC_START_FROM_RAP_FRAME, IN, SetU32},
};
static const struct cap_def enc_caps[] = {
    {QcomIrisCapProfileH264, 0, 17, 4, HFI_PROP_PROFILE, OUT | MENU, SetProfile},
    {QcomIrisCapProfileHevc, 0, 2, 0, HFI_PROP_PROFILE, OUT | MENU, SetProfile},
    {QcomIrisCapLevelH264, 0, 17, 14, HFI_PROP_LEVEL, OUT | MENU, SetLevel},
    {QcomIrisCapLevelHevc, 0, 12, 7, HFI_PROP_LEVEL, OUT | MENU, SetLevel},
    {QcomIrisCapStage, 1, 2, 2, HFI_PROP_STAGE, 0, SetStage},
    {QcomIrisCapHeaderMode, 0, 1, 1, HFI_PROP_SEQ_HEADER_MODE, OUT | MENU, SetHeaderModeGen2},
    {QcomIrisCapPrependSpsPpsToIdr, 0, 1, 0, 0, 0, SetNone},
    {QcomIrisCapBitrate, 1, BITRATE_MAX, 20000000, HFI_PROP_TOTAL_BITRATE, OUT | IN | DYN, SetBitrateGen2},
    {QcomIrisCapBitratePeak, 1, BITRATE_MAX, 20000000, HFI_PROP_TOTAL_PEAK_BITRATE, OUT | IN | DYN, SetPeakBitrate},
    {QcomIrisCapBitrateMode, 0, 1, 0, HFI_PROP_RATE_CONTROL, OUT | MENU, SetBitrateModeGen2},
    {QcomIrisCapTimeDeltaBasedRc, 0, 1, 0, HFI_PROP_TIME_DELTA_BASED_RATE_CONTROL, OUT, SetTimeDeltaRc},
    {QcomIrisCapFrameSkipMode, 0, 2, 0, 0, OUT | MENU, SetNone},
    {QcomIrisCapFrameRcEnable, 0, 1, 1, 0, 0, SetNone},
    {QcomIrisCapGopSize, 0, 0x7fffffff, 2 * 30 - 1, HFI_PROP_MAX_GOP_FRAMES, OUT | IN | DYN, SetU32},
    {QcomIrisCapEntropyMode, 0, 1, 1, HFI_PROP_CABAC_SESSION, OUT | MENU, SetEntropyGen2},
    {QcomIrisCapMinFrameQpH264, 1, 51, 1, HFI_PROP_MIN_QP_PACKED, OUT, SetMinQp},
    {QcomIrisCapMinFrameQpHevc, 1, 51, 1, HFI_PROP_MIN_QP_PACKED, OUT, SetMinQp},
    {QcomIrisCapMaxFrameQpH264, 1, 51, 51, HFI_PROP_MAX_QP_PACKED, OUT, SetMaxQp},
    {QcomIrisCapMaxFrameQpHevc, 1, 51, 51, HFI_PROP_MAX_QP_PACKED, OUT, SetMaxQp},
    {QcomIrisCapIFrameMinQpH264, 1, 51, 1, 0, 0, SetNone}, {QcomIrisCapIFrameMinQpHevc, 1, 51, 1, 0, 0, SetNone},
    {QcomIrisCapPFrameMinQpH264, 1, 51, 1, 0, 0, SetNone}, {QcomIrisCapPFrameMinQpHevc, 1, 51, 1, 0, 0, SetNone},
    {QcomIrisCapBFrameMinQpH264, 1, 51, 1, 0, 0, SetNone}, {QcomIrisCapBFrameMinQpHevc, 1, 51, 1, 0, 0, SetNone},
    {QcomIrisCapIFrameMaxQpH264, 1, 51, 51, 0, 0, SetNone}, {QcomIrisCapIFrameMaxQpHevc, 1, 51, 51, 0, 0, SetNone},
    {QcomIrisCapPFrameMaxQpH264, 1, 51, 51, 0, 0, SetNone}, {QcomIrisCapPFrameMaxQpHevc, 1, 51, 51, 0, 0, SetNone},
    {QcomIrisCapBFrameMaxQpH264, 1, 51, 51, 0, 0, SetNone}, {QcomIrisCapBFrameMaxQpHevc, 1, 51, 51, 0, 0, SetNone},
    {QcomIrisCapIFrameQpH264, 1, 51, 20, HFI_PROP_QP_PACKED, OUT | IN | DYN, SetFrameQp},
    {QcomIrisCapIFrameQpHevc, 1, 51, 20, HFI_PROP_QP_PACKED, OUT | IN | DYN, SetFrameQp},
    {QcomIrisCapPFrameQpH264, 1, 51, 20, HFI_PROP_QP_PACKED, OUT | IN | DYN, SetFrameQp},
    {QcomIrisCapPFrameQpHevc, 1, 51, 20, HFI_PROP_QP_PACKED, OUT | IN | DYN, SetFrameQp},
    {QcomIrisCapBFrameQpH264, 1, 51, 20, HFI_PROP_QP_PACKED, OUT | IN | DYN, SetFrameQp},
    {QcomIrisCapBFrameQpHevc, 1, 51, 20, HFI_PROP_QP_PACKED, OUT | IN | DYN, SetFrameQp},
    {QcomIrisCapInputBufHostMaxCount, 64, 256, 64, HFI_PROP_BUFFER_HOST_MAX_COUNT, IN, SetU32},
    {QcomIrisCapOutputBufHostMaxCount, 64, 256, 64, HFI_PROP_BUFFER_HOST_MAX_COUNT, OUT, SetU32},
    {QcomIrisCapRotation, 0, 270, 0, HFI_PROP_ROTATION, OUT, SetRotation},
    {QcomIrisCapHflip, 0, 1, 0, HFI_PROP_FLIP, OUT | IN | DYN, SetFlip},
    {QcomIrisCapVflip, 0, 1, 0, HFI_PROP_FLIP, OUT | IN | DYN, SetFlip},
    {QcomIrisCapIrType, 0, 1, 0, 0, OUT | MENU, SetNone},
    {QcomIrisCapIrPeriod, 0, 0x7fffffff, 0, 0, OUT | DYN, SetIrPeriodGen2},
    {QcomIrisCapLtrCount, 0, 2, 0, HFI_PROP_LTR_COUNT, OUT, SetLtrCountGen2},
    {QcomIrisCapUseLtr, 0, 3, 0, HFI_PROP_LTR_USE, IN | DYN, SetUseAndMarkLtr},
    {QcomIrisCapMarkLtr, -1, 1, -1, HFI_PROP_LTR_MARK, IN | DYN, SetUseAndMarkLtr},
    {QcomIrisCapBFrame, 0, 1, 0, HFI_PROP_MAX_B_FRAMES, OUT, SetU32},
    {QcomIrisCapLayerEnable, 0, 1, 0, 0, OUT, SetNone},
    {QcomIrisCapLayerTypeH264, 0, 1, 1, HFI_PROP_LAYER_ENCODING_TYPE, OUT | MENU, SetLayerType},
    {QcomIrisCapLayerTypeHevc, 0, 1, 1, HFI_PROP_LAYER_ENCODING_TYPE, OUT | MENU, SetLayerType},
    {QcomIrisCapLayerCountH264, 0, 5, 0, HFI_PROP_LAYER_COUNT, OUT | DYN, SetLayerCountGen2},
    {QcomIrisCapLayerCountHevc, 0, 5, 0, HFI_PROP_LAYER_COUNT, OUT | DYN, SetLayerCountGen2},
    {QcomIrisCapLayer0BitrateH264, 1, BITRATE_MAX, 20000000, HFI_PROP_BITRATE_LAYER1, OUT | IN | DYN, SetLayerBitrate},
    {QcomIrisCapLayer1BitrateH264, 1, BITRATE_MAX, 20000000, HFI_PROP_BITRATE_LAYER2, OUT | IN | DYN, SetLayerBitrate},
    {QcomIrisCapLayer2BitrateH264, 1, BITRATE_MAX, 20000000, HFI_PROP_BITRATE_LAYER3, OUT | IN | DYN, SetLayerBitrate},
    {QcomIrisCapLayer3BitrateH264, 1, BITRATE_MAX, 20000000, HFI_PROP_BITRATE_LAYER4, OUT | IN | DYN, SetLayerBitrate},
    {QcomIrisCapLayer4BitrateH264, 1, BITRATE_MAX, 20000000, HFI_PROP_BITRATE_LAYER5, OUT | IN | DYN, SetLayerBitrate},
    {QcomIrisCapLayer5BitrateH264, 1, BITRATE_MAX, 20000000, HFI_PROP_BITRATE_LAYER6, OUT | IN | DYN, SetLayerBitrate},
    {QcomIrisCapLayer0BitrateHevc, 1, BITRATE_MAX, 20000000, HFI_PROP_BITRATE_LAYER1, OUT | IN | DYN, SetLayerBitrate},
    {QcomIrisCapLayer1BitrateHevc, 1, BITRATE_MAX, 20000000, HFI_PROP_BITRATE_LAYER2, OUT | IN | DYN, SetLayerBitrate},
    {QcomIrisCapLayer2BitrateHevc, 1, BITRATE_MAX, 20000000, HFI_PROP_BITRATE_LAYER3, OUT | IN | DYN, SetLayerBitrate},
    {QcomIrisCapLayer3BitrateHevc, 1, BITRATE_MAX, 20000000, HFI_PROP_BITRATE_LAYER4, OUT | IN | DYN, SetLayerBitrate},
    {QcomIrisCapLayer4BitrateHevc, 1, BITRATE_MAX, 20000000, HFI_PROP_BITRATE_LAYER5, OUT | IN | DYN, SetLayerBitrate},
    {QcomIrisCapLayer5BitrateHevc, 1, BITRATE_MAX, 20000000, HFI_PROP_BITRATE_LAYER6, OUT | IN | DYN, SetLayerBitrate},
    {QcomIrisCapRequestSyncFrame, 0, 1, 0, HFI_PROP_REQUEST_SYNC_FRAME, IN | DYN, SetReqSyncFrame},
};
#undef OUT
#undef IN
#undef DYN
#undef MENU

/* ---- commands ---- */
static int send_packet(struct qcom_iris_session *s, uint32_t session_id, uint32_t type, uint32_t flags, uint32_t port,
                       uint32_t payload_type, const void *payload, uint32_t payload_size)
{
    struct qcom_iris_hfi_writer w;
    qcom_iris_hfi_begin(&w, s->packet, sizeof s->packet, session_id, (*s->header_id)++);
    qcom_iris_hfi_add(&w, type, flags, payload_type, port, (*s->packet_id)++, payload, payload_size);
    if (!w.ok) return -EINVAL_;
    return s->ops->send(s->ops->context, s->packet, qcom_iris_hfi_size(&w));
}
/* iris_hfi_gen2_packet_session_command / _property */
static int command(struct qcom_iris_session *s, uint32_t type, uint32_t flags, uint32_t port, uint32_t payload_type,
                   const void *payload, uint32_t size)
{
    return send_packet(s, s->session_id, type, flags, port, payload_type, payload, size);
}
static int property(struct qcom_iris_session *s, uint32_t type, uint32_t port, uint32_t payload_type, const void *payload, uint32_t size)
{
    return send_packet(s, s->session_id, type, HFI_HOST_FLAGS_NONE, port, payload_type, payload, size);
}
static int property_u32(struct qcom_iris_session *s, uint32_t type, uint32_t port, uint32_t payload_type, uint32_t value)
{
    return property(s, type, port, payload_type, &value, 4);
}

/* iris_hfi_gen2_get_port: input is the bitstream for a decoder, raw for an encoder. */
static uint32_t port_of(const struct qcom_iris_session *s, bool input)
{
    return s->encoder ? (input ? HFI_PORT_RAW : HFI_PORT_BITSTREAM) : (input ? HFI_PORT_BITSTREAM : HFI_PORT_RAW);
}
/* iris_get_port_info */
static uint32_t cap_port(const struct qcom_iris_session *s, enum qcom_iris_cap id)
{
    const uint32_t f = s->caps[id].flags;
    if (f & QcomIrisCapInputPort) return port_of(s, true);
    if (f & QcomIrisCapOutputPort) return port_of(s, false);
    return HFI_PORT_NONE;
}
static uint32_t cap_value(const struct qcom_iris_session *s, enum qcom_iris_cap id) {return (uint32_t)s->caps[id].value;}
static bool client_set(const struct qcom_iris_session *s, enum qcom_iris_cap id) {return (s->caps[id].flags & QcomIrisCapClientSet) != 0;}
static bool h264(const struct qcom_iris_session *s) {return s->codec == QcomIrisCodecH264;}
static bool output_streaming(const struct qcom_iris_session *s) {return s->state == QcomIrisOutputStreaming || s->state == QcomIrisStreaming;}
static bool input_streaming(const struct qcom_iris_session *s) {return s->state == QcomIrisInputStreaming || s->state == QcomIrisStreaming;}

static bool res_is_less_than(uint32_t w, uint32_t h, uint32_t rw, uint32_t rh)
{
    const uint32_t mbs = ((h + 15) / 16) * ((w + 15) / 16), ref = ((rh + 15) / 16) * ((rw + 15) / 16), side = rw > rh ? rw : rh;
    return mbs < ref && w < side && h < side;
}

static int apply_setter(struct qcom_iris_session *s, enum qcom_iris_cap id)
{
    const struct qcom_iris_cap_value *c = &s->caps[id];
    const uint32_t port = cap_port(s, id);
    switch (c->setter) {
    case SetU32Enum: return property_u32(s, c->hfi_id, port, HFI_PAYLOAD_U32_ENUM, cap_value(s, id));
    case SetU32: return property_u32(s, c->hfi_id, port, HFI_PAYLOAD_U32, cap_value(s, id));
    case SetStage: {
        uint32_t mode = 2;
        if (!s->encoder && res_is_less_than(s->src_width, s->src_height, 1280, 720)) mode = 1;
        return property_u32(s, c->hfi_id, port, HFI_PAYLOAD_U32, mode);
    }
    case SetPipe: return property_u32(s, c->hfi_id, port, HFI_PAYLOAD_U32, cap_value(s, QcomIrisCapPipe));
    case SetProfile: {
        const enum qcom_iris_cap p = h264(s) ? QcomIrisCapProfileH264 : QcomIrisCapProfileHevc;
        return property_u32(s, s->caps[p].hfi_id, port, HFI_PAYLOAD_U32_ENUM, cap_value(s, p));
    }
    case SetLevel: {
        const enum qcom_iris_cap l = h264(s) ? QcomIrisCapLevelH264 : QcomIrisCapLevelHevc;
        return property_u32(s, s->caps[l].hfi_id, port, HFI_PAYLOAD_U32_ENUM, cap_value(s, l));
    }
    case SetHeaderModeGen2: {
        uint32_t v;
        if (cap_value(s, QcomIrisCapPrependSpsPpsToIdr)) v = HFI_SEQ_HEADER_PREFIX_WITH_SYNC_FRAME;
        else if (cap_value(s, id) == 1) v = HFI_SEQ_HEADER_JOINED_WITH_1ST_FRAME;
        else v = HFI_SEQ_HEADER_SEPERATE_FRAME;
        return property_u32(s, c->hfi_id, port, HFI_PAYLOAD_U32_ENUM, v);
    }
    case SetBitrateGen2: {
        uint32_t max_bitrate = cap_value(s, QcomIrisCapEntropyMode) == QcomIrisEntropyCabac ? CABAC_MAX_BITRATE : CAVLC_MAX_BITRATE;
        uint32_t bitrate = cap_value(s, id);
        if (bitrate > max_bitrate) bitrate = max_bitrate;
        return property_u32(s, c->hfi_id, port, HFI_PAYLOAD_U32, bitrate);
    }
    case SetPeakBitrate: {
        if (cap_value(s, QcomIrisCapBitrateMode) != QcomIrisBitrateModeCbr) return 0;
        uint32_t peak = cap_value(s, id), bitrate = cap_value(s, QcomIrisCapBitrate);
        if (client_set(s, id)) {if (peak < bitrate) peak = bitrate;}
        else peak = bitrate;
        s->caps[id].value = peak;
        return property_u32(s, c->hfi_id, port, HFI_PAYLOAD_U32, peak);
    }
    case SetBitrateModeGen2: {
        const uint32_t mode = cap_value(s, QcomIrisCapBitrateMode);
        uint32_t rc = 0;
        if (!cap_value(s, QcomIrisCapFrameRcEnable)) rc = HFI_RC_OFF;
        else if (mode == QcomIrisBitrateModeVbr) rc = HFI_RC_VBR_CFR;
        else if (mode == QcomIrisBitrateModeCbr) rc = cap_value(s, QcomIrisCapFrameSkipMode) ? HFI_RC_CBR_VFR : HFI_RC_CBR_CFR;
        else if (mode == QcomIrisBitrateModeCq) rc = HFI_RC_CQ;
        s->hfi_rc_type = rc;
        return property_u32(s, c->hfi_id, port, HFI_PAYLOAD_U32_ENUM, rc);
    }
    case SetTimeDeltaRc: return property_u32(s, c->hfi_id, port, HFI_PAYLOAD_U32, cap_value(s, id));
    case SetEntropyGen2: {
        if (!h264(s)) return 0;
        uint32_t mode = cap_value(s, id);
        const uint32_t profile = cap_value(s, QcomIrisCapProfileH264);
        if (profile == QcomIrisH264ProfileBaseline || profile == QcomIrisH264ProfileConstrainedBaseline) mode = QcomIrisEntropyCavlc;
        s->caps[id].value = mode;
        return property_u32(s, c->hfi_id, port, HFI_PAYLOAD_U32, mode);
    }
    case SetMinQp: case SetMaxQp: {
        const bool min = c->setter == SetMinQp;
        const enum qcom_iris_cap frame = min ? (h264(s) ? QcomIrisCapMinFrameQpH264 : QcomIrisCapMinFrameQpHevc)
                                             : (h264(s) ? QcomIrisCapMaxFrameQpH264 : QcomIrisCapMaxFrameQpHevc);
        const enum qcom_iris_cap i = min ? (h264(s) ? QcomIrisCapIFrameMinQpH264 : QcomIrisCapIFrameMinQpHevc)
                                         : (h264(s) ? QcomIrisCapIFrameMaxQpH264 : QcomIrisCapIFrameMaxQpHevc);
        const enum qcom_iris_cap p = (enum qcom_iris_cap)(i + 2), b = (enum qcom_iris_cap)(i + 4);
        const bool all = client_set(s, frame);
        const uint32_t enable = (all || client_set(s, i)) | (all || client_set(s, p)) << 1 | (all || client_set(s, b)) << 2;
        if (!enable) return 0;
        uint32_t v[3];
        const enum qcom_iris_cap each[3] = {i, p, b};
        for (unsigned k = 0; k < 3; ++k) {
            const uint32_t own = cap_value(s, each[k]), common = cap_value(s, frame);
            v[k] = min ? (own > common ? own : common) : (own < common ? own : common);
        }
        return property_u32(s, c->hfi_id, port, HFI_PAYLOAD_32_PACKED, v[0] | v[1] << 8 | v[2] << 16 | enable << 24);
    }
    case SetFrameQp: {
        if (output_streaming(s) && s->hfi_rc_type != HFI_RC_OFF) return 0;
        const enum qcom_iris_cap i = h264(s) ? QcomIrisCapIFrameQpH264 : QcomIrisCapIFrameQpHevc;
        const enum qcom_iris_cap p = (enum qcom_iris_cap)(i + 2), b = (enum qcom_iris_cap)(i + 4);
        uint32_t enable;
        if (s->hfi_rc_type == HFI_RC_OFF) enable = 7;
        else enable = client_set(s, i) | client_set(s, p) << 1 | client_set(s, b) << 2;
        if (!enable) return 0;
        return property_u32(s, c->hfi_id, port, HFI_PAYLOAD_32_PACKED,
                            cap_value(s, i) | cap_value(s, p) << 8 | cap_value(s, b) << 16 | enable << 24);
    }
    case SetRotation: {
        uint32_t v;
        switch (cap_value(s, id)) {
        case 0: return 0;
        case 90: v = HFI_ROTATION_90; break;
        case 180: v = HFI_ROTATION_180; break;
        case 270: v = HFI_ROTATION_270; break;
        default: return -EINVAL_;
        }
        return property_u32(s, c->hfi_id, port, HFI_PAYLOAD_U32, v);
    }
    case SetFlip: {
        uint32_t v = HFI_DISABLE_FLIP;
        if (cap_value(s, QcomIrisCapHflip)) v |= HFI_HORIZONTAL_FLIP;
        if (cap_value(s, QcomIrisCapVflip)) v |= HFI_VERTICAL_FLIP;
        return property_u32(s, c->hfi_id, port, HFI_PAYLOAD_U32_ENUM, v);
    }
    case SetIrPeriodGen2: {
        if (output_streaming(s)) return 0;   /* random intra refresh: not changed while streaming */
        return property_u32(s, HFI_PROP_IR_RANDOM_PERIOD, port, HFI_PAYLOAD_U32, cap_value(s, id));
    }
    case SetLtrCountGen2: {
        const uint32_t count = cap_value(s, id);
        if (!count) return -EINVAL_;
        if (s->hfi_rc_type == HFI_RC_CBR_VFR || s->hfi_rc_type == HFI_RC_CBR_CFR || s->hfi_rc_type == HFI_RC_OFF) {
            s->caps[QcomIrisCapLtrCount].value = 0;
            return -EINVAL_;
        }
        return property_u32(s, c->hfi_id, port, HFI_PAYLOAD_U32, count);
    }
    case SetUseAndMarkLtr: {
        if (!input_streaming(s) && !output_streaming(s)) return -EINVAL_;
        if (!cap_value(s, QcomIrisCapLtrCount) || c->value == -1) return -EINVAL_;
        return property_u32(s, c->hfi_id, port, HFI_PAYLOAD_U32, cap_value(s, id));
    }
    case SetLayerType: {
        if (s->hfi_rc_type == HFI_RATE_CONTROL_CQ || s->hfi_rc_type == HFI_RATE_CONTROL_OFF) return -EINVAL_;
        uint32_t type;
        if (h264(s)) {
            if (!cap_value(s, QcomIrisCapLayerEnable) || !cap_value(s, QcomIrisCapLayerCountH264)) return -EINVAL_;
            if (cap_value(s, QcomIrisCapLayerTypeH264) == 1) type = s->hfi_rc_type == HFI_RC_VBR_CFR ? HFI_HIER_P_HYBRID_LTR : HFI_HIER_P_SLIDING_WINDOW;
            else if (s->hfi_rc_type == HFI_RC_VBR_CFR) type = HFI_HIER_B;
            else return -EINVAL_;
        } else {
            if (!cap_value(s, QcomIrisCapLayerCountHevc)) return -EINVAL_;
            if (cap_value(s, QcomIrisCapLayerTypeHevc) == 1) type = HFI_HIER_P_SLIDING_WINDOW;
            else if (s->hfi_rc_type == HFI_RC_VBR_CFR) type = HFI_HIER_B;
            else return -EINVAL_;
        }
        s->hfi_layer_type = type;
        return property_u32(s, c->hfi_id, port, HFI_PAYLOAD_U32_ENUM, type);
    }
    case SetLayerCountGen2: {
        uint32_t count = h264(s) ? cap_value(s, QcomIrisCapLayerCountH264) : cap_value(s, QcomIrisCapLayerCountHevc), max;
        if (!count) return -EINVAL_;
        if (s->hfi_layer_type == HFI_HIER_B) max = 3;
        else if (s->hfi_layer_type == HFI_HIER_P_HYBRID_LTR) max = 5;
        else if (s->hfi_layer_type == HFI_HIER_P_SLIDING_WINDOW) max = h264(s) ? 3 : (s->hfi_rc_type == HFI_RC_VBR_CFR ? 5 : 3);
        else return -EINVAL_;
        if (count > max) count = max;
        count += 1;
        s->hfi_layer_count = count;
        return property_u32(s, c->hfi_id, port, HFI_PAYLOAD_U32, count);
    }
    case SetLayerBitrate: {
        if (client_set(s, QcomIrisCapBitrate)) return 0;
        if (!client_set(s, id)) return -EINVAL_;
        if (!input_streaming(s) && !output_streaming(s)) return -EINVAL_;
        return property_u32(s, c->hfi_id, port, HFI_PAYLOAD_U32, cap_value(s, id));
    }
    case SetReqSyncFrame: {
        const uint32_t v = cap_value(s, QcomIrisCapPrependSpsPpsToIdr) ? HFI_SYNC_FRAME_REQUEST_WITH_PREFIX_SEQ_HDR
                                                                       : HFI_SYNC_FRAME_REQUEST_WITHOUT_SEQ_HDR;
        return property_u32(s, c->hfi_id, port, HFI_PAYLOAD_U32_ENUM, v);
    }
    default: return 0;
    }
}
/* the loop of iris_set_properties after the config params: every capability with a setter, errors ignored as there */
static void apply_caps(struct qcom_iris_session *s)
{
    for (unsigned id = 1; id < QcomIrisCapMax; ++id)
        if (s->caps[id].valid && s->caps[id].setter) (void)apply_setter(s, (enum qcom_iris_cap)id);
}

void qcom_iris_session_init(struct qcom_iris_session *s, const struct qcom_iris_session_ops *ops, uint32_t *header_id,
                            uint32_t *packet_id, uint32_t session_id, bool encoder, enum qcom_iris_codec codec, uint32_t num_vpp_pipe)
{
    __builtin_memset(s, 0, sizeof *s);
    s->ops = ops;
    s->header_id = header_id;
    s->packet_id = packet_id;
    s->session_id = session_id;
    s->encoder = encoder;
    s->codec = codec;
    s->num_vpp_pipe = num_vpp_pipe;
    s->state = QcomIrisDeinit;
    s->frame_rate = 30;
    s->operating_rate = 30;
    const struct cap_def *defs = encoder ? enc_caps : dec_caps;
    const unsigned count = encoder ? sizeof enc_caps / sizeof enc_caps[0] : sizeof dec_caps / sizeof dec_caps[0];
    for (unsigned i = 0; i < count; ++i) {
        struct qcom_iris_cap_value *c = &s->caps[defs[i].id];
        c->min = defs[i].min; c->max = defs[i].max; c->value = defs[i].value;
        c->hfi_id = defs[i].hfi_id; c->flags = defs[i].flags; c->setter = defs[i].setter; c->valid = 1;
    }
    if (!encoder) s->caps[QcomIrisCapPipe].min = s->caps[QcomIrisCapPipe].max = s->caps[QcomIrisCapPipe].value = num_vpp_pipe;
    /* iris_vdec_inst_init / iris_venc_inst_init defaults: 1280x720 */
    s->src_width = s->dst_width = 1280;
    s->src_height = s->dst_height = 720;
    s->output_min_count = 4;
}

/* ---- internal buffers (iris_buffer.c) ---- */
static struct qcom_iris_buffer_query query(const struct qcom_iris_session *s)
{
    struct qcom_iris_buffer_query q = {0};
    q.encoder = s->encoder;
    q.codec = s->codec;
    q.src_width = s->src_width; q.src_height = s->src_height;
    q.dst_width = s->dst_width; q.dst_height = s->dst_height;
    q.crop_width = s->crop_width; q.crop_height = s->crop_height;
    q.fw_min_count = s->fw_min_count;
    q.output_min_count = s->output_min_count;
    q.num_vpp_pipe = s->num_vpp_pipe;
    q.ten_bit = !s->encoder && s->caps[QcomIrisCapBitDepth].value == BIT_DEPTH_10;
    q.max_mbpf = (8192 * 4352) / 256;
    q.stage = cap_value(s, QcomIrisCapStage);
    q.b_frames = cap_value(s, QcomIrisCapBFrame);
    q.ltr_count = cap_value(s, QcomIrisCapLtrCount);
    q.rc_type = s->hfi_rc_type;
    q.layer_type = s->hfi_layer_type;
    q.layer_count = s->hfi_layer_count;
    q.bitrate_mode = cap_value(s, QcomIrisCapBitrateMode);
    q.frame_rc = cap_value(s, QcomIrisCapFrameRcEnable);
    q.rotation = cap_value(s, QcomIrisCapRotation);
    return q;
}

/* iris_hfi_gen2_get_port_from_buf_type */
static uint32_t buffer_port(const struct qcom_iris_session *s, uint32_t type)
{
    if (!s->encoder) {
        switch (type) {
        case QcomIrisBufInput: case QcomIrisBufBin: case QcomIrisBufComv: case QcomIrisBufNonComv: case QcomIrisBufLine:
        case QcomIrisBufPartial: return HFI_PORT_BITSTREAM;
        case QcomIrisBufOutput: case QcomIrisBufDpb: return HFI_PORT_RAW;
        default: return HFI_PORT_NONE;
        }
    }
    switch (type) {
    case QcomIrisBufInput: case QcomIrisBufVpss: return HFI_PORT_RAW;
    case QcomIrisBufOutput: case QcomIrisBufBin: case QcomIrisBufComv: case QcomIrisBufNonComv: case QcomIrisBufLine:
    case QcomIrisBufScratch2: return HFI_PORT_BITSTREAM;
    default: return HFI_PORT_NONE;
    }
}
/* iris_hfi_gen2_buf_type_from_driver */
static uint32_t hfi_buffer_type(const struct qcom_iris_session *s, uint32_t type)
{
    switch (type) {
    case QcomIrisBufInput: return s->encoder ? HFI_BUFFER_RAW : HFI_BUFFER_BITSTREAM;
    case QcomIrisBufOutput: return s->encoder ? HFI_BUFFER_BITSTREAM : HFI_BUFFER_RAW;
    case QcomIrisBufBin: return HFI_BUFFER_BIN;
    case QcomIrisBufComv: return HFI_BUFFER_COMV;
    case QcomIrisBufNonComv: return HFI_BUFFER_NON_COMV;
    case QcomIrisBufLine: return HFI_BUFFER_LINE;
    case QcomIrisBufDpb: case QcomIrisBufScratch2: return HFI_BUFFER_DPB;
    case QcomIrisBufPersist: return HFI_BUFFER_PERSIST;
    case QcomIrisBufArp: return HFI_BUFFER_ARP;
    case QcomIrisBufVpss: return HFI_BUFFER_VPSS;
    case QcomIrisBufPartial: return HFI_BUFFER_PARTIAL_DATA;
    default: return 0;
    }
}
static uint32_t driver_buffer_type(const struct qcom_iris_session *s, uint32_t hfi)
{
    switch (hfi) {
    case HFI_BUFFER_BITSTREAM: return s->encoder ? QcomIrisBufOutput : QcomIrisBufInput;
    case HFI_BUFFER_RAW: return s->encoder ? QcomIrisBufInput : QcomIrisBufOutput;
    case HFI_BUFFER_BIN: return QcomIrisBufBin;
    case HFI_BUFFER_ARP: return QcomIrisBufArp;
    case HFI_BUFFER_COMV: return QcomIrisBufComv;
    case HFI_BUFFER_NON_COMV: return QcomIrisBufNonComv;
    case HFI_BUFFER_LINE: return QcomIrisBufLine;
    case HFI_BUFFER_DPB: return s->encoder ? QcomIrisBufScratch2 : QcomIrisBufDpb;
    case HFI_BUFFER_PERSIST: return QcomIrisBufPersist;
    case HFI_BUFFER_PARTIAL_DATA: return QcomIrisBufPartial;
    case HFI_BUFFER_VPSS: return QcomIrisBufVpss;
    default: return 0;
    }
}

/* iris_hfi_gen2_session_queue_buffer / _release_buffer */
static int queue_buffer(struct qcom_iris_session *s, uint32_t type, uint32_t index, uint64_t addr, uint32_t size, uint32_t data_offset,
                        uint32_t data_size, uint64_t timestamp, uint32_t flags, bool release)
{
    struct qcom_iris_hfi_buffer b = {0};
    b.type = hfi_buffer_type(s, type);
    b.index = index;
    b.base_address = addr;
    b.buffer_size = (!s->encoder && type == QcomIrisBufInput) ? ALIGN_UP(size, 256u) : size;
    b.data_offset = data_offset;
    b.data_size = data_size;
    b.flags = flags | HFI_BUF_HOST_FLAGS_CB_NON_SECURE | (release ? HFI_BUF_HOST_FLAG_RELEASE : 0);
    b.timestamp = timestamp;
    if (type == QcomIrisBufComv && !release) {
        /* iris_hfi_gen2_set_num_comv */
        const uint32_t comv = s->fw_min_count ? s->fw_min_count : s->output_min_count;
        const int r = property_u32(s, HFI_PROP_COMV_BUFFER_COUNT, HFI_PORT_BITSTREAM, HFI_PAYLOAD_U32, comv);
        if (r) return r;
    }
    return command(s, HFI_CMD_BUFFER, release ? (HFI_HOST_FLAGS_RESPONSE_REQUIRED | HFI_HOST_FLAGS_INTR_REQUIRED) : HFI_HOST_FLAGS_INTR_REQUIRED,
                   buffer_port(s, type), HFI_PAYLOAD_STRUCTURE, &b, sizeof b);
}

static int create_internal(struct qcom_iris_session *s, uint32_t type)
{
    const struct qcom_iris_buffer_query q = query(s);
    const uint32_t size = qcom_iris_buffer_size(&q, (enum qcom_iris_buffer)type), count = qcom_iris_buffer_count(&q, (enum qcom_iris_buffer)type);
    if (!size) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        struct qcom_iris_internal *b = NULL;
        for (unsigned k = 0; k < QcomIrisInternalMax && !b; ++k) if (!s->internal[k].type) b = &s->internal[k];
        if (!b) return -ENOSPC_;
        if (s->ops->allocate(s->ops->context, size, &b->device_addr, &b->handle)) return -ENOMEM_;
        b->type = type; b->index = i; b->size = size; b->queued = false; b->pending_release = false;
    }
    return 0;
}
static int queue_internal(struct qcom_iris_session *s, uint32_t type)
{
    for (unsigned k = 0; k < QcomIrisInternalMax; ++k) {
        struct qcom_iris_internal *b = &s->internal[k];
        if (b->type != type || b->queued || b->pending_release) continue;
        if (type == QcomIrisBufDpb && s->state != QcomIrisStreaming) continue;   /* deferred until both sides stream */
        const int r = queue_buffer(s, type, b->index, b->device_addr, b->size, 0, 0, 0, 0, false);
        if (r) return r;
        b->queued = true;
    }
    return 0;
}
static void destroy_internal(struct qcom_iris_session *s, struct qcom_iris_internal *b)
{
    if (b->handle) s->ops->release(s->ops->context, b->handle);
    __builtin_memset(b, 0, sizeof *b);
}
/* iris_release_internal_buffers: queued ones asked back; the firmware's release-done destroys them. */
static int release_internal(struct qcom_iris_session *s, uint32_t type)
{
    for (unsigned k = 0; k < QcomIrisInternalMax; ++k) {
        struct qcom_iris_internal *b = &s->internal[k];
        if (b->type != type || b->pending_release || !b->queued) continue;
        b->pending_release = true;
        const int r = queue_buffer(s, type, b->index, b->device_addr, b->size, 0, 0, 0, 0, true);
        if (r) {b->pending_release = false; return r;}
    }
    return 0;
}
/* iris_destroy_dequeued_internal_buffers */
static void destroy_unqueued(struct qcom_iris_session *s, const uint32_t *types, unsigned n)
{
    for (unsigned k = 0; k < QcomIrisInternalMax; ++k)
        for (unsigned t = 0; t < n; ++t)
            if (s->internal[k].type == types[t] && !s->internal[k].queued) destroy_internal(s, &s->internal[k]);
}

static const uint32_t dec_input_internal[] = {QcomIrisBufBin, QcomIrisBufComv, QcomIrisBufNonComv, QcomIrisBufLine, QcomIrisBufPartial};
static const uint32_t dec_output_internal[] = {QcomIrisBufDpb};
static const uint32_t enc_input_internal[] = {QcomIrisBufVpss};
static const uint32_t enc_output_internal[] = {QcomIrisBufBin, QcomIrisBufComv, QcomIrisBufNonComv, QcomIrisBufLine, QcomIrisBufScratch2};
#define COUNT(a) (sizeof (a) / sizeof (a)[0])

static int create_and_queue(struct qcom_iris_session *s, const uint32_t *types, unsigned n)
{
    destroy_unqueued(s, types, n);
    for (unsigned t = 0; t < n; ++t) {const int r = create_internal(s, types[t]); if (r) return r;}
    for (unsigned t = 0; t < n; ++t) {const int r = queue_internal(s, types[t]); if (r) return r;}
    return 0;
}
/* iris_alloc_and_queue_persist_bufs */
static int persist(struct qcom_iris_session *s, uint32_t type)
{
    for (unsigned k = 0; k < QcomIrisInternalMax; ++k) if (s->internal[k].type == type) return 0;
    int r = create_internal(s, type);
    if (!r) r = queue_internal(s, type);
    return r;
}

/* ---- config params (iris_hfi_gen2_session_set_config_params) ---- */
static uint32_t codec_align(const struct qcom_iris_session *s) {return s->codec == QcomIrisCodecHevc ? 32 : 16;}
static uint32_t color_info(const struct qcom_iris_session *s)
{
    uint32_t primaries = HFI_PRIMARIES_RESERVED, transfer = HFI_TRANSFER_RESERVED, matrix = HFI_MATRIX_COEFF_RESERVED;
    uint32_t described = 0, signal = 0, full = 0;
    if (s->color_described) {
        described = 1; signal = 1;
        primaries = s->color_primaries; transfer = s->transfer; matrix = s->matrix;
        full = s->full_range;
    }
    return (matrix & 0xff) | ((transfer << 8) & 0xff00) | ((primaries << 16) & 0xff0000) | ((described << 24) & 0x1000000) |
           ((full << 25) & 0x2000000) | ((UNSPECIFIED_COLOR_FORMAT << 26) & 0x1c000000) | ((signal << 29) & 0x20000000);
}
static int config_param(struct qcom_iris_session *s, uint32_t type, bool input)
{
    const uint32_t port = port_of(s, input);
    switch (type) {
    case HFI_PROP_RAW_RESOLUTION:
        return property_u32(s, type, port, HFI_PAYLOAD_32_PACKED, s->enc_raw_width << 16 | s->enc_raw_height);
    case HFI_PROP_BITSTREAM_RESOLUTION: {
        uint32_t w, h;
        if (!s->encoder) {w = s->src_width; h = s->src_height;}
        else {w = s->crop_width; h = s->crop_height;}   /* enc_scale_width/height: no rotation here */
        const uint32_t r = ALIGN_UP(w, codec_align(s)) << 16 | ALIGN_UP(h, codec_align(s));
        if (!s->encoder) s->src_subcr.bitstream_resolution = r;
        return property_u32(s, type, port, s->encoder ? HFI_PAYLOAD_32_PACKED : HFI_PAYLOAD_U32, r);
    }
    case HFI_PROP_CROP_OFFSETS: {
        uint32_t left = 0, top = 0, right, bottom;
        if (!s->encoder) {
            if (input) {bottom = s->src_height - s->crop_height; right = s->src_width - s->crop_width; left = s->crop_left; top = s->crop_top;}
            else {bottom = s->dst_height - s->crop_height; right = s->dst_width - s->crop_width; left = s->crop_left; top = s->crop_top;}
        } else if (input) {
            bottom = s->enc_raw_height - s->crop_height; right = s->enc_raw_width - s->crop_width; left = s->crop_left; top = s->crop_top;
        } else {
            bottom = ALIGN_UP(s->crop_height, codec_align(s)) - s->crop_height; right = ALIGN_UP(s->crop_width, codec_align(s)) - s->crop_width;
        }
        uint32_t payload[2] = {left << 16 | top, right << 16 | bottom};
        s->src_subcr.crop_offsets[0] = payload[0];
        s->src_subcr.crop_offsets[1] = payload[1];
        return property(s, type, port, HFI_PAYLOAD_64_PACKED, payload, 8);
    }
    case HFI_PROP_CODED_FRAMES: s->src_subcr.coded_frames = HFI_BITMASK_FRAME_MBS_ONLY_FLAG;
        return property_u32(s, type, port, HFI_PAYLOAD_U32, HFI_BITMASK_FRAME_MBS_ONLY_FLAG);
    case HFI_PROP_LUMA_CHROMA_BIT_DEPTH: s->src_subcr.bit_depth = BIT_DEPTH_8;
        return property_u32(s, type, port, HFI_PAYLOAD_U32, BIT_DEPTH_8);
    case HFI_PROP_BUFFER_FW_MIN_OUTPUT_COUNT: s->src_subcr.fw_min_count = s->output_min_count;
        return property_u32(s, type, port, HFI_PAYLOAD_U32, s->output_min_count);
    case HFI_PROP_PIC_ORDER_CNT_TYPE: s->src_subcr.pic_order_cnt = 0;
        return property_u32(s, type, port, HFI_PAYLOAD_U32, 0);
    case HFI_PROP_SIGNAL_COLOR_INFO: s->src_subcr.color_info = color_info(s);
        return property_u32(s, type, port, HFI_PAYLOAD_32_PACKED, s->src_subcr.color_info);
    case HFI_PROP_PROFILE: case HFI_PROP_LEVEL: {
        enum qcom_iris_cap id;
        const bool profile = type == HFI_PROP_PROFILE;
        switch (s->codec) {
        case QcomIrisCodecHevc: id = profile ? QcomIrisCapProfileHevc : QcomIrisCapLevelHevc; break;
        case QcomIrisCodecVp9: id = profile ? QcomIrisCapProfileVp9 : QcomIrisCapLevelVp9; break;
        default: id = profile ? QcomIrisCapProfileH264 : QcomIrisCapLevelH264; break;
        }
        if (profile) s->src_subcr.profile = cap_value(s, id); else s->src_subcr.level = cap_value(s, id);
        /* iris_hfi_gen2_set_profile / _set_level: the port of the OUTPUT (input) queue */
        return property_u32(s, type, port_of(s, true), HFI_PAYLOAD_U32_ENUM, cap_value(s, id));
    }
    case HFI_PROP_TIER: s->src_subcr.tier = cap_value(s, QcomIrisCapTier);
        return property_u32(s, type, port_of(s, true), HFI_PAYLOAD_U32_ENUM, s->src_subcr.tier);
    case HFI_PROP_OPB_ENABLE: return property_u32(s, type, port, HFI_PAYLOAD_U32, 1);   /* NV12 output: split mode */
    case HFI_PROP_COLOR_FORMAT:   /* iris_hfi_gen2_set_colorformat: NV12, or P010 for a 10-bit stream (linear, as NV12) */
        return property_u32(s, type, port, HFI_PAYLOAD_U32_ENUM,
                            !s->encoder && s->caps[QcomIrisCapBitDepth].value == BIT_DEPTH_10 ? HFI_COLOR_FMT_P010 : HFI_COLOR_FMT_NV12);
    case HFI_PROP_LINEAR_STRIDE_SCANLINE: {
        uint32_t stride, scanline;
        if (!s->encoder) {stride = s->dst_width; scanline = s->dst_height;}
        else {stride = s->enc_stride; scanline = s->enc_scanline;}
        uint32_t payload[2] = {stride << 16 | scanline, stride << 16 | scanline / 2};
        return property(s, type, port, HFI_PAYLOAD_64_PACKED, payload, 8);
    }
    case HFI_PROP_UBWC_STRIDE_SCANLINE: return 0;   /* AV1 only */
    case HFI_PROP_FRAME_RATE: return property_u32(s, type, port_of(s, false), HFI_PAYLOAD_Q16, s->frame_rate << 16);
    default: return 0;
    }
}
/* sm8550 config parameter lists (iris_hfi_gen2.c) */
static const uint32_t dec_in_default[] = {HFI_PROP_BITSTREAM_RESOLUTION, HFI_PROP_CROP_OFFSETS, HFI_PROP_LUMA_CHROMA_BIT_DEPTH,
    HFI_PROP_CODED_FRAMES, HFI_PROP_BUFFER_FW_MIN_OUTPUT_COUNT, HFI_PROP_PIC_ORDER_CNT_TYPE, HFI_PROP_PROFILE, HFI_PROP_LEVEL,
    HFI_PROP_SIGNAL_COLOR_INFO};
static const uint32_t dec_in_hevc[] = {HFI_PROP_BITSTREAM_RESOLUTION, HFI_PROP_CROP_OFFSETS, HFI_PROP_LUMA_CHROMA_BIT_DEPTH,
    HFI_PROP_BUFFER_FW_MIN_OUTPUT_COUNT, HFI_PROP_PROFILE, HFI_PROP_LEVEL, HFI_PROP_TIER, HFI_PROP_SIGNAL_COLOR_INFO};
static const uint32_t dec_in_vp9[] = {HFI_PROP_BITSTREAM_RESOLUTION, HFI_PROP_CROP_OFFSETS, HFI_PROP_LUMA_CHROMA_BIT_DEPTH,
    HFI_PROP_BUFFER_FW_MIN_OUTPUT_COUNT, HFI_PROP_PROFILE, HFI_PROP_LEVEL};
static const uint32_t dec_out[] = {HFI_PROP_OPB_ENABLE, HFI_PROP_COLOR_FORMAT, HFI_PROP_LINEAR_STRIDE_SCANLINE, HFI_PROP_UBWC_STRIDE_SCANLINE};
static const uint32_t enc_in[] = {HFI_PROP_COLOR_FORMAT, HFI_PROP_RAW_RESOLUTION, HFI_PROP_CROP_OFFSETS, HFI_PROP_LINEAR_STRIDE_SCANLINE,
    HFI_PROP_SIGNAL_COLOR_INFO};
static const uint32_t enc_out[] = {HFI_PROP_BITSTREAM_RESOLUTION, HFI_PROP_CROP_OFFSETS, HFI_PROP_FRAME_RATE};
static const uint32_t dec_sub_in[] = {HFI_PROP_NO_OUTPUT};
static const uint32_t dec_sub_out_avc[] = {HFI_PROP_PICTURE_TYPE, HFI_PROP_CABAC_SESSION};
static const uint32_t dec_sub_out_other[] = {HFI_PROP_PICTURE_TYPE};

static void input_params(const struct qcom_iris_session *s, const uint32_t **list, unsigned *n)
{
    if (s->encoder) {*list = enc_in; *n = COUNT(enc_in); return;}
    switch (s->codec) {
    case QcomIrisCodecHevc: *list = dec_in_hevc; *n = COUNT(dec_in_hevc); break;
    case QcomIrisCodecVp9: *list = dec_in_vp9; *n = COUNT(dec_in_vp9); break;
    default: *list = dec_in_default; *n = COUNT(dec_in_default); break;
    }
}
static int config_params(struct qcom_iris_session *s, bool input)
{
    const uint32_t *list;
    unsigned n;
    if (input) input_params(s, &list, &n);
    else if (s->encoder) {list = enc_out; n = COUNT(enc_out);}
    else {list = dec_out; n = COUNT(dec_out);}
    for (unsigned i = 0; i < n; ++i) {const int r = config_param(s, list[i], input); if (r) return r;}
    return 0;
}
/* iris_set_properties */
static int set_properties(struct qcom_iris_session *s, bool input)
{
    const int r = config_params(s, input);
    if (r) return r;
    apply_caps(s);
    return 0;
}

/* iris_hfi_gen2_subscribe_change_param (decoder only) */
static int subscribe_change_param(struct qcom_iris_session *s, bool input)
{
    if (s->encoder) return 0;
    if ((input && s->ipsc_properties_set) || (!input && s->opsc_properties_set)) return 0;
    const uint32_t *list;
    unsigned n;
    input_params(s, &list, &n);
    uint32_t payload[32] = {HFI_MODE_PORT_SETTINGS_CHANGE};
    for (unsigned i = 0; i < n; ++i) payload[i + 1] = list[i];
    int r = command(s, HFI_CMD_SUBSCRIBE_MODE, HFI_HOST_FLAGS_RESPONSE_REQUIRED | HFI_HOST_FLAGS_INTR_REQUIRED, port_of(s, input),
                    HFI_PAYLOAD_U32_ARRAY, payload, (n + 1) * 4);
    if (r) return r;
    if (input) {s->ipsc_properties_set = true; return 0;}
    s->dst_subcr = s->src_subcr;
    const struct qcom_iris_subscription *p = &s->dst_subcr;
    const uint32_t raw = port_of(s, false);
    for (unsigned i = 0; i < n; ++i) {
        uint32_t v[2] = {0, 0}, size = 4;
        switch (list[i]) {
        case HFI_PROP_BITSTREAM_RESOLUTION: v[0] = p->bitstream_resolution; break;
        case HFI_PROP_CROP_OFFSETS: v[0] = p->crop_offsets[0]; v[1] = p->crop_offsets[1]; size = 8; break;
        case HFI_PROP_CODED_FRAMES: v[0] = p->coded_frames; break;
        case HFI_PROP_LUMA_CHROMA_BIT_DEPTH: v[0] = p->bit_depth; break;
        case HFI_PROP_BUFFER_FW_MIN_OUTPUT_COUNT: v[0] = p->fw_min_count; break;
        case HFI_PROP_PIC_ORDER_CNT_TYPE: v[0] = p->pic_order_cnt; break;
        case HFI_PROP_SIGNAL_COLOR_INFO: v[0] = p->color_info; break;
        case HFI_PROP_PROFILE: v[0] = p->profile; break;
        case HFI_PROP_LEVEL: v[0] = p->level; break;
        case HFI_PROP_TIER: v[0] = p->tier; break;
        default: continue;
        }
        r = property(s, list[i], raw, size == 8 ? HFI_PAYLOAD_64_PACKED : HFI_PAYLOAD_U32, v, size);
        if (r) return r;
    }
    s->opsc_properties_set = true;
    return 0;
}
/* iris_hfi_gen2_subscribe_property (decoder only) */
static int subscribe_property(struct qcom_iris_session *s, bool input)
{
    if (s->encoder) return 0;
    const uint32_t *list;
    unsigned n;
    if (input) {list = dec_sub_in; n = COUNT(dec_sub_in);}
    else if (s->codec == QcomIrisCodecH264) {list = dec_sub_out_avc; n = COUNT(dec_sub_out_avc);}
    else {list = dec_sub_out_other; n = COUNT(dec_sub_out_other);}
    uint32_t payload[8] = {HFI_MODE_PROPERTY};
    for (unsigned i = 0; i < n; ++i) payload[i + 1] = list[i];
    return command(s, HFI_CMD_SUBSCRIBE_MODE, HFI_HOST_FLAGS_RESPONSE_REQUIRED | HFI_HOST_FLAGS_INTR_REQUIRED, port_of(s, input),
                   HFI_PAYLOAD_U32_ARRAY, payload, (n + 1) * 4);
}
/* iris_hfi_gen2_session_start */
static int session_start(struct qcom_iris_session *s, bool input)
{
    int r = subscribe_change_param(s, input);
    if (!r) r = subscribe_property(s, input);
    if (!r) r = command(s, HFI_CMD_START, HFI_HOST_FLAGS_RESPONSE_REQUIRED | HFI_HOST_FLAGS_INTR_REQUIRED, port_of(s, input), HFI_PAYLOAD_NONE,
                        NULL, 0);
    return r;
}
static int resume(struct qcom_iris_session *s, bool input, uint32_t what)
{
    return command(s, HFI_CMD_RESUME, HFI_HOST_FLAGS_RESPONSE_REQUIRED | HFI_HOST_FLAGS_INTR_REQUIRED, port_of(s, input), HFI_PAYLOAD_U32, &what, 4);
}
static bool change_sub_state(struct qcom_iris_session *s, uint32_t clear, uint32_t set)
{
    if (s->state == QcomIrisError) return true;
    s->sub_state = (s->sub_state | set) & ~clear;
    return true;
}
/* iris_inst_state_change_streamon / _streamoff */
static void streamon(struct qcom_iris_session *s, bool input)
{
    if (input) s->state = s->state == QcomIrisInit ? QcomIrisInputStreaming : s->state == QcomIrisOutputStreaming ? QcomIrisStreaming : QcomIrisError;
    else s->state = s->state == QcomIrisInit ? QcomIrisOutputStreaming : s->state == QcomIrisInputStreaming ? QcomIrisStreaming : QcomIrisError;
}
static void streamoff(struct qcom_iris_session *s, bool input)
{
    if (input) s->state = s->state == QcomIrisInputStreaming ? QcomIrisInit : s->state == QcomIrisStreaming ? QcomIrisOutputStreaming : QcomIrisError;
    else s->state = s->state == QcomIrisOutputStreaming ? QcomIrisInit : s->state == QcomIrisStreaming ? QcomIrisInputStreaming : QcomIrisError;
}

int qcom_iris_session_open(struct qcom_iris_session *s)
{
    if (s->state != QcomIrisDeinit) return -EINVAL_;
    s->ipsc_properties_set = s->opsc_properties_set = false;
    int r = send_packet(s, 0, HFI_CMD_OPEN, HFI_HOST_FLAGS_RESPONSE_REQUIRED | HFI_HOST_FLAGS_INTR_REQUIRED, HFI_PORT_NONE, HFI_PAYLOAD_U32,
                        &s->session_id, 4);
    if (r) return r;
    uint32_t codec;
    switch (s->codec) {
    case QcomIrisCodecHevc: codec = s->encoder ? HFI_CODEC_ENCODE_HEVC : HFI_CODEC_DECODE_HEVC; break;
    case QcomIrisCodecVp9: codec = HFI_CODEC_DECODE_VP9; break;
    default: codec = s->encoder ? HFI_CODEC_ENCODE_AVC : HFI_CODEC_DECODE_AVC; break;
    }
    r = property_u32(s, HFI_PROP_CODEC, HFI_PORT_NONE, HFI_PAYLOAD_U32_ENUM, codec);
    if (!r && !s->encoder) r = property_u32(s, HFI_PROP_DEC_DEFAULT_HEADER, HFI_PORT_BITSTREAM, HFI_PAYLOAD_U32, 0);
    if (!r) s->state = QcomIrisInit;
    return r;
}

int qcom_iris_session_close(struct qcom_iris_session *s)
{
    int r = 0;
    if (s->state != QcomIrisDeinit) {
        s->responded = false;
        r = command(s, HFI_CMD_CLOSE, HFI_HOST_FLAGS_RESPONSE_REQUIRED | HFI_HOST_FLAGS_INTR_REQUIRED | HFI_HOST_FLAGS_NON_DISCARDABLE,
                    HFI_PORT_NONE, HFI_PAYLOAD_NONE, NULL, 0);
        if (!r && !s->ops->wait(s->ops->context, s, HW_RESPONSE_TIMEOUT_MS)) r = -ETIMEDOUT_;
    }
    for (unsigned k = 0; k < QcomIrisInternalMax; ++k) if (s->internal[k].type) destroy_internal(s, &s->internal[k]);
    s->state = QcomIrisDeinit;
    return r;
}

int qcom_iris_decoder_set_format(struct qcom_iris_session *s, uint32_t w, uint32_t h)
{
    if (s->encoder || !w || !h) return -EINVAL_;
    s->src_width = w; s->src_height = h;
    s->crop_left = s->crop_top = 0; s->crop_width = w; s->crop_height = h;
    s->dst_width = ALIGN_UP(w, 128u); s->dst_height = ALIGN_UP(h, 32u);
    s->output_min_count = qcom_iris_buffer_count(&(struct qcom_iris_buffer_query){.codec = s->codec, .dst_width = s->dst_width,
        .dst_height = s->dst_height}, QcomIrisBufOutput);
    return 0;
}

/* iris_vdec_streamon_input + iris_process_streamon_input */
int qcom_iris_decoder_start_input(struct qcom_iris_session *s)
{
    if (s->encoder || s->state != QcomIrisInit) return -EINVAL_;
    int r = set_properties(s, true);
    if (!r) r = persist(s, QcomIrisBufPersist);
    if (!r) r = create_and_queue(s, dec_input_internal, COUNT(dec_input_internal));
    if (!r) r = session_start(s, true);
    if (r) return r;
    uint32_t set = 0;
    if (s->sub_state & QcomIrisSubInputPause) change_sub_state(s, QcomIrisSubInputPause, 0);
    if ((s->sub_state & (QcomIrisSubDrc | QcomIrisSubDrain | QcomIrisSubFirstIpsc)) && !(s->sub_state & QcomIrisSubInputPause)) {
        r = command(s, HFI_CMD_PAUSE, HFI_HOST_FLAGS_RESPONSE_REQUIRED | HFI_HOST_FLAGS_INTR_REQUIRED, port_of(s, true), HFI_PAYLOAD_NONE, NULL, 0);
        if (r) return r;
        set = QcomIrisSubInputPause;
    }
    streamon(s, true);
    change_sub_state(s, 0, set);
    return s->state == QcomIrisError ? -EINVAL_ : 0;
}

/* iris_vdec_streamon_output + iris_process_streamon_output */
int qcom_iris_decoder_start_output(struct qcom_iris_session *s, uint32_t output_width, uint32_t output_height)
{
    if (s->encoder || (s->state != QcomIrisInputStreaming && s->state != QcomIrisInit)) return -EINVAL_;
    if (output_width) s->dst_width = output_width;
    if (output_height) s->dst_height = output_height;
    int r = config_params(s, false);
    if (r) return r;
    destroy_unqueued(s, dec_output_internal, COUNT(dec_output_internal));
    r = create_internal(s, QcomIrisBufDpb);
    if (r) return r;
    const bool first_ipsc = s->sub_state & QcomIrisSubFirstIpsc;
    const bool drain_active = (s->sub_state & QcomIrisSubDrain) && (s->sub_state & QcomIrisSubDrainLast);
    const bool drc_active = (s->sub_state & QcomIrisSubDrc) && (s->sub_state & QcomIrisSubDrcLast);
    uint32_t clear = 0;
    if (drc_active) clear = QcomIrisSubDrc | QcomIrisSubDrcLast;
    else if (drain_active) clear = QcomIrisSubDrain | QcomIrisSubDrainLast;
    if (first_ipsc || drc_active) {
        /* iris_alloc_and_queue_input_int_bufs at the stream's size, then stage and pipe */
        for (unsigned t = 0; t < COUNT(dec_input_internal); ++t) {r = release_internal(s, dec_input_internal[t]); if (r) return r;}
        for (unsigned t = 0; t < COUNT(dec_input_internal); ++t) {r = create_internal(s, dec_input_internal[t]); if (r) return r;}
        for (unsigned t = 0; t < COUNT(dec_input_internal); ++t) {r = queue_internal(s, dec_input_internal[t]); if (r) return r;}
        (void)apply_setter(s, QcomIrisCapStage);
        (void)apply_setter(s, QcomIrisCapPipe);
    }
    if (s->state == QcomIrisInputStreaming && (s->sub_state & QcomIrisSubInputPause)) {
        r = resume(s, true, drain_active ? HFI_CMD_DRAIN : HFI_CMD_SETTINGS_CHANGE);
        if (r) return r;
        clear |= QcomIrisSubInputPause;
    }
    if (s->sub_state & QcomIrisSubFirstIpsc) clear |= QcomIrisSubFirstIpsc;
    r = session_start(s, false);
    if (r) return r;
    if (s->sub_state & QcomIrisSubOutputPause) clear |= QcomIrisSubOutputPause;
    streamon(s, false);
    change_sub_state(s, clear, 0);
    if (s->state == QcomIrisStreaming) r = queue_internal(s, QcomIrisBufDpb);
    return r;
}

int qcom_iris_decoder_drain(struct qcom_iris_session *s)
{
    if (s->encoder || !input_streaming(s) || (s->sub_state & QcomIrisSubDrain)) return -EINVAL_;
    const int r = command(s, HFI_CMD_DRAIN, HFI_HOST_FLAGS_RESPONSE_REQUIRED | HFI_HOST_FLAGS_INTR_REQUIRED | HFI_HOST_FLAGS_NON_DISCARDABLE,
                          port_of(s, true), HFI_PAYLOAD_NONE, NULL, 0);
    if (!r) change_sub_state(s, 0, QcomIrisSubDrain);
    return r;
}

/* iris_vdec_start_cmd / iris_venc_start_cmd */
static int start_cmd(struct qcom_iris_session *s)
{
    uint32_t clear;
    int r;
    if ((s->sub_state & QcomIrisSubDrc) && (s->sub_state & QcomIrisSubDrcLast)) {
        clear = QcomIrisSubDrc | QcomIrisSubDrcLast;
        if (s->sub_state & QcomIrisSubInputPause) {r = resume(s, true, HFI_CMD_SETTINGS_CHANGE); if (r) return r; clear |= QcomIrisSubInputPause;}
        if (s->sub_state & QcomIrisSubOutputPause) {r = resume(s, false, HFI_CMD_SETTINGS_CHANGE); if (r) return r; clear |= QcomIrisSubOutputPause;}
    } else if ((s->sub_state & QcomIrisSubDrain) && (s->sub_state & QcomIrisSubDrainLast)) {
        clear = QcomIrisSubDrain | QcomIrisSubDrainLast;
        if (s->sub_state & QcomIrisSubInputPause) {r = resume(s, true, HFI_CMD_DRAIN); if (r) return r; clear |= QcomIrisSubInputPause;}
        if (s->sub_state & QcomIrisSubOutputPause) {r = resume(s, false, HFI_CMD_DRAIN); if (r) return r; clear |= QcomIrisSubOutputPause;}
    } else {
        s->state = QcomIrisError;
        return -EINVAL_;
    }
    change_sub_state(s, clear, 0);
    return 0;
}
int qcom_iris_decoder_resume(struct qcom_iris_session *s) {return s->encoder ? -EINVAL_ : start_cmd(s);}

int qcom_iris_encoder_set_format(struct qcom_iris_session *s, uint32_t w, uint32_t h, uint32_t stride, uint32_t scanline, uint32_t frame_rate)
{
    if (!s->encoder || !w || !h || (stride & 127) || stride < ALIGN_UP(w, 128u) || (scanline & 31) || scanline < ALIGN_UP(h, 32u)) return -EINVAL_;
    s->enc_raw_width = w; s->enc_raw_height = h;
    s->enc_stride = stride; s->enc_scanline = scanline;
    s->src_width = ALIGN_UP(w, 128u); s->src_height = ALIGN_UP(h, 32u);
    s->crop_left = s->crop_top = 0; s->crop_width = w; s->crop_height = h;
    s->dst_width = ALIGN_UP(w, codec_align(s)); s->dst_height = ALIGN_UP(h, codec_align(s));
    if (frame_rate) s->frame_rate = frame_rate;
    return 0;
}

/* iris_venc_streamon_input */
int qcom_iris_encoder_start_input(struct qcom_iris_session *s)
{
    if (!s->encoder || (s->state != QcomIrisInit && s->state != QcomIrisOutputStreaming)) return -EINVAL_;
    int r = set_properties(s, true);
    if (!r) r = persist(s, QcomIrisBufArp);
    if (!r) r = create_and_queue(s, enc_input_internal, COUNT(enc_input_internal));
    if (!r) r = session_start(s, true);
    if (r) return r;
    if (s->sub_state & QcomIrisSubInputPause) change_sub_state(s, QcomIrisSubInputPause, 0);
    streamon(s, true);
    return s->state == QcomIrisError ? -EINVAL_ : 0;
}
/* iris_venc_streamon_output */
int qcom_iris_encoder_start_output(struct qcom_iris_session *s)
{
    if (!s->encoder || (s->state != QcomIrisInit && s->state != QcomIrisInputStreaming)) return -EINVAL_;
    int r = set_properties(s, false);
    if (!r) r = persist(s, QcomIrisBufArp);
    if (!r) r = create_and_queue(s, enc_output_internal, COUNT(enc_output_internal));
    if (!r) r = session_start(s, false);
    if (r) return r;
    uint32_t clear = 0;
    if (s->sub_state & QcomIrisSubOutputPause) clear = QcomIrisSubOutputPause;
    streamon(s, false);
    change_sub_state(s, clear, 0);
    return s->state == QcomIrisError ? -EINVAL_ : 0;
}
int qcom_iris_encoder_drain(struct qcom_iris_session *s)
{
    if (!s->encoder || !input_streaming(s) || (s->sub_state & QcomIrisSubDrain)) return -EINVAL_;
    const int r = command(s, HFI_CMD_DRAIN, HFI_HOST_FLAGS_RESPONSE_REQUIRED | HFI_HOST_FLAGS_INTR_REQUIRED | HFI_HOST_FLAGS_NON_DISCARDABLE,
                          port_of(s, true), HFI_PAYLOAD_NONE, NULL, 0);
    if (!r) change_sub_state(s, 0, QcomIrisSubDrain);
    return r;
}
int qcom_iris_encoder_resume(struct qcom_iris_session *s) {return s->encoder ? start_cmd(s) : -EINVAL_;}

/* iris_op_s_ctrl: the value kept as the client's; a dynamic one is sent at once while the session streams. */
int qcom_iris_session_set_cap(struct qcom_iris_session *s, enum qcom_iris_cap id, int64_t value)
{
    if ((unsigned)id >= QcomIrisCapMax || !s->caps[id].valid || value < s->caps[id].min || value > s->caps[id].max) return -EINVAL_;
    s->caps[id].value = value;
    s->caps[id].flags |= QcomIrisCapClientSet;
    if ((input_streaming(s) || output_streaming(s)) && (s->caps[id].flags & QcomIrisCapDynamic) && s->caps[id].setter)
        return apply_setter(s, id);
    return 0;
}
int qcom_iris_encoder_request_sync_frame(struct qcom_iris_session *s)
{
    if (!s->encoder || !s->caps[QcomIrisCapRequestSyncFrame].valid) return -EINVAL_;
    return apply_setter(s, QcomIrisCapRequestSyncFrame);
}

int qcom_iris_session_queue_input(struct qcom_iris_session *s, uint32_t index, uint64_t addr, uint32_t size, uint32_t data_offset,
                                  uint32_t data_size, uint64_t timestamp, uint32_t flags)
{
    if (!input_streaming(s)) return -EINVAL_;
    return queue_buffer(s, QcomIrisBufInput, index, addr, size, data_offset, data_size, timestamp, flags, false);
}
int qcom_iris_session_queue_output(struct qcom_iris_session *s, uint32_t index, uint64_t addr, uint32_t size)
{
    if (!output_streaming(s)) return -EINVAL_;
    return queue_buffer(s, QcomIrisBufOutput, index, addr, size, 0, 0, 0, 0, false);
}

/* iris_hfi_gen2_session_stop + iris_session_streamoff */
int qcom_iris_session_stop(struct qcom_iris_session *s, bool input)
{
    if (input ? !input_streaming(s) : !output_streaming(s)) return -EINVAL_;
    s->responded = false;
    int r = command(s, HFI_CMD_STOP, HFI_HOST_FLAGS_RESPONSE_REQUIRED | HFI_HOST_FLAGS_INTR_REQUIRED | HFI_HOST_FLAGS_NON_DISCARDABLE,
                    port_of(s, input), HFI_PAYLOAD_NONE, NULL, 0);
    if (!r && !s->ops->wait(s->ops->context, s, HW_RESPONSE_TIMEOUT_MS)) r = -ETIMEDOUT_;
    if (r) {
        /* iris_kill_session */
        command(s, HFI_CMD_CLOSE, HFI_HOST_FLAGS_RESPONSE_REQUIRED | HFI_HOST_FLAGS_INTR_REQUIRED | HFI_HOST_FLAGS_NON_DISCARDABLE,
                HFI_PORT_NONE, HFI_PAYLOAD_NONE, NULL, 0);
        s->state = QcomIrisError;
        return r;
    }
    streamoff(s, input);
    return 0;
}

/* ---- messages (iris_hfi_gen2_response.c) ---- */
static void fail(struct qcom_iris_session *s, uint32_t code)
{
    if (s->state == QcomIrisError) return;   /* reported once */
    s->state = QcomIrisError;
    s->last_error = code;
    if (s->ops->failed) s->ops->failed(s->ops->context, s, code);
}
void qcom_iris_session_fail(struct qcom_iris_session *s, uint32_t code)
{
    fail(s, code);
    if (s->ops->signal) s->ops->signal(s->ops->context, s);
}

static void frame_flags(const struct qcom_iris_session *s, uint32_t hfi_flags, struct qcom_iris_frame *f)
{
    const uint32_t keyframe = HFI_GEN2_PICTURE_IDR | HFI_GEN2_PICTURE_I | HFI_GEN2_PICTURE_CRA | HFI_GEN2_PICTURE_BLA;
    f->picture_type = s->frame_info.picture_type;
    f->error = (s->frame_info.picture_type & HFI_GEN2_PICTURE_NOSHOW) || s->frame_info.data_corrupt || s->frame_info.overflow;
    f->keyframe = !(s->frame_info.picture_type & HFI_GEN2_PICTURE_NOSHOW) && (s->frame_info.picture_type & keyframe);
    f->last = (hfi_flags & (HFI_BUF_FW_FLAG_LAST | HFI_BUF_FW_FLAG_PSC_LAST)) != 0;
}

static void handle_buffer(struct qcom_iris_session *s, const struct qcom_iris_hfi_packet *p)
{
    if (p->payload_info == HFI_PAYLOAD_NONE) return;
    if (!qcom_iris_hfi_payload_valid(p)) {fail(s, HFI_ERROR_FATAL); return;}
    /* packets are 4-byte aligned in the queue; the buffer descriptor holds 64-bit fields */
    struct qcom_iris_hfi_buffer copy;
    __builtin_memcpy(&copy, p->payload, sizeof copy);
    const struct qcom_iris_hfi_buffer *b = &copy;
    const uint32_t type = driver_buffer_type(s, b->type);
    if (!type) return;
    if ((p->port == HFI_PORT_NONE && b->type != HFI_BUFFER_PERSIST) ||
        (p->port != HFI_PORT_BITSTREAM && p->port != HFI_PORT_RAW && b->type != HFI_BUFFER_PERSIST))
        return;
    struct qcom_iris_frame f = {0};
    f.index = b->index;
    f.device_addr = b->base_address;
    f.data_offset = b->data_offset;
    f.data_size = b->data_size;
    f.timestamp = b->timestamp;
    f.hfi_flags = b->flags;
    if (type == QcomIrisBufInput) {
        frame_flags(s, b->flags, &f);
        if (s->ops->input_done) s->ops->input_done(s->ops->context, s, &f);
        return;
    }
    if (type == QcomIrisBufOutput) {
        if (b->flags & HFI_BUF_FW_FLAG_LAST) {
            if (!(s->sub_state & QcomIrisSubDrainLast) && (s->sub_state & QcomIrisSubDrain))
                change_sub_state(s, 0, QcomIrisSubDrainLast | QcomIrisSubOutputPause);
        }
        if (b->flags & HFI_BUF_FW_FLAG_PSC_LAST) {
            if (!(s->sub_state & QcomIrisSubDrcLast) && (s->sub_state & QcomIrisSubDrc) && (s->sub_state & QcomIrisSubInputPause) &&
                !(s->sub_state & QcomIrisSubFirstIpsc))
                change_sub_state(s, 0, QcomIrisSubDrcLast | QcomIrisSubOutputPause);
        }
        frame_flags(s, b->flags, &f);
        if (!f.data_size && s->state == QcomIrisStreaming && !(b->flags & HFI_BUF_FW_FLAG_LAST)) f.error = true;
        if (s->ops->output_done) s->ops->output_done(s->ops->context, s, &f);
        return;
    }
    /* iris_hfi_gen2_handle_release_internal_buffer */
    for (unsigned k = 0; k < QcomIrisInternalMax; ++k) {
        struct qcom_iris_internal *in = &s->internal[k];
        if (in->type && in->device_addr == b->base_address) {destroy_internal(s, in); break;}
    }
}

/* iris_hfi_gen2_read_input_subcr_params */
static void settings_change(struct qcom_iris_session *s)
{
    const struct qcom_iris_subscription *p = &s->src_subcr;
    const uint32_t width = (p->bitstream_resolution & HFI_BITMASK_BITSTREAM_WIDTH) >> 16, height = p->bitstream_resolution & HFI_BITMASK_BITSTREAM_HEIGHT;
    s->src_width = width;
    s->src_height = height;
    s->dst_width = ALIGN_UP(width, 128u);
    s->dst_height = ALIGN_UP(height, 32u);
    s->crop_top = p->crop_offsets[0] & 0xffff;
    s->crop_left = (p->crop_offsets[0] >> 16) & 0xffff;
    s->crop_height = height - (p->crop_offsets[1] & 0xffff) - s->crop_top;
    s->crop_width = width - ((p->crop_offsets[1] >> 16) & 0xffff) - s->crop_left;
    switch (s->codec) {
    case QcomIrisCodecHevc: s->caps[QcomIrisCapProfileHevc].value = p->profile; s->caps[QcomIrisCapLevelHevc].value = p->level; break;
    case QcomIrisCodecVp9: s->caps[QcomIrisCapProfileVp9].value = p->profile; s->caps[QcomIrisCapLevelVp9].value = p->level; break;
    default: s->caps[QcomIrisCapProfileH264].value = p->profile; s->caps[QcomIrisCapLevelH264].value = p->level; break;
    }
    s->caps[QcomIrisCapPoc].value = p->pic_order_cnt;
    s->caps[QcomIrisCapTier].value = p->tier;
    s->caps[QcomIrisCapBitDepth].value = p->bit_depth == BIT_DEPTH_8 ? BIT_DEPTH_8 : BIT_DEPTH_10;
    const bool supported = (p->bit_depth == BIT_DEPTH_8 || p->bit_depth == BIT_DEPTH_10) && (p->coded_frames & HFI_BITMASK_FRAME_MBS_ONLY_FLAG);
    s->fw_min_count = p->fw_min_count;
    const struct qcom_iris_buffer_query q = query(s);
    s->output_min_count = qcom_iris_buffer_count(&q, QcomIrisBufOutput);
    struct qcom_iris_stream_info info = {0};
    info.coded_width = width; info.coded_height = height;
    info.crop_left = s->crop_left; info.crop_top = s->crop_top; info.crop_width = s->crop_width; info.crop_height = s->crop_height;
    info.output_width = s->dst_width; info.output_height = s->dst_height;
    info.output_size = qcom_iris_buffer_size(&q, QcomIrisBufOutput);
    info.output_min_count = s->output_min_count;
    info.bit_depth = p->bit_depth; info.color_info = p->color_info; info.profile = p->profile; info.level = p->level;
    info.supported = supported;   /* 8-bit as NV12, 10-bit as P010 */
    if (!supported) fail(s, HFI_ERROR_STREAM_UNSUPPORTED);
    if (s->ops->settings_change) s->ops->settings_change(s->ops->context, s, &info);
}

/* iris_hfi_gen2_init_src_change_param: the defaults the firmware's property packets then overwrite */
static void init_src_change_param(struct qcom_iris_session *s)
{
    struct qcom_iris_subscription *p = &s->src_subcr;
    p->bitstream_resolution = s->src_width << 16 | s->src_height;
    p->crop_offsets[0] = s->crop_left << 16 | s->crop_top;
    p->crop_offsets[1] = (s->src_width - s->crop_width) << 16 | (s->src_height - s->crop_height);
    p->fw_min_count = s->output_min_count;
    p->color_info = color_info(s) & ~(0x1000000u | 0x20000000u);
    switch (s->codec) {
    case QcomIrisCodecHevc: p->profile = cap_value(s, QcomIrisCapProfileHevc); p->level = cap_value(s, QcomIrisCapLevelHevc); break;
    case QcomIrisCodecVp9: p->profile = cap_value(s, QcomIrisCapProfileVp9); p->level = cap_value(s, QcomIrisCapLevelVp9); break;
    default: p->profile = cap_value(s, QcomIrisCapProfileH264); p->level = cap_value(s, QcomIrisCapLevelH264); break;
    }
    p->pic_order_cnt = cap_value(s, QcomIrisCapPoc);
    p->bit_depth = cap_value(s, QcomIrisCapBitDepth);
    p->coded_frames = HFI_BITMASK_FRAME_MBS_ONLY_FLAG;
}

static void handle_property(struct qcom_iris_session *s, const struct qcom_iris_hfi_packet *p)
{
    if (p->flags & HFI_FW_FLAGS_INFORMATION) return;
    if (p->size < sizeof *p + 4) return;
    const uint32_t v = p->payload[0];
    struct qcom_iris_subscription *c = &s->src_subcr;
    switch (p->type) {
    case HFI_PROP_BITSTREAM_RESOLUTION: c->bitstream_resolution = v; break;
    case HFI_PROP_CROP_OFFSETS: if (p->size >= sizeof *p + 8) {c->crop_offsets[0] = v; c->crop_offsets[1] = p->payload[1];} break;
    case HFI_PROP_LUMA_CHROMA_BIT_DEPTH: c->bit_depth = v; break;
    case HFI_PROP_CODED_FRAMES: c->coded_frames = v; break;
    case HFI_PROP_BUFFER_FW_MIN_OUTPUT_COUNT: c->fw_min_count = v; break;
    case HFI_PROP_PIC_ORDER_CNT_TYPE: c->pic_order_cnt = v; break;
    case HFI_PROP_SIGNAL_COLOR_INFO: c->color_info = v; break;
    case HFI_PROP_PROFILE: c->profile = v; break;
    case HFI_PROP_LEVEL: c->level = v; break;
    case HFI_PROP_TIER: c->tier = v; break;
    case HFI_PROP_PICTURE_TYPE: s->frame_info.picture_type = v; break;
    case HFI_PROP_NO_OUTPUT: s->frame_info.no_output = true; break;
    default: break;
    }
}

static void handle_command(struct qcom_iris_session *s, const struct qcom_iris_hfi_packet *p)
{
    switch (p->type) {
    case HFI_CMD_CLOSE:
        if (!(p->flags & HFI_FW_FLAGS_SUCCESS)) s->state = QcomIrisError;
        s->responded = true;
        if (s->ops->signal) s->ops->signal(s->ops->context, s);
        break;
    case HFI_CMD_STOP:
        /* iris_hfi_gen2_handle_session_stop: the raw port pauses the CAPTURE side, the bitstream port the OUTPUT side */
        if (p->port == HFI_PORT_RAW) change_sub_state(s, 0, QcomIrisSubOutputPause);
        else if (p->port == HFI_PORT_BITSTREAM) change_sub_state(s, 0, QcomIrisSubInputPause);
        s->responded = true;
        if (s->ops->signal) s->ops->signal(s->ops->context, s);
        break;
    case HFI_CMD_BUFFER: handle_buffer(s, p); break;
    case HFI_CMD_SETTINGS_CHANGE:
        if (p->port != HFI_PORT_BITSTREAM || s->encoder) break;
        if (s->sub_state & QcomIrisSubDrc) break;
        /* iris_inst_sub_state_change_drc */
        if (s->state == QcomIrisInputStreaming || s->state == QcomIrisInit) change_sub_state(s, 0, QcomIrisSubFirstIpsc | QcomIrisSubInputPause);
        else change_sub_state(s, 0, QcomIrisSubDrc | QcomIrisSubInputPause);
        settings_change(s);
        break;
    case HFI_CMD_DRAIN:
        if (!(p->flags & HFI_FW_FLAGS_SUCCESS)) {fail(s, HFI_ERROR_FATAL); break;}
        if (s->sub_state & QcomIrisSubDrain) change_sub_state(s, 0, QcomIrisSubInputPause);
        break;
    default: break;
    }
}

static void handle_info(struct qcom_iris_session *s, const struct qcom_iris_hfi_packet *p)
{
    switch (p->type) {
    case HFI_INFO_DATA_CORRUPT: s->frame_info.data_corrupt = true; break;
    case HFI_INFO_BUFFER_OVERFLOW: s->frame_info.overflow = true; break;
    case HFI_INFO_HFI_FLAG_DRAIN_LAST:
        if (!(s->sub_state & QcomIrisSubDrainLast) && (s->sub_state & QcomIrisSubDrain))
            change_sub_state(s, 0, QcomIrisSubDrainLast | QcomIrisSubOutputPause);
        break;
    case HFI_INFO_HFI_FLAG_PSC_LAST:
        if (!(s->sub_state & QcomIrisSubDrcLast) && (s->sub_state & QcomIrisSubDrc) && (s->sub_state & QcomIrisSubInputPause) &&
            !(s->sub_state & QcomIrisSubFirstIpsc))
            change_sub_state(s, 0, QcomIrisSubDrcLast | QcomIrisSubOutputPause);
        break;
    default: break;
    }
}

/* iris_hfi_gen2_handle_session_response: settings-change defaults first, then the packets by range: errors, information,
 * properties, commands. */
void qcom_iris_session_handle(struct qcom_iris_session *s, const void *message)
{
    __builtin_memset(&s->frame_info, 0, sizeof s->frame_info);
    for (const struct qcom_iris_hfi_packet *p = qcom_iris_hfi_next(message, NULL); p; p = qcom_iris_hfi_next(message, p))
        if (p->type == HFI_CMD_SETTINGS_CHANGE && p->port == HFI_PORT_BITSTREAM) {init_src_change_param(s); break;}
    /* The firmware's own error packet names the reason; a command's answer flagged as a session error only names the command.
     * The error packet is taken first, so the client hears the reason. */
    for (const struct qcom_iris_hfi_packet *p = qcom_iris_hfi_next(message, NULL); p; p = qcom_iris_hfi_next(message, p))
        if (p->type > HFI_SESSION_ERROR_BEGIN && p->type < HFI_SESSION_ERROR_END) {fail(s, p->type); break;}
    static const struct {uint32_t begin, end; uint8_t kind;} range[] = {
        {HFI_SESSION_ERROR_BEGIN, HFI_SESSION_ERROR_END, 0}, {HFI_INFORMATION_BEGIN, HFI_INFORMATION_END, 1},
        {HFI_PROP_BEGIN, HFI_PROP_END, 2}, {HFI_CMD_BEGIN, HFI_CMD_END, 3},
    };
    for (unsigned r = 0; r < sizeof range / sizeof range[0]; ++r) {
        for (const struct qcom_iris_hfi_packet *p = qcom_iris_hfi_next(message, NULL); p; p = qcom_iris_hfi_next(message, p)) {
            if (p->flags & HFI_FW_FLAGS_SESSION_ERROR) fail(s, p->type);
            if (p->type <= range[r].begin || p->type >= range[r].end) continue;
            switch (range[r].kind) {
            case 0: fail(s, p->type); break;
            case 1: handle_info(s, p); break;
            case 2: handle_property(s, p); break;
            case 3: handle_command(s, p); break;
            }
        }
    }
}
