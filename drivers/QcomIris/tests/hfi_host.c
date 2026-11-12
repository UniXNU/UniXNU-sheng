/* SPDX-License-Identifier: GPL-2.0-only */
/* sources: src/IrisHfi.c */
/* The HFI queues and packets: the table layout the firmware reads, command writes with wrap-around and a full queue, message reads
 * with wrap-around, an oversized message dropped as mainline drops it, and the packet writer and validator. */
#include "QcomIrisHfi.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *memory;
static struct qcom_iris_queues queues;

static volatile struct qcom_iris_queue_header *header(unsigned q)
{
    return &((volatile struct qcom_iris_queue_table *)memory)->q_hdr[q];
}
static uint8_t *queue(unsigned q) {return memory + sizeof(struct qcom_iris_queue_table) + q * QCOM_IRIS_IFACEQ_QUEUE_SIZE;}

/* The firmware's side: a message written to the message queue at its write index. */
static void firmware_send(const void *message, uint32_t size)
{
    volatile struct qcom_iris_queue_header *h = header(QcomIrisMsgQ);
    uint32_t at = h->write_idx * 4;
    for (uint32_t i = 0; i < size; ++i) queue(QcomIrisMsgQ)[(at + i) % QCOM_IRIS_IFACEQ_QUEUE_SIZE] = ((const uint8_t *)message)[i];
    h->write_idx = ((at + size) % QCOM_IRIS_IFACEQ_QUEUE_SIZE) / 4;
}

int main(void)
{
    memory = aligned_alloc(4096, QCOM_IRIS_QUEUE_BYTES);
    uint32_t sfr[1024];
    queues.base = memory;
    queues.device_addr = 0x30000000;
    memset(memory, 0xa5, QCOM_IRIS_QUEUE_BYTES);
    qcom_iris_queues_init(&queues, sfr);
    const struct qcom_iris_queue_table *t = (const void *)memory;
    assert(t->size == 456 && t->qhdr0_offset == 288 && t->qhdr_size == 56 && t->num_q == 3 && t->num_active_q == 3);
    assert(!strcmp(t->name, "iris-hfi-queues") && sfr[0] == 4096);
    for (unsigned q = 0; q < 3; ++q) {
        assert(t->q_hdr[q].start_addr == 0x30000000 + 456 + q * QCOM_IRIS_IFACEQ_QUEUE_SIZE);
        assert(t->q_hdr[q].q_size == QCOM_IRIS_IFACEQ_QUEUE_SIZE / 4 && t->q_hdr[q].header_type == 0x0101 && t->q_hdr[q].status == 1);
        assert(t->q_hdr[q].rx_req == (q == QcomIrisDbgQ ? 0u : 1u));
    }
    assert(qcom_iris_uc_region_size() == 0x400000);   /* SFR plus 3 MiB of queues, to the next MiB */

    /* the system init message, as iris_hfi_gen2_packet_sys_init lays it out */
    uint8_t buffer[QCOM_IRIS_CORE_PKT_SIZE];
    struct qcom_iris_hfi_writer w;
    qcom_iris_hfi_begin(&w, buffer, sizeof buffer, 0, 7);
    uint32_t value = HFI_VIDEO_ARCH_LX;
    qcom_iris_hfi_add(&w, HFI_CMD_INIT, HFI_HOST_FLAGS_RESPONSE_REQUIRED | HFI_HOST_FLAGS_INTR_REQUIRED | HFI_HOST_FLAGS_NON_DISCARDABLE,
                      HFI_PAYLOAD_U32, HFI_PORT_NONE, 1, &value, 4);
    value = 8;
    qcom_iris_hfi_add(&w, HFI_PROP_UBWC_MAX_CHANNELS, 0, HFI_PAYLOAD_U32, HFI_PORT_NONE, 2, &value, 4);
    assert(qcom_iris_hfi_size(&w) == 32 + 2 * 36 && qcom_iris_hfi_valid(buffer, qcom_iris_hfi_size(&w)));
    const struct qcom_iris_hfi_header *h = (const void *)buffer;
    assert(h->num_packets == 2 && h->header_id == 7);
    const struct qcom_iris_hfi_packet *p = qcom_iris_hfi_next(buffer, NULL);
    assert(p->type == HFI_CMD_INIT && p->flags == 7 && p->payload[0] == 1);
    p = qcom_iris_hfi_next(buffer, p);
    assert(p->type == HFI_PROP_UBWC_MAX_CHANNELS && p->payload[0] == 8 && !qcom_iris_hfi_next(buffer, p));

    /* commands: written at the index, the index advanced in words, wrap-around, and a full queue refused */
    assert(!qcom_iris_queue_write(&queues, buffer, qcom_iris_hfi_size(&w)));
    assert(header(QcomIrisCmdQ)->write_idx == 104 / 4 && !memcmp(queue(QcomIrisCmdQ), buffer, 104));
    header(QcomIrisCmdQ)->write_idx = (QCOM_IRIS_IFACEQ_QUEUE_SIZE - 40) / 4;
    header(QcomIrisCmdQ)->read_idx = header(QcomIrisCmdQ)->write_idx;
    assert(!qcom_iris_queue_write(&queues, buffer, 104));
    assert(header(QcomIrisCmdQ)->write_idx == 64 / 4 && !memcmp(queue(QcomIrisCmdQ), buffer + 40, 64));
    header(QcomIrisCmdQ)->read_idx = header(QcomIrisCmdQ)->write_idx + 25;   /* 100 bytes free */
    assert(qcom_iris_queue_write(&queues, buffer, 104) == -1);

    /* messages: empty, one read, one across the end, and one too large dropped */
    uint8_t in[QCOM_IRIS_CORE_PKT_SIZE];
    assert(qcom_iris_queue_read(&queues, QcomIrisMsgQ, in, sizeof in) == 0);
    firmware_send(buffer, 104);
    assert(qcom_iris_queue_read(&queues, QcomIrisMsgQ, in, sizeof in) == 104 && !memcmp(in, buffer, 104));
    header(QcomIrisMsgQ)->read_idx = header(QcomIrisMsgQ)->write_idx = (QCOM_IRIS_IFACEQ_QUEUE_SIZE - 8) / 4;
    firmware_send(buffer, 104);
    assert(qcom_iris_queue_read(&queues, QcomIrisMsgQ, in, sizeof in) == 104 && !memcmp(in, buffer, 104));
    uint32_t huge[2] = {QCOM_IRIS_CORE_PKT_SIZE + 4, 0};
    firmware_send(huge, 8);
    assert(qcom_iris_queue_read(&queues, QcomIrisMsgQ, in, sizeof in) == -1);
    assert(header(QcomIrisMsgQ)->read_idx == header(QcomIrisMsgQ)->write_idx && header(QcomIrisMsgQ)->rx_req == 1);

    /* a message whose packet runs past its header's size is refused */
    ((struct qcom_iris_hfi_packet *)(buffer + 32))->size = 4000;
    assert(!qcom_iris_hfi_valid(buffer, 104));
    printf("hfi: queue table, command and message queues, packets: ok\n");
    free(memory);
    return 0;
}
