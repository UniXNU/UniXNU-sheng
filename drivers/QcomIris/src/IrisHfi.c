/* SPDX-License-Identifier: GPL-2.0-only */
#include "QcomIrisHfi.h"

_Static_assert(sizeof(struct qcom_iris_queue_header) == 56, "iris_hfi_queue_header");
_Static_assert(sizeof(struct qcom_iris_queue_table) == 456, "iris_hfi_queue_table_header");
_Static_assert(sizeof(struct qcom_iris_hfi_header) == 32 && sizeof(struct qcom_iris_hfi_packet) == 32, "iris_hfi_header, packet");
_Static_assert(sizeof(struct qcom_iris_hfi_buffer) == 64, "iris_hfi_buffer");

static void copy_in(volatile uint8_t *to, const uint8_t *from, uint32_t n) {for (uint32_t i = 0; i < n; ++i) to[i] = from[i];}
static void copy_out(uint8_t *to, const volatile uint8_t *from, uint32_t n) {for (uint32_t i = 0; i < n; ++i) to[i] = from[i];}
static void zero(volatile uint8_t *p, size_t n) {for (size_t i = 0; i < n; ++i) p[i] = 0;}
static void barrier(void) {__atomic_thread_fence(__ATOMIC_SEQ_CST); __asm__ volatile("dsb sy" ::: "memory");}

static volatile struct qcom_iris_queue_table *table(const struct qcom_iris_queues *q)
{
    return (volatile struct qcom_iris_queue_table *)q->base;
}
static uint32_t queue_offset(unsigned id) {return (uint32_t)sizeof(struct qcom_iris_queue_table) + id * QCOM_IRIS_IFACEQ_QUEUE_SIZE;}

/* iris_vpu_setup_ucregion_memory_map: the region the firmware is told is the SFR and the queues, 1 MiB aligned. */
uint32_t qcom_iris_uc_region_size(void)
{
    return (uint32_t)((QCOM_IRIS_SFR_SIZE + QCOM_IRIS_QUEUE_BYTES + 0xfffff) & ~(size_t)0xfffff);
}

void qcom_iris_queues_init(const struct qcom_iris_queues *q, volatile uint32_t *sfr)
{
    volatile struct qcom_iris_queue_table *t = table(q);
    zero(q->base, sizeof *t);
    for (unsigned id = 0; id < QcomIrisNumQ; ++id) {
        volatile struct qcom_iris_queue_header *h = &t->q_hdr[id];
        h->status = 1;
        h->start_addr = q->device_addr + queue_offset(id);
        h->header_type = 0x0101;   /* IFACEQ_DFLT_QHDR */
        h->queue_type = (uint16_t)id;
        h->q_size = QCOM_IRIS_IFACEQ_QUEUE_SIZE / 4;
        h->pkt_size = 0;
        h->rx_wm = 1;
        h->tx_wm = 1;
        h->rx_req = id == QcomIrisDbgQ ? 0 : 1;   /* no interrupt for debug messages */
        h->tx_req = 0;
        h->rx_irq_status = 0;
        h->tx_irq_status = 0;
        h->read_idx = 0;
        h->write_idx = 0;
    }
    t->version = 0;
    t->device_addr = 0;
    const char name[] = "iris-hfi-queues";
    copy_in((volatile uint8_t *)t->name, (const uint8_t *)name, sizeof name);
    t->size = sizeof *t;
    t->qhdr0_offset = sizeof *t - QcomIrisNumQ * sizeof(struct qcom_iris_queue_header);
    t->qhdr_size = sizeof(struct qcom_iris_queue_header);
    t->num_q = QcomIrisNumQ;
    t->num_active_q = QcomIrisNumQ;
    if (sfr) sfr[0] = QCOM_IRIS_SFR_SIZE;
    barrier();
}

int qcom_iris_queue_write(const struct qcom_iris_queues *q, const void *packet, uint32_t size)
{
    volatile struct qcom_iris_queue_header *h = &table(q)->q_hdr[QcomIrisCmdQ];
    volatile uint8_t *queue = q->base + queue_offset(QcomIrisCmdQ);
    const uint32_t write_idx = h->write_idx * 4, read_idx = h->read_idx * 4;
    if (!packet || !size || (size & 3) || write_idx >= QCOM_IRIS_IFACEQ_QUEUE_SIZE || read_idx >= QCOM_IRIS_IFACEQ_QUEUE_SIZE) return -1;
    const uint32_t empty = write_idx < read_idx ? read_idx - write_idx : QCOM_IRIS_IFACEQ_QUEUE_SIZE - (write_idx - read_idx);
    if (empty < size) return -1;
    h->tx_req = 0;
    uint32_t new_write = write_idx + size;
    if (new_write < QCOM_IRIS_IFACEQ_QUEUE_SIZE) {
        copy_in(queue + write_idx, packet, size);
    } else {
        const uint32_t residue = new_write - QCOM_IRIS_IFACEQ_QUEUE_SIZE;
        copy_in(queue + write_idx, packet, size - residue);
        copy_in(queue, (const uint8_t *)packet + size - residue, residue);
        new_write = residue;
    }
    barrier();
    h->write_idx = new_write / 4;
    barrier();
    return 0;
}

