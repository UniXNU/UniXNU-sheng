/* SPDX-License-Identifier: GPL-2.0-only */
/* sources: src/IrisSystem.c src/IrisHfi.c */
/* The system messages and responses: the init message with the SM8550 UBWC values, the image version, inter-frame power collapse
 * and power-collapse prep; init done, a failed init, the image version string and a system error. */
#include "QcomIrisSystem.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint8_t b[4096];
    uint32_t header = 0, packet = 0;
    const uint32_t n = qcom_iris_system_init(b, sizeof b, &header, &packet);
    assert(n == 32 + 8 * 36 && qcom_iris_hfi_valid(b, n) && header == 1 && packet == 8);
    const uint32_t expect[8][2] = {{HFI_CMD_INIT, 1}, {HFI_PROP_UBWC_MAX_CHANNELS, 8}, {HFI_PROP_UBWC_MAL_LENGTH, 32}, {HFI_PROP_UBWC_HBB, 16},
        {HFI_PROP_UBWC_BANK_SWZL_LEVEL1, 0}, {HFI_PROP_UBWC_BANK_SWZL_LEVEL2, 1}, {HFI_PROP_UBWC_BANK_SWZL_LEVEL3, 1}, {HFI_PROP_UBWC_BANK_SPREADING, 1}};
    unsigned i = 0;
    for (const struct qcom_iris_hfi_packet *p = qcom_iris_hfi_next(b, NULL); p; p = qcom_iris_hfi_next(b, p), ++i)
        assert(p->type == expect[i][0] && p->payload[0] == expect[i][1] && p->port == HFI_PORT_NONE);
    assert(i == 8);
    assert(qcom_iris_system_image_version(b, sizeof b, &header, &packet) == 64);
    assert(((const struct qcom_iris_hfi_packet *)(b + 32))->flags == (HFI_HOST_FLAGS_RESPONSE_REQUIRED | HFI_HOST_FLAGS_INTR_REQUIRED | HFI_HOST_FLAGS_GET_PROPERTY));
    assert(qcom_iris_system_interframe_powercollapse(b, sizeof b, &header, &packet) == 68);
    assert(qcom_iris_system_pc_prep(b, sizeof b, &header, &packet) == 64);

    struct qcom_iris_system_event e;
    struct qcom_iris_hfi_writer w;
    qcom_iris_hfi_begin(&w, b, sizeof b, 0, 0);
    qcom_iris_hfi_add(&w, HFI_CMD_INIT, HFI_FW_FLAGS_SUCCESS, HFI_PAYLOAD_NONE, HFI_PORT_NONE, 0, NULL, 0);
    char version[128] = "video-firmware.3.1";
    qcom_iris_hfi_add(&w, HFI_PROP_IMAGE_VERSION, 0, HFI_PAYLOAD_STRING, HFI_PORT_NONE, 0, version, sizeof version);
    qcom_iris_system_handle(b, &e);
    assert(e.init_done && !e.error && !strncmp(e.version, "video-firmware.3.1  ", 20));
    qcom_iris_hfi_begin(&w, b, sizeof b, 0, 0);
    qcom_iris_hfi_add(&w, HFI_CMD_INIT, 0, HFI_PAYLOAD_NONE, HFI_PORT_NONE, 0, NULL, 0);
    qcom_iris_system_handle(b, &e);
    assert(e.init_failed && !e.init_done);
    qcom_iris_hfi_begin(&w, b, sizeof b, 0, 0);
    qcom_iris_hfi_add(&w, HFI_SYS_ERROR_WD_TIMEOUT, 0, HFI_PAYLOAD_NONE, HFI_PORT_NONE, 0, NULL, 0);
    qcom_iris_system_handle(b, &e);
    assert(e.error && e.error_type == HFI_SYS_ERROR_WD_TIMEOUT);
    printf("system: init with UBWC 4.0, image version, power collapse messages, init and error responses: ok\n");
    return 0;
}
