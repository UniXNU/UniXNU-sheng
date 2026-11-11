/* SPDX-License-Identifier: GPL-2.0-only */
/* The core-level HFI gen 2 messages (iris_hfi_gen2_packet.c sys_init, image version, inter-frame power collapse, power-collapse prep)
 * with the SM8550 UBWC 4.0 configuration (highest bank bit 16), and the system half of iris_hfi_gen2_response.c. */
#ifndef QCOM_IRIS_SYSTEM_H
#define QCOM_IRIS_SYSTEM_H
#include <stdbool.h>
#include <stdint.h>
#include "QcomIrisHfi.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Each writes one message into buffer and returns its size (0 when it does not fit). */
uint32_t qcom_iris_system_init(uint8_t *buffer, uint32_t capacity, uint32_t *header_id, uint32_t *packet_id);
uint32_t qcom_iris_system_image_version(uint8_t *buffer, uint32_t capacity, uint32_t *header_id, uint32_t *packet_id);
/* The firmware's debug messages: the debug configuration (0: the default) and the message kinds it writes to the debug queue
 * (1 errors, 2 fatal, 8 high). The firmware writes none until it is told a level. */
uint32_t qcom_iris_system_debug(uint8_t *buffer, uint32_t capacity, uint32_t *header_id, uint32_t *packet_id, uint32_t level);
uint32_t qcom_iris_system_interframe_powercollapse(uint8_t *buffer, uint32_t capacity, uint32_t *header_id, uint32_t *packet_id);
uint32_t qcom_iris_system_pc_prep(uint8_t *buffer, uint32_t capacity, uint32_t *header_id, uint32_t *packet_id);

struct qcom_iris_system_event {
    bool init_done, init_failed, error;
    uint32_t error_type;              /* the packet type of a system error */
    char version[128];                /* the firmware's image version, NUL-terminated, empty when none came */
};
/* iris_hfi_gen2_handle_system_response for a validated message with session id 0. */
void qcom_iris_system_handle(const void *message, struct qcom_iris_system_event *);
#ifdef __cplusplus
}
#endif
#endif