int qcom_iris_queue_read(const struct qcom_iris_queues *q, unsigned id, void *packet, uint32_t capacity)
{
    if (id != QcomIrisMsgQ && id != QcomIrisDbgQ) return -1;
    volatile struct qcom_iris_queue_header *h = &table(q)->q_hdr[id];
    volatile uint8_t *queue = q->base + queue_offset(id);
    const uint32_t receive_request = id == QcomIrisMsgQ ? 1 : 0;
    const uint32_t write_idx = h->write_idx * 4, read_idx = h->read_idx * 4;
    if (read_idx == write_idx) {
        h->rx_req = receive_request;
        barrier();
        return 0;
    }
    if (read_idx > QCOM_IRIS_IFACEQ_QUEUE_SIZE - 4 || write_idx >= QCOM_IRIS_IFACEQ_QUEUE_SIZE) return -1;
    barrier();
    uint32_t size;
    copy_out((uint8_t *)&size, queue + read_idx, 4);
    if (!size) return -1;
    uint32_t new_read = read_idx + size;
    int result = (int)size;
    if (size <= QCOM_IRIS_CORE_PKT_SIZE && size <= capacity) {
        if (new_read < QCOM_IRIS_IFACEQ_QUEUE_SIZE) {
            copy_out(packet, queue + read_idx, size);
        } else {
            const uint32_t residue = new_read - QCOM_IRIS_IFACEQ_QUEUE_SIZE;
            copy_out(packet, queue + read_idx, size - residue);
            copy_out((uint8_t *)packet + size - residue, queue, residue);
            new_read = residue;
        }
    } else {
        new_read = write_idx;   /* the mainline drop: skip to the writer */
        result = -1;
    }
    h->rx_req = receive_request;
    h->read_idx = new_read / 4;
    barrier();
    return result;
}

void qcom_iris_hfi_begin(struct qcom_iris_hfi_writer *w, uint8_t *buffer, uint32_t capacity, uint32_t session_id, uint32_t header_id)
{
    w->buffer = buffer;
    w->capacity = capacity;
    w->ok = buffer && capacity >= sizeof(struct qcom_iris_hfi_header);
    if (!w->ok) return;
    struct qcom_iris_hfi_header *h = (struct qcom_iris_hfi_header *)buffer;
    __builtin_memset(h, 0, sizeof *h);
    h->size = sizeof *h;
    h->session_id = session_id;
    h->header_id = header_id;
}

void qcom_iris_hfi_add(struct qcom_iris_hfi_writer *w, uint32_t type, uint32_t flags, uint32_t payload_info, uint32_t port,
                       uint32_t packet_id, const void *payload, uint32_t payload_size)
{
    if (!w->ok) return;
    struct qcom_iris_hfi_header *h = (struct qcom_iris_hfi_header *)w->buffer;
    const uint32_t size = (uint32_t)sizeof(struct qcom_iris_hfi_packet) + payload_size;
    if ((payload_size & 3) || h->size + size > w->capacity || h->size + size > QCOM_IRIS_IFACEQ_MAX_PKT_SIZE * 4) {w->ok = false; return;}
    struct qcom_iris_hfi_packet *p = (struct qcom_iris_hfi_packet *)(w->buffer + h->size);
    __builtin_memset(p, 0, size);
    p->size = size;
    p->type = type;
    p->flags = flags;
    p->payload_info = payload_info;
    p->port = port;
    p->packet_id = packet_id;
    if (payload_size) __builtin_memcpy(p->payload, payload, payload_size);
    h->num_packets++;
    h->size += size;
}

uint32_t qcom_iris_hfi_size(const struct qcom_iris_hfi_writer *w)
{
    return w->ok ? ((const struct qcom_iris_hfi_header *)w->buffer)->size : 0;
}

bool qcom_iris_hfi_valid(const void *message, uint32_t size)
{
    const struct qcom_iris_hfi_header *h = message;
    if (size < sizeof *h + sizeof(struct qcom_iris_hfi_packet) || h->size > size || h->size < sizeof *h + sizeof(struct qcom_iris_hfi_packet) ||
        h->size > QCOM_IRIS_CORE_PKT_SIZE)
        return false;
    uint32_t at = sizeof *h;
    for (uint32_t i = 0; i < h->num_packets; ++i) {
        if (at + sizeof(struct qcom_iris_hfi_packet) > h->size) return false;
        const struct qcom_iris_hfi_packet *p = (const struct qcom_iris_hfi_packet *)((const uint8_t *)message + at);
        if (!p->size || p->size < sizeof *p || (p->size & 3) || at + p->size > h->size) return false;
        at += p->size;
    }
    return true;
}

const struct qcom_iris_hfi_packet *qcom_iris_hfi_next(const void *message, const struct qcom_iris_hfi_packet *previous)
{
    const struct qcom_iris_hfi_header *h = message;
    const uint8_t *start = (const uint8_t *)message + sizeof *h, *end = (const uint8_t *)message + h->size;
    const uint8_t *at = previous ? (const uint8_t *)previous + previous->size : start;
    if (!h->num_packets || at + sizeof(struct qcom_iris_hfi_packet) > end) return NULL;
    return (const struct qcom_iris_hfi_packet *)at;
}

bool qcom_iris_hfi_payload_valid(const struct qcom_iris_hfi_packet *p)
{
    uint32_t need = 0;
    switch (p->payload_info) {
    case HFI_PAYLOAD_U32: case HFI_PAYLOAD_S32: case HFI_PAYLOAD_Q16: case HFI_PAYLOAD_U32_ENUM: case HFI_PAYLOAD_32_PACKED: need = 4; break;
    case HFI_PAYLOAD_U64: case HFI_PAYLOAD_S64: case HFI_PAYLOAD_64_PACKED: need = 8; break;
    case HFI_PAYLOAD_STRUCTURE: if (p->type == HFI_CMD_BUFFER) need = sizeof(struct qcom_iris_hfi_buffer); break;
    default: break;
    }
    return p->size >= sizeof *p + need;
}
