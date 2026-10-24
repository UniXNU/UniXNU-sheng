/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* GLINK over shared memory: the native GLINK protocol (version handshake, channel open/close, receive intents, data, done) on a pair of
 * SMEM FIFOs. Event driven and allocation free: the owner calls qcom_glink_poll whenever the remote rings the GLINK doorbell (and after
 * sending), every call does a bounded amount of work, and nothing here waits. A send that cannot go out yet (no remote intent, no FIFO
 * room) returns QcomGlinkAgain and the owner tries again on the next event; an intent is asked for once per pending send. */
#ifndef QCOM_GLINK_H
#define QCOM_GLINK_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define QCOM_GLINK_MAX_CHANNELS 6u
#define QCOM_GLINK_MAX_REMOTE_INTENTS 24u
#define QCOM_GLINK_MAX_LOCAL_INTENTS 24u
#define QCOM_GLINK_NAME 32u
#define QCOM_GLINK_FEATURE_INTENT_REUSE 1u

enum {
    QCOM_GLINK_VERSION = 0, QCOM_GLINK_VERSION_ACK = 1, QCOM_GLINK_OPEN = 2, QCOM_GLINK_CLOSE = 3, QCOM_GLINK_OPEN_ACK = 4,
    QCOM_GLINK_INTENT = 5, QCOM_GLINK_RX_DONE = 6, QCOM_GLINK_RX_INTENT_REQ = 7, QCOM_GLINK_RX_INTENT_REQ_ACK = 8,
    QCOM_GLINK_TX_DATA = 9, QCOM_GLINK_CLOSE_ACK = 11, QCOM_GLINK_TX_DATA_CONT = 12, QCOM_GLINK_READ_NOTIF = 13,
    QCOM_GLINK_RX_DONE_W_REUSE = 14, QCOM_GLINK_SIGNALS = 15,
};

enum {
    QcomGlinkOk = 0,
    QcomGlinkAgain = -1,    /* not now: no intent or no FIFO room yet */
    QcomGlinkNotOpen = -2,
    QcomGlinkTooBig = -3,
    QcomGlinkFull = -4,     /* no channel slot */
    QcomGlinkCorrupt = -5,  /* the receive FIFO holds something that cannot be a message: the link is stopped */
    QcomGlinkArgument = -6,
};

/* One direction's FIFO in SMEM: indices are byte offsets into data. */
typedef struct {
    volatile uint32_t *head, *tail;
    volatile uint8_t *data;
    uint32_t length;
} QcomGlinkFifo;

typedef struct QcomGlinkChannel QcomGlinkChannel;
typedef struct {
    void (*state)(void *context, QcomGlinkChannel *channel, int up);
    /* A complete message; data stays valid for the duration of the call only. */
    void (*receive)(void *context, QcomGlinkChannel *channel, const uint8_t *data, uint32_t length);
} QcomGlinkClient;

struct QcomGlinkChannel {
    char name[QCOM_GLINK_NAME];
    uint16_t lcid, rcid;
    uint8_t used, initiator, open_sent, open_acked, remote_open, up, intent_asked;
    QcomGlinkClient client;
    void *context;
    /* Our receive intents: count buffers of size bytes in storage (count * size). */
    uint8_t *storage;
    uint32_t intent_size, intent_count;
    struct {uint32_t fill; uint8_t advertised, busy;} local[QCOM_GLINK_MAX_LOCAL_INTENTS];
    /* The remote's intents we may send into. */
    struct {uint32_t id, size; uint8_t valid, busy;} remote[QCOM_GLINK_MAX_REMOTE_INTENTS];
};

typedef struct {
    QcomGlinkFifo tx, rx;
    void (*kick)(void *context);
    void *kick_context;
    uint32_t features;
    uint8_t version_sent, version_agreed, broken, kick_pending, notify_sent;
    uint16_t next_lcid;
    QcomGlinkChannel channel[QCOM_GLINK_MAX_CHANNELS];
    /* Counters for the log. */
    uint32_t rx_messages, tx_messages, rx_data, unknown_opens, dropped;
    char last_unknown[QCOM_GLINK_NAME];
    char unknown_names[96];  /* the distinct names of those opens, space separated, as many as fit */
} QcomGlink;

/* Sets up the link on its FIFOs (rx may have no data yet: the remote makes its FIFO later) and resets the indices we own. */
void qcom_glink_init(QcomGlink *g, QcomGlinkFifo tx, QcomGlinkFifo rx, void (*kick)(void *), void *kick_context);
/* The remote's FIFO once it exists. */
void qcom_glink_set_rx(QcomGlink *g, volatile uint8_t *data, uint32_t length);
/* Registers a channel by name with intent_count receive buffers of intent_size bytes in storage. open_now: we send OPEN ourselves;
 * otherwise we answer when the remote opens it. */
QcomGlinkChannel *qcom_glink_channel(QcomGlink *g, const char *name, uint32_t intent_size, uint32_t intent_count, uint8_t *storage,
                                     const QcomGlinkClient *client, void *context, int open_now);
/* Sends VERSION (once) and any channel OPEN that is due, then handles every complete message in the receive FIFO. Returns the number of
 * messages handled or QcomGlinkCorrupt. Rings the remote when anything was written. */
int qcom_glink_poll(QcomGlink *g);
/* Queues one message on an open channel (single chunk, up to the remote intent size). */
int qcom_glink_send(QcomGlink *g, QcomGlinkChannel *channel, const void *data, uint32_t length);

/* FIFO primitives, exposed for the tests. */
uint32_t qcom_glink_rx_avail(const QcomGlinkFifo *f);
uint32_t qcom_glink_tx_avail(const QcomGlinkFifo *f);

#ifdef __cplusplus
}
#endif
#endif
