/* SPDX-License-Identifier: GPL-2.0-only */
#include "QcomIrisSystem.h"

/* qcom_ubwc_cfg_data ubwc_4_0_hbb16 (SM8550) through the ubwc.h helpers */
#define UBWC_MAX_CHANNELS 8u        /* macrotile mode: UBWC 3.1 and later */
#define UBWC_MAL_LENGTH 32u         /* 64-byte minimum access only for UBWC 1.0 */
#define UBWC_HBB 16u
#define UBWC_SWIZZLE_L1 0u
#define UBWC_SWIZZLE_L2 1u
#define UBWC_SWIZZLE_L3 1u
#define UBWC_BANK_SPREADING 1u

uint32_t qcom_iris_system_init(uint8_t *buffer, uint32_t capacity, uint32_t *header_id, uint32_t *packet_id)
{
    struct qcom_iris_hfi_writer w;
    qcom_iris_hfi_begin(&w, buffer, capacity, 0, (*header_id)++);
    uint32_t v = HFI_VIDEO_ARCH_LX;
    qcom_iris_hfi_add(&w, HFI_CMD_INIT, HFI_HOST_FLAGS_RESPONSE_REQUIRED | HFI_HOST_FLAGS_INTR_REQUIRED | HFI_HOST_FLAGS_NON_DISCARDABLE,
                      HFI_PAYLOAD_U32, HFI_PORT_NONE, (*packet_id)++, &v, 4);
    const struct {uint32_t type, value;} props[] = {
        {HFI_PROP_UBWC_MAX_CHANNELS, UBWC_MAX_CHANNELS}, {HFI_PROP_UBWC_MAL_LENGTH, UBWC_MAL_LENGTH}, {HFI_PROP_UBWC_HBB, UBWC_HBB},
        {HFI_PROP_UBWC_BANK_SWZL_LEVEL1, UBWC_SWIZZLE_L1}, {HFI_PROP_UBWC_BANK_SWZL_LEVEL2, UBWC_SWIZZLE_L2},
        {HFI_PROP_UBWC_BANK_SWZL_LEVEL3, UBWC_SWIZZLE_L3}, {HFI_PROP_UBWC_BANK_SPREADING, UBWC_BANK_SPREADING},
    };
    for (unsigned i = 0; i < sizeof props / sizeof props[0]; ++i) {
        v = props[i].value;
        qcom_iris_hfi_add(&w, props[i].type, HFI_HOST_FLAGS_NONE, HFI_PAYLOAD_U32, HFI_PORT_NONE, (*packet_id)++, &v, 4);
    }
    return qcom_iris_hfi_size(&w);
}

uint32_t qcom_iris_system_image_version(uint8_t *buffer, uint32_t capacity, uint32_t *header_id, uint32_t *packet_id)
{
    struct qcom_iris_hfi_writer w;
    qcom_iris_hfi_begin(&w, buffer, capacity, 0, (*header_id)++);
    qcom_iris_hfi_add(&w, HFI_PROP_IMAGE_VERSION, HFI_HOST_FLAGS_RESPONSE_REQUIRED | HFI_HOST_FLAGS_INTR_REQUIRED | HFI_HOST_FLAGS_GET_PROPERTY,
                      HFI_PAYLOAD_NONE, HFI_PORT_NONE, (*packet_id)++, NULL, 0);
    return qcom_iris_hfi_size(&w);
}

uint32_t qcom_iris_system_interframe_powercollapse(uint8_t *buffer, uint32_t capacity, uint32_t *header_id, uint32_t *packet_id)
{
    struct qcom_iris_hfi_writer w;
    qcom_iris_hfi_begin(&w, buffer, capacity, 0, (*header_id)++);
    const uint32_t v = 1;
    qcom_iris_hfi_add(&w, HFI_PROP_INTRA_FRAME_POWER_COLLAPSE, HFI_HOST_FLAGS_NONE, HFI_PAYLOAD_U32, HFI_PORT_NONE, (*packet_id)++, &v, 4);
    return qcom_iris_hfi_size(&w);
}

uint32_t qcom_iris_system_pc_prep(uint8_t *buffer, uint32_t capacity, uint32_t *header_id, uint32_t *packet_id)
{
    struct qcom_iris_hfi_writer w;
    qcom_iris_hfi_begin(&w, buffer, capacity, 0, (*header_id)++);
    qcom_iris_hfi_add(&w, HFI_CMD_POWER_COLLAPSE, HFI_HOST_FLAGS_NONE, HFI_PAYLOAD_NONE, HFI_PORT_NONE, (*packet_id)++, NULL, 0);
    return qcom_iris_hfi_size(&w);
}

void qcom_iris_system_handle(const void *message, struct qcom_iris_system_event *e)
{
    *e = (struct qcom_iris_system_event){0};
    static const struct {uint32_t begin, end; uint8_t kind;} range[] = {
        {HFI_SYSTEM_ERROR_BEGIN, HFI_SYSTEM_ERROR_END, 0}, {HFI_PROP_BEGIN, HFI_PROP_END, 1}, {HFI_CMD_BEGIN, HFI_CMD_END, 2},
    };
    for (unsigned r = 0; r < sizeof range / sizeof range[0]; ++r) {
        for (const struct qcom_iris_hfi_packet *p = qcom_iris_hfi_next(message, NULL); p; p = qcom_iris_hfi_next(message, p)) {
            if (p->flags & HFI_FW_FLAGS_SYSTEM_ERROR) {e->error = true; e->error_type = p->type; return;}
            if (p->type <= range[r].begin || p->type >= range[r].end) continue;
            switch (range[r].kind) {
            case 0:
                e->error = true;
                e->error_type = p->type;
                return;
            case 1:
                if (p->type == HFI_PROP_IMAGE_VERSION) {
                    /* iris_hfi_gen2_handle_image_version_property: NULs shown as spaces */
                    const uint8_t *s = (const uint8_t *)p->payload;
                    const uint32_t n = p->size - (uint32_t)sizeof *p;
                    if (n < sizeof e->version - 1) break;
                    for (unsigned i = 0; i < sizeof e->version - 1; ++i) e->version[i] = s[i] ? (char)s[i] : ' ';
                    e->version[sizeof e->version - 1] = 0;
                }
                break;
            case 2:
                if (p->flags & HFI_FW_FLAGS_SUCCESS) e->init_done = true;
                else e->init_failed = true;
                break;
            }
        }
    }
}
