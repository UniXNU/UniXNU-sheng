/* SPDX-License-Identifier: GPL-2.0-only */
/* The pinned mainline buffer calculator through the shim: the format sizes match the mainline formulas worked by hand, every
 * internal buffer of the VPU 3 decoder and encoder tables has a size, and the counts follow iris_vpu_buf_count. */
#include "QcomIrisBuffers.h"
#include <assert.h>
#include <stdio.h>

static struct qcom_iris_buffer_query decoder(enum qcom_iris_codec codec, uint32_t w, uint32_t h)
{
    struct qcom_iris_buffer_query q = {0};
    q.codec = codec;
    q.src_width = w; q.src_height = h;
    q.dst_width = (w + 127) & ~127u; q.dst_height = (h + 31) & ~31u;
    q.crop_width = w; q.crop_height = h;
    q.output_min_count = 4;
    q.num_vpp_pipe = 4;
    q.max_mbpf = (8192 * 4352) / 256;
    q.stage = 2;
    return q;
}

int main(void)
{
    struct qcom_iris_buffer_query q = decoder(QcomIrisCodecH264, 1920, 1080);
    /* NV12 1920x1088: stride 1920, Y 1088 lines, UV 544 lines: (1920 * 1088 + 1920 * 544) rounded to 4 KiB */
    assert(qcom_iris_buffer_size(&q, QcomIrisBufOutput) == ((1920u * 1088 + 1920u * 544 + 4095) & ~4095u));
    /* decoder bitstream below 4K: NUM_MBS_4K (256 x 144) * 256 * 3 / 2 / 2 */
    assert(qcom_iris_buffer_size(&q, QcomIrisBufInput) == 256u * 144 * 256 * 3 / 2 / 2);
    const enum qcom_iris_buffer dec_internal[] = {QcomIrisBufBin, QcomIrisBufComv, QcomIrisBufNonComv, QcomIrisBufLine,
                                                  QcomIrisBufPersist, QcomIrisBufDpb};
    for (unsigned c = 0; c < 3; ++c) {
        q = decoder((enum qcom_iris_codec)c, 3840, 2160);
        for (unsigned i = 0; i < sizeof dec_internal / sizeof dec_internal[0]; ++i) {
            const uint32_t size = qcom_iris_buffer_size(&q, dec_internal[i]);
            /* VP9 has no COMV or NON_COMV on VPU 3 (mainline sizes them for H.264 and HEVC only); every other entry is sized */
            if (!(c == QcomIrisCodecVp9 && (dec_internal[i] == QcomIrisBufComv || dec_internal[i] == QcomIrisBufNonComv))) assert(size);
            printf("dec codec %u 3840x2160 buffer %u: %u bytes, count %u\n", c, dec_internal[i], size,
                   qcom_iris_buffer_count(&q, dec_internal[i]));
        }
    }
    q = decoder(QcomIrisCodecH264, 1280, 720);
    assert(qcom_iris_buffer_count(&q, QcomIrisBufOutput) == 4);
    q.fw_min_count = 21;
    assert(qcom_iris_buffer_count(&q, QcomIrisBufOutput) == 21 && qcom_iris_buffer_count(&q, QcomIrisBufDpb) == 21);
    q = decoder(QcomIrisCodecVp9, 1280, 720);
    assert(qcom_iris_buffer_count(&q, QcomIrisBufOutput) == 9);

    struct qcom_iris_buffer_query e = {0};
    e.encoder = true;
    e.codec = QcomIrisCodecHevc;
    e.src_width = 1920; e.src_height = 1088;   /* NV12 input, aligned 128 x 32 */
    e.dst_width = 1920; e.dst_height = 1088;   /* coded size aligned to 32 for HEVC */
    e.crop_width = 1920; e.crop_height = 1080;
    e.num_vpp_pipe = 4; e.max_mbpf = (8192 * 4352) / 256; e.stage = 2; e.frame_rc = 1;
    /* 1920x1088 above 720p: 1920 * 1088 * 3 / 4, rounded to 4 KiB */
    assert(qcom_iris_buffer_size(&e, QcomIrisBufOutput) == ((1920u * 1088 * 3 / 4 + 4095) & ~4095u));
    const enum qcom_iris_buffer enc_internal[] = {QcomIrisBufBin, QcomIrisBufComv, QcomIrisBufNonComv, QcomIrisBufLine,
                                                  QcomIrisBufArp, QcomIrisBufScratch2, QcomIrisBufVpss};
    for (unsigned i = 0; i < sizeof enc_internal / sizeof enc_internal[0]; ++i) {
        const uint32_t size = qcom_iris_buffer_size(&e, enc_internal[i]);
        if (enc_internal[i] != QcomIrisBufVpss) assert(size);   /* VPSS is for scaling or rotation only */
        printf("enc hevc 1920x1088 buffer %u: %u bytes, count %u\n", enc_internal[i], size, qcom_iris_buffer_count(&e, enc_internal[i]));
    }
    printf("buffers: pinned calculator through the shim: ok\n");
    return 0;
}
