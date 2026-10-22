/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomQmp.h"

/* Descriptor words (byte offset / 4): magic, version, features; the AOP's link state, its ack, channel state, its ack, mailbox size and
 * offset; then the same six for our side. */
#define MAGIC 0x4d41494cu
#define D_MAGIC 0
#define D_VERSION 1
#define D_UCORE_LINK 3
#define D_UCORE_LINK_ACK 4
#define D_UCORE_CH 5
#define D_UCORE_CH_ACK 6
#define D_MCORE_LINK 9
#define D_MCORE_LINK_ACK 10
#define D_MCORE_CH 11
#define D_MCORE_CH_ACK 12
#define D_MCORE_SIZE 13
#define D_MCORE_OFFSET 14
#define UP 0x0000ffffu

static void barrier(void)
{
#if defined(__aarch64__) && defined(KERNEL)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}
static void kick(QcomQmp *q) {barrier(); if (q->kick) q->kick(q->context);}

void qcom_qmp_init(QcomQmp *q, volatile uint32_t *ram, uint32_t ram_bytes, void (*k)(void *), void *context)
{
    *q = (QcomQmp){ram, ram_bytes, 0, 0, QcomQmpIdle, k, context};
}

int qcom_qmp_step(QcomQmp *q)
{
    volatile uint32_t *r = q->ram;
    switch (q->state) {
    case QcomQmpIdle: {
        if (r[D_MAGIC] != MAGIC || r[D_VERSION] != 1) return q->state = QcomQmpBadDescriptor;
        const uint32_t box = r[D_MCORE_OFFSET], size = r[D_MCORE_SIZE];
        if ((box & 3) || size < QCOM_QMP_MESSAGE + 4 || box < 0x3c || box > q->ram_bytes || size > q->ram_bytes - box)
            return q->state = QcomQmpBadDescriptor;
        q->box = box;
        q->box_size = size;
        /* A channel a boot stage left open is used as it is. */
        if (r[D_MCORE_LINK] == UP && r[D_MCORE_LINK_ACK] == UP && r[D_MCORE_CH] == UP && r[D_UCORE_CH] == UP && r[D_MCORE_CH_ACK] == UP)
            return q->state = QcomQmpOpen;
        r[D_UCORE_LINK_ACK] = r[D_UCORE_LINK];
        r[D_MCORE_LINK] = UP;
        kick(q);
        q->state = QcomQmpLinkWait;
    }
        /* fall through: the ack may already be there */
    case QcomQmpLinkWait:
        if (r[D_MCORE_LINK_ACK] != UP) return q->state;
        r[D_MCORE_CH] = UP;
        kick(q);
        q->state = QcomQmpChannelWait;
        /* fall through */
    case QcomQmpChannelWait:
        if (r[D_UCORE_CH] != UP) return q->state;
        r[D_UCORE_CH_ACK] = UP;
        kick(q);
        q->state = QcomQmpAckWait;
        /* fall through */
    case QcomQmpAckWait:
        if (r[D_MCORE_CH_ACK] != UP) return q->state;
        return q->state = QcomQmpOpen;
    case QcomQmpSending:
        if (r[q->box / 4] != 0) return q->state;
        return q->state = QcomQmpOpen;
    default:
        return q->state;
    }
}

int qcom_qmp_send(QcomQmp *q, const char *text)
{
    if (q->state != QcomQmpOpen) return QcomQmpBusy;
    uint32_t words[QCOM_QMP_MESSAGE / 4] = {0};
    unsigned n = 0;
    while (text[n]) {
        if (n >= QCOM_QMP_MESSAGE - 1) return QcomQmpTooLong;
        words[n / 4] |= (uint32_t)(uint8_t)text[n] << (8 * (n % 4));
        ++n;
    }
    volatile uint32_t *r = q->ram;
    if (r[q->box / 4] != 0) return QcomQmpBusy;
    for (unsigned i = 0; i < QCOM_QMP_MESSAGE / 4; ++i) r[q->box / 4 + 1 + i] = words[i];
    barrier();
    r[q->box / 4] = QCOM_QMP_MESSAGE;
    kick(q);
    return q->state = QcomQmpSending;
}

void qcom_qmp_abandon(QcomQmp *q)
{
    if (q->state != QcomQmpSending) return;
    q->ram[q->box / 4] = 0;
    barrier();
    q->state = QcomQmpOpen;
}
