/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The AOSS QMP mailbox: a message RAM shared with the always-on processor, a link and channel handshake, then one text message at a
 * time ("{class: image, res: load_state, name: adsp, val: on}"). Written as a state machine stepped on each doorbell from the AOP (or a
 * timer): qcom_qmp_step never waits, it returns what it is waiting for and the owner calls it again when the AOP rings or the deadline
 * passes. Ringing the AOP is the owner's kick callback. */
#ifndef QCOM_QMP_H
#define QCOM_QMP_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define QCOM_QMP_MESSAGE 0x64u  /* message length the AOP takes (padded with zeroes) */

enum {
    QcomQmpIdle = 0,         /* closed, nothing started */
    QcomQmpLinkWait = 1,     /* our link is up, waiting for the AOP's ack */
    QcomQmpChannelWait = 2,  /* our channel is up, waiting for the AOP's channel */
    QcomQmpAckWait = 3,      /* the AOP's channel acked, waiting for its ack of ours */
    QcomQmpOpen = 4,
    QcomQmpSending = 5,      /* a message is in the mailbox, waiting for the AOP to take it */
    QcomQmpBadDescriptor = -1,
    QcomQmpBusy = -2,
    QcomQmpTooLong = -3,
};

typedef struct {
    volatile uint32_t *ram;  /* the message RAM (0x400 bytes on SM8550) */
    uint32_t ram_bytes;
    uint32_t box, box_size;  /* our mailbox inside it */
    int state;
    void (*kick)(void *context);
    void *context;
} QcomQmp;

void qcom_qmp_init(QcomQmp *q, volatile uint32_t *ram, uint32_t ram_bytes, void (*kick)(void *), void *context);
/* Advances the handshake or the pending send; returns the state reached. */
int qcom_qmp_step(QcomQmp *q);
/* Puts a message in the mailbox once the channel is open (QcomQmpSending), QcomQmpBusy otherwise. */
int qcom_qmp_send(QcomQmp *q, const char *text);
/* Gives up a send the AOP did not take before the owner's deadline: the mailbox is cleared. */
void qcom_qmp_abandon(QcomQmp *q);

#ifdef __cplusplus
}
#endif
#endif
