/* SPDX-License-Identifier: GPL-2.0-only */
/* HFI gen 2 of the iris VPU: the shared queues (iris_hfi_queue.h/.c), packet headers (iris_hfi_gen2_packet.h/.c) and the enums of
 * iris_hfi_common.h. Command and property ids come from the pinned iris_hfi_gen2_defines.h. Pure C over memory the caller owns: the
 * same code runs in the kernel and in the host tests. */
#ifndef QCOM_IRIS_HFI_H
#define QCOM_IRIS_HFI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "iris_hfi_gen2_defines.h"
#ifdef __cplusplus
extern "C" {
#endif
/* iris_hfi_common.h */
enum {HFI_PORT_NONE = 0, HFI_PORT_BITSTREAM = 1, HFI_PORT_RAW = 2};
enum {
    HFI_PAYLOAD_NONE = 0, HFI_PAYLOAD_U32 = 1, HFI_PAYLOAD_S32 = 2, HFI_PAYLOAD_U64 = 3, HFI_PAYLOAD_S64 = 4,
    HFI_PAYLOAD_STRUCTURE = 5, HFI_PAYLOAD_BLOB = 6, HFI_PAYLOAD_STRING = 7, HFI_PAYLOAD_Q16 = 8, HFI_PAYLOAD_U32_ENUM = 9,
    HFI_PAYLOAD_32_PACKED = 10, HFI_PAYLOAD_U32_ARRAY = 11, HFI_PAYLOAD_S32_ARRAY = 12, HFI_PAYLOAD_64_PACKED = 13,
};
enum {
    HFI_HOST_FLAGS_NONE = 0, HFI_HOST_FLAGS_INTR_REQUIRED = 1, HFI_HOST_FLAGS_RESPONSE_REQUIRED = 2,
    HFI_HOST_FLAGS_NON_DISCARDABLE = 4, HFI_HOST_FLAGS_GET_PROPERTY = 8,
};
enum {
    HFI_PRIMARIES_RESERVED = 0, HFI_PRIMARIES_BT709 = 1, HFI_PRIMARIES_UNSPECIFIED = 2, HFI_PRIMARIES_BT470_SYSTEM_M = 4,
    HFI_PRIMARIES_BT470_SYSTEM_BG = 5, HFI_PRIMARIES_BT601_525 = 6, HFI_PRIMARIES_SMPTE_ST240M = 7, HFI_PRIMARIES_GENERIC_FILM = 8,
    HFI_PRIMARIES_BT2020 = 9, HFI_PRIMARIES_SMPTE_ST428_1 = 10, HFI_PRIMARIES_SMPTE_RP431_2 = 11, HFI_PRIMARIES_SMPTE_EG431_1 = 12,
    HFI_PRIMARIES_SMPTE_EBU_TECH = 22,
};
enum {
    HFI_TRANSFER_RESERVED = 0, HFI_TRANSFER_BT709 = 1, HFI_TRANSFER_UNSPECIFIED = 2, HFI_TRANSFER_BT470_SYSTEM_M = 4,
    HFI_TRANSFER_BT470_SYSTEM_BG = 5, HFI_TRANSFER_BT601_525_OR_625 = 6, HFI_TRANSFER_SMPTE_ST240M = 7, HFI_TRANSFER_LINEAR = 8,
    HFI_TRANSFER_LOG_100_1 = 9, HFI_TRANSFER_LOG_SQRT = 10, HFI_TRANSFER_XVYCC = 11, HFI_TRANSFER_BT1361_0 = 12,
    HFI_TRANSFER_SRGB_SYCC = 13, HFI_TRANSFER_BT2020_14 = 14, HFI_TRANSFER_BT2020_15 = 15, HFI_TRANSFER_SMPTE_ST2084_PQ = 16,
    HFI_TRANSFER_SMPTE_ST428_1 = 17, HFI_TRANSFER_BT2100_2_HLG = 18,
};
enum {
    HFI_MATRIX_COEFF_SRGB_SMPTE_ST428_1 = 0, HFI_MATRIX_COEFF_BT709 = 1, HFI_MATRIX_COEFF_UNSPECIFIED = 2, HFI_MATRIX_COEFF_RESERVED = 3,
    HFI_MATRIX_COEFF_FCC_TITLE_47 = 4, HFI_MATRIX_COEFF_BT470_SYS_BG_OR_BT601_625 = 5, HFI_MATRIX_COEFF_BT601_525_BT1358_525_OR_625 = 6,
    HFI_MATRIX_COEFF_SMPTE_ST240 = 7, HFI_MATRIX_COEFF_YCGCO = 8, HFI_MATRIX_COEFF_BT2020_NON_CONSTANT = 9,
    HFI_MATRIX_COEFF_BT2020_CONSTANT = 10, HFI_MATRIX_COEFF_SMPTE_ST2085 = 11, HFI_MATRIX_COEFF_BT2100 = 14,
};
/* iris_core.h, iris_hfi_queue.h */
#define QCOM_IRIS_CORE_PKT_SIZE (1024 * 4)
#define QCOM_IRIS_IFACEQ_MAX_PKT_SIZE 1024
#define QCOM_IRIS_IFACEQ_QUEUE_SIZE (QCOM_IRIS_IFACEQ_MAX_PKT_SIZE * 64 * 16)
#define QCOM_IRIS_SFR_SIZE 4096u
enum {QcomIrisCmdQ = 0, QcomIrisMsgQ = 1, QcomIrisDbgQ = 2, QcomIrisNumQ = 3};

struct qcom_iris_queue_header {
    uint32_t status, start_addr;
    uint16_t queue_type, header_type;
    uint32_t q_size, pkt_size, pkt_drop_cnt, rx_wm, tx_wm, rx_req, tx_req, rx_irq_status, tx_irq_status, read_idx, write_idx;
};
struct qcom_iris_queue_table {
    uint32_t version, size, qhdr0_offset, qhdr_size, num_q, num_active_q;
    uint64_t device_addr;            /* a host pointer in the mainline layout; the firmware does not use it */
    char name[256];
    struct qcom_iris_queue_header q_hdr[QcomIrisNumQ];
};
/* Bytes of the queue memory (the table and the three queues), 4 KiB aligned; and of the UC region the firmware is told. */
#define QCOM_IRIS_QUEUE_BYTES ((sizeof(struct qcom_iris_queue_table) + (size_t)QCOM_IRIS_IFACEQ_QUEUE_SIZE * QcomIrisNumQ + 4095) & ~(size_t)4095)
uint32_t qcom_iris_uc_region_size(void);

struct qcom_iris_queues {
    volatile uint8_t *base;          /* CPU mapping of the queue memory (QCOM_IRIS_QUEUE_BYTES), write-combined or uncached */
    uint32_t device_addr;            /* its device (IOVA) address */
};
/* iris_hfi_queues_init: the table and the three queue headers. sfr: the CPU mapping of the SFR buffer, whose first word is its size. */
void qcom_iris_queues_init(const struct qcom_iris_queues *, volatile uint32_t *sfr);
/* iris_hfi_queue_write on the command queue: 0, or -1 without room for the packet. The caller raises the interrupt after. */
int qcom_iris_queue_write(const struct qcom_iris_queues *, const void *packet, uint32_t size);
/* iris_hfi_queue_read on the message (q = QcomIrisMsgQ) or debug queue: the packet's size in bytes, 0 when empty, -1 for a packet
 * larger than QCOM_IRIS_CORE_PKT_SIZE (dropped, as mainline drops it) or a corrupt index. */
int qcom_iris_queue_read(const struct qcom_iris_queues *, unsigned q, void *packet, uint32_t capacity);

/* iris_hfi_gen2_packet.h */
struct qcom_iris_hfi_header {
    uint32_t size, session_id, header_id, reserved[4], num_packets;
};
struct qcom_iris_hfi_packet {
    uint32_t size, type, flags, payload_info, port, packet_id, reserved[2];
    uint32_t payload[];
};
struct qcom_iris_hfi_buffer {
    uint32_t type, index;
    uint64_t base_address;
    uint32_t addr_offset, buffer_size, data_offset, data_size;
    uint64_t timestamp;
    uint32_t flags, reserved[5];
};
/* A message under construction: the header and its packets in a caller buffer. */
struct qcom_iris_hfi_writer {
    uint8_t *buffer;
    uint32_t capacity;
    bool ok;
};
void qcom_iris_hfi_begin(struct qcom_iris_hfi_writer *, uint8_t *buffer, uint32_t capacity, uint32_t session_id, uint32_t header_id);
void qcom_iris_hfi_add(struct qcom_iris_hfi_writer *, uint32_t type, uint32_t flags, uint32_t payload_info, uint32_t port,
                       uint32_t packet_id, const void *payload, uint32_t payload_size);
uint32_t qcom_iris_hfi_size(const struct qcom_iris_hfi_writer *);

/* iris_hfi_gen2_validate_hdr_packet: every packet of a message lies inside it and inside the core packet limit. */
bool qcom_iris_hfi_valid(const void *message, uint32_t size);
/* The packets of a validated message, one at a time: NULL after the last. */
const struct qcom_iris_hfi_packet *qcom_iris_hfi_next(const void *message, const struct qcom_iris_hfi_packet *previous);
/* iris_hfi_gen2_validate_packet_payload: the packet is large enough for its payload type. */
bool qcom_iris_hfi_payload_valid(const struct qcom_iris_hfi_packet *);
#ifdef __cplusplus
}
#endif
#endif
