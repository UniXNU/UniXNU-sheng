/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomGlink.h"

/* The sender keeps 8 bytes so a full FIFO never looks empty, and 16 more for a READ_NOTIF when it runs out of room. */
#define FULL_RESERVE 8u
#define BLOCKED_RESERVE 16u
#define GLINK_VERSION_1 1u

static void barrier(void)
{
#if defined(__aarch64__) && defined(KERNEL)
    __asm__ volatile("dmb osh" ::: "memory");
#endif
}
static uint32_t align8(uint32_t v) {return (v + 7u) & ~7u;}
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static void put32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}
static size_t name_length(const char *s) {size_t n = 0; while (n < QCOM_GLINK_NAME - 1 && s[n]) ++n; return n;}

uint32_t qcom_glink_rx_avail(const QcomGlinkFifo *f)
{
    if (!f->data || !f->length) return 0;
    const uint32_t head = *f->head, tail = *f->tail;
    if (head >= f->length || tail >= f->length) return 0;
    return head >= tail ? head - tail : f->length - tail + head;
}

uint32_t qcom_glink_tx_avail(const QcomGlinkFifo *f)
{
    const uint32_t head = *f->head, tail = *f->tail;
    if (head >= f->length || tail >= f->length) return 0;
    const uint32_t avail = tail <= head ? f->length - head + tail : tail - head;
    return avail < FULL_RESERVE + BLOCKED_RESERVE ? 0 : avail - FULL_RESERVE - BLOCKED_RESERVE;
}

static void rx_peek(const QcomGlinkFifo *f, uint32_t offset, uint8_t *to, uint32_t count)
{
    uint32_t at = *f->tail + offset;
    if (at >= f->length) at -= f->length;
    for (uint32_t i = 0; i < count; ++i) {
        to[i] = f->data[at];
        if (++at == f->length) at = 0;
    }
}

static void rx_advance(QcomGlinkFifo *f, uint32_t count)
{
    uint32_t tail = *f->tail + count;
    if (tail >= f->length) tail -= f->length;
    barrier();
    *f->tail = tail;
}

/* Writes header and payload as one message (padded to 8 bytes); the caller checked the room. */
static void tx_write(QcomGlink *g, const uint8_t *header, uint32_t header_length, const uint8_t *data, uint32_t data_length)
{
    QcomGlinkFifo *f = &g->tx;
    uint32_t head = *f->head;
    for (uint32_t i = 0; i < header_length; ++i) {f->data[head] = header[i]; if (++head == f->length) head = 0;}
    for (uint32_t i = 0; i < data_length; ++i) {f->data[head] = data[i]; if (++head == f->length) head = 0;}
    head = align8(head);
    if (head >= f->length) head -= f->length;
    barrier();
    *f->head = head;
    g->tx_messages++;
    g->kick_pending = 1;
}

static int tx(QcomGlink *g, const uint8_t *header, uint32_t header_length, const uint8_t *data, uint32_t data_length)
{
    if (align8(header_length + data_length) > qcom_glink_tx_avail(&g->tx)) return QcomGlinkAgain;
    tx_write(g, header, header_length, data, data_length);
    return QcomGlinkOk;
}

static int command(QcomGlink *g, uint16_t cmd, uint16_t param1, uint32_t param2)
{
    uint8_t m[8];
    m[0] = (uint8_t)cmd; m[1] = (uint8_t)(cmd >> 8); m[2] = (uint8_t)param1; m[3] = (uint8_t)(param1 >> 8);
    put32(m + 4, param2);
    return tx(g, m, 8, 0, 0);
}

void qcom_glink_init(QcomGlink *g, QcomGlinkFifo txf, QcomGlinkFifo rxf, void (*kick)(void *), void *kick_context)
{
    for (size_t i = 0; i < sizeof *g; ++i) ((uint8_t *)g)[i] = 0;
    g->tx = txf;
    g->rx = rxf;
    g->kick = kick;
    g->kick_context = kick_context;
    g->features = QCOM_GLINK_FEATURE_INTENT_REUSE;
    g->next_lcid = 1;
    *g->tx.head = 0;
    *g->rx.tail = 0;
    barrier();
}

void qcom_glink_set_rx(QcomGlink *g, volatile uint8_t *data, uint32_t length)
{
    g->rx.data = data;
    g->rx.length = length;
}

QcomGlinkChannel *qcom_glink_channel(QcomGlink *g, const char *name, uint32_t intent_size, uint32_t intent_count, uint8_t *storage,
                                     const QcomGlinkClient *client, void *context, int open_now)
{
    if (!name || !client || intent_count > QCOM_GLINK_MAX_LOCAL_INTENTS || (intent_count && (!storage || !intent_size))) return 0;
    for (unsigned i = 0; i < QCOM_GLINK_MAX_CHANNELS; ++i) {
        QcomGlinkChannel *c = &g->channel[i];
        if (c->used) continue;
        for (size_t b = 0; b < sizeof *c; ++b) ((uint8_t *)c)[b] = 0;
        const size_t n = name_length(name);
        for (size_t b = 0; b < n; ++b) c->name[b] = name[b];
        c->used = 1;
        c->lcid = g->next_lcid++;
        c->client = *client;
        c->context = context;
        c->storage = storage;
        c->intent_size = intent_size;
        c->intent_count = intent_count;
        c->initiator = open_now != 0;
        c->open_sent = open_now ? 0 : 2;  /* 2: wait for the remote's OPEN before sending ours */
        return c;
    }
    return 0;
}

static QcomGlinkChannel *by_lcid(QcomGlink *g, uint32_t lcid)
{
    for (unsigned i = 0; i < QCOM_GLINK_MAX_CHANNELS; ++i)
        if (g->channel[i].used && g->channel[i].lcid == lcid) return &g->channel[i];
    return 0;
}
static QcomGlinkChannel *by_rcid(QcomGlink *g, uint32_t rcid)
{
    for (unsigned i = 0; i < QCOM_GLINK_MAX_CHANNELS; ++i)
        if (g->channel[i].used && g->channel[i].remote_open && g->channel[i].rcid == rcid) return &g->channel[i];
    return 0;
}

static int send_open(QcomGlink *g, QcomGlinkChannel *c)
{
    uint8_t m[8];
    const uint32_t n = (uint32_t)name_length(c->name) + 1;
    m[0] = QCOM_GLINK_OPEN; m[1] = 0; m[2] = (uint8_t)c->lcid; m[3] = (uint8_t)(c->lcid >> 8);
    put32(m + 4, n);
    if (align8(8 + n) > qcom_glink_tx_avail(&g->tx)) return QcomGlinkAgain;
    tx_write(g, m, 8, (const uint8_t *)c->name, n);
    c->open_sent = 1;
    return QcomGlinkOk;
}

static int advertise(QcomGlink *g, QcomGlinkChannel *c, unsigned index)
{
    uint8_t m[16];
    m[0] = QCOM_GLINK_INTENT; m[1] = 0; m[2] = (uint8_t)c->lcid; m[3] = (uint8_t)(c->lcid >> 8);
    put32(m + 4, 1);
    put32(m + 8, c->intent_size);
    put32(m + 12, index + 1);
    const int rc = tx(g, m, 16, 0, 0);
    if (!rc) {c->local[index].advertised = 1; c->local[index].fill = 0;}
    return rc;
}

static void reset_channel(QcomGlinkChannel *c)
{
    c->open_acked = c->remote_open = c->up = c->intent_asked = 0;
    c->open_sent = c->initiator ? 0 : 2;
    for (unsigned i = 0; i < QCOM_GLINK_MAX_LOCAL_INTENTS; ++i) c->local[i].advertised = c->local[i].busy = 0, c->local[i].fill = 0;
    for (unsigned i = 0; i < QCOM_GLINK_MAX_REMOTE_INTENTS; ++i) c->remote[i].valid = 0;
}

/* Channel bring-up and our intents: whatever is due and fits in the FIFO now. */
static void progress(QcomGlink *g)
{
    if (!g->version_sent) {
        if (command(g, QCOM_GLINK_VERSION, GLINK_VERSION_1, g->features)) return;
        g->version_sent = 1;
    }
    if (!g->version_agreed) return;
    for (unsigned i = 0; i < QCOM_GLINK_MAX_CHANNELS; ++i) {
        QcomGlinkChannel *c = &g->channel[i];
        if (!c->used) continue;
        if (c->open_sent == 0 || (c->open_sent == 2 && c->remote_open)) {
            if (send_open(g, c)) return;
        }
        if (c->open_sent == 1 && c->open_acked && c->remote_open && !c->up) {
            for (unsigned k = 0; k < c->intent_count; ++k)
                if (!c->local[k].advertised && advertise(g, c, k)) return;
            c->up = 1;
            if (c->client.state) c->client.state(c->context, c, 1);
        }
        if (c->up)
            for (unsigned k = 0; k < c->intent_count; ++k)
                if (!c->local[k].advertised && !c->local[k].busy && advertise(g, c, k)) return;
    }
}

static int handle_data(QcomGlink *g, uint16_t cmd, uint32_t rcid, uint32_t liid, uint32_t avail)
{
    uint8_t h[16];
    if (avail < 16) return 0;
    rx_peek(&g->rx, 0, h, 16);
    const uint32_t chunk = le32(h + 8), left = le32(h + 12);
    if (chunk > g->rx.length || align8(16 + chunk) < chunk) return QcomGlinkCorrupt;
    if (avail < 16 + chunk) return 0;  /* not all there yet */
    QcomGlinkChannel *c = by_rcid(g, rcid);
    const unsigned index = liid - 1;
    if (!c || !liid || index >= c->intent_count || !c->local[index].advertised) {
        g->dropped++;
    } else {
        uint32_t fill = cmd == QCOM_GLINK_TX_DATA ? 0 : c->local[index].fill;
        if ((uint64_t)fill + chunk + left > c->intent_size) {
            g->dropped++;
            fill = 0;
        } else {
            rx_peek(&g->rx, 16, c->storage + (size_t)index * c->intent_size + fill, chunk);
            fill += chunk;
            if (!left) {
                c->local[index].busy = 1;
                g->rx_data++;
                if (c->client.receive) c->client.receive(c->context, c, c->storage + (size_t)index * c->intent_size, fill);
                c->local[index].busy = 0;
                fill = 0;
                /* The intent is ours again once the remote knows it is done; with reuse it stays advertised. */
                const int reuse = (g->features & QCOM_GLINK_FEATURE_INTENT_REUSE) != 0;
                if (command(g, reuse ? QCOM_GLINK_RX_DONE_W_REUSE : QCOM_GLINK_RX_DONE, c->lcid, liid) == QcomGlinkOk && !reuse)
                    c->local[index].advertised = 0;
            }
        }
        c->local[index].fill = fill;
    }
    rx_advance(&g->rx, align8(16 + chunk));
    return 1;
}

/* One message at the FIFO tail: 1 handled, 0 incomplete (wait for more), negative on a broken stream. */

/* Adds name to the list of opens nobody serves unless it is there already or does not fit. */
static void note_unknown(QcomGlink *g, const char *name)
{
    char *list = g->unknown_names;
    const unsigned cap = sizeof g->unknown_names;
    unsigned n = 0, used = 0;
    while (n < QCOM_GLINK_NAME && name[n]) ++n;
    while (used < cap && list[used]) ++used;
    for (unsigned at = 0; at < used;) {
        unsigned end = at;
        while (end < used && list[end] != ' ') ++end;
        unsigned same = end - at == n;
        for (unsigned b = 0; same && b < n; ++b) same = list[at + b] == name[b];
        if (same) return;
        at = end + 1;
    }
    if (!n || used + (used ? 1 : 0) + n + 1 > cap) return;
    if (used) list[used++] = ' ';
    for (unsigned b = 0; b < n; ++b) list[used++] = name[b];
    list[used] = 0;
}
static int receive_one(QcomGlink *g)
{
    const uint32_t avail = qcom_glink_rx_avail(&g->rx);
    uint8_t m[8];
    if (avail < 8) return 0;
    rx_peek(&g->rx, 0, m, 8);
    const uint16_t cmd = (uint16_t)(m[0] | m[1] << 8), param1 = (uint16_t)(m[2] | m[3] << 8);
    const uint32_t param2 = le32(m + 4);
    QcomGlinkChannel *c;
    switch (cmd) {
    case QCOM_GLINK_VERSION:
        if (param1 == GLINK_VERSION_1) g->features &= param2;
        if (param1 && command(g, QCOM_GLINK_VERSION_ACK, GLINK_VERSION_1, g->features)) return 0;
        g->version_agreed = param1 != 0;
        break;
    case QCOM_GLINK_VERSION_ACK:
        if (param1 == GLINK_VERSION_1 && param2 != g->features) {
            g->features &= param2;
            if (command(g, QCOM_GLINK_VERSION, GLINK_VERSION_1, g->features)) return 0;
        }
        g->version_agreed = param1 != 0;
        break;
    case QCOM_GLINK_OPEN: {
        const uint32_t n = param2 & 0xffff;
        if (n > g->rx.length || n == 0) return QcomGlinkCorrupt;
        if (avail < 8 + n) return 0;
        char name[QCOM_GLINK_NAME] = {0};
        rx_peek(&g->rx, 8, (uint8_t *)name, n < QCOM_GLINK_NAME ? n : QCOM_GLINK_NAME - 1);
        name[QCOM_GLINK_NAME - 1] = 0;
        c = 0;
        for (unsigned i = 0; i < QCOM_GLINK_MAX_CHANNELS && !c; ++i) {
            QcomGlinkChannel *k = &g->channel[i];
            unsigned same = k->used;
            for (unsigned b = 0; same && b < QCOM_GLINK_NAME; ++b) {
                if (k->name[b] != name[b]) same = 0;
                if (!name[b]) break;
            }
            if (same) c = k;
        }
        if (!c) {
            g->unknown_opens++;
            for (unsigned b = 0; b < QCOM_GLINK_NAME; ++b) g->last_unknown[b] = name[b];
            note_unknown(g, name);
        } else {
            if (c->remote_open && c->up && c->client.state) c->client.state(c->context, c, 0);
            if (c->remote_open) reset_channel(c);
            if (command(g, QCOM_GLINK_OPEN_ACK, param1, 0)) return 0;
            c->rcid = param1;
            c->remote_open = 1;
        }
        rx_advance(&g->rx, align8(8 + n));
        g->rx_messages++;
        return 1;
    }
    case QCOM_GLINK_OPEN_ACK:
        if ((c = by_lcid(g, param1)) && c->open_sent == 1) c->open_acked = 1;
        break;
    case QCOM_GLINK_CLOSE:
        if (command(g, QCOM_GLINK_CLOSE_ACK, param1, 0)) return 0;
        if ((c = by_rcid(g, param1))) {
            const int was = c->up;
            if (c->open_sent == 1 && command(g, QCOM_GLINK_CLOSE, c->lcid, 0)) return 0;
            reset_channel(c);
            if (was && c->client.state) c->client.state(c->context, c, 0);
        }
        break;
    case QCOM_GLINK_CLOSE_ACK:
        break;
    case QCOM_GLINK_INTENT: {
        if (param2 > (g->rx.length - 8) / 8) return QcomGlinkCorrupt;
        const uint32_t bytes = 8 + 8 * param2;
        if (avail < bytes) return 0;
        c = by_rcid(g, param1);
        for (uint32_t i = 0; i < param2; ++i) {
            uint8_t pair[8];
            rx_peek(&g->rx, 8 + 8 * i, pair, 8);
            if (!c) continue;
            unsigned slot = QCOM_GLINK_MAX_REMOTE_INTENTS;
            for (unsigned k = 0; k < QCOM_GLINK_MAX_REMOTE_INTENTS; ++k)
                if (!c->remote[k].valid) {slot = k; break;}
            if (slot == QCOM_GLINK_MAX_REMOTE_INTENTS) {g->dropped++; continue;}
            c->remote[slot].size = le32(pair);
            c->remote[slot].id = le32(pair + 4);
            c->remote[slot].valid = 1;
            c->remote[slot].busy = 0;
        }
        if (c) c->intent_asked = 0;
        rx_advance(&g->rx, align8(bytes));
        g->rx_messages++;
        return 1;
    }
    case QCOM_GLINK_RX_DONE:
    case QCOM_GLINK_RX_DONE_W_REUSE:
        if ((c = by_rcid(g, param1)))
            for (unsigned k = 0; k < QCOM_GLINK_MAX_REMOTE_INTENTS; ++k)
                if (c->remote[k].valid && c->remote[k].id == param2) {
                    c->remote[k].busy = 0;
                    if (cmd == QCOM_GLINK_RX_DONE) c->remote[k].valid = 0;
                    break;
                }
        break;
    case QCOM_GLINK_RX_INTENT_REQ: {
        /* Grant it if a buffer of ours that is big enough is not advertised (all are, normally: then refuse). */
        int granted = 0;
        if ((c = by_rcid(g, param1)) && c->up && param2 <= c->intent_size)
            for (unsigned k = 0; k < c->intent_count; ++k)
                if (!c->local[k].advertised && !c->local[k].busy) {
                    if (advertise(g, c, k)) return 0;
                    granted = 1;
                    break;
                }
        if (command(g, QCOM_GLINK_RX_INTENT_REQ_ACK, c ? c->lcid : param1, (uint32_t)granted)) return 0;
        break;
    }
    case QCOM_GLINK_RX_INTENT_REQ_ACK:
        if ((c = by_rcid(g, param1)) && !param2) c->intent_asked = 0;
        break;
    case QCOM_GLINK_TX_DATA:
    case QCOM_GLINK_TX_DATA_CONT: {
        const int rc = handle_data(g, cmd, param1, param2, avail);
        if (rc == 1) g->rx_messages++;
        return rc;
    }
    case QCOM_GLINK_READ_NOTIF:
        g->kick_pending = 1;
        break;
    case QCOM_GLINK_SIGNALS:
        break;
    default:
        return QcomGlinkCorrupt;
    }
    rx_advance(&g->rx, 8);
    g->rx_messages++;
    return 1;
}

int qcom_glink_poll(QcomGlink *g)
{
    if (g->broken) return QcomGlinkCorrupt;
    int handled = 0;
    g->notify_sent = 0;
    progress(g);
    /* Bounded: at most one FIFO's worth of minimal messages per call. */
    for (uint32_t budget = g->rx.length / 8 + 1; g->rx.data && budget; --budget) {
        const int rc = receive_one(g);
        if (rc < 0) {g->broken = 1; handled = rc; break;}
        if (!rc) break;
        ++handled;
    }
    if (!g->broken) progress(g);
    if (g->kick_pending && g->kick) {g->kick_pending = 0; g->kick(g->kick_context);}
    return handled;
}

int qcom_glink_send(QcomGlink *g, QcomGlinkChannel *c, const void *data, uint32_t length)
{
    if (!g || !c || (!data && length)) return QcomGlinkArgument;
    if (g->broken || !c->up) return QcomGlinkNotOpen;
    if (align8(16 + length) > g->tx.length / 2) return QcomGlinkTooBig;
    unsigned best = QCOM_GLINK_MAX_REMOTE_INTENTS;
    for (unsigned k = 0; k < QCOM_GLINK_MAX_REMOTE_INTENTS; ++k)
        if (c->remote[k].valid && !c->remote[k].busy && c->remote[k].size >= length &&
            (best == QCOM_GLINK_MAX_REMOTE_INTENTS || c->remote[k].size < c->remote[best].size))
            best = k;
    if (best == QCOM_GLINK_MAX_REMOTE_INTENTS) {
        if (!c->intent_asked && command(g, QCOM_GLINK_RX_INTENT_REQ, c->lcid, length) == QcomGlinkOk) c->intent_asked = 1;
        if (g->kick_pending && g->kick) {g->kick_pending = 0; g->kick(g->kick_context);}
        return QcomGlinkAgain;
    }
    uint8_t h[16];
    h[0] = QCOM_GLINK_TX_DATA; h[1] = 0; h[2] = (uint8_t)c->lcid; h[3] = (uint8_t)(c->lcid >> 8);
    put32(h + 4, c->remote[best].id);
    put32(h + 8, length);
    put32(h + 12, 0);
    if (tx(g, h, 16, (const uint8_t *)data, length)) {
        /* Ask the remote to ring us once it has read, so the owner retries then. */
        const uint32_t head = *g->tx.head, tail = *g->tx.tail;
        if (!g->notify_sent && head < g->tx.length && tail < g->tx.length) {
            uint8_t m[8] = {QCOM_GLINK_READ_NOTIF, 0, 0, 0, 0, 0, 0, 0};
            const uint32_t raw = tail <= head ? g->tx.length - head + tail : tail - head;
            if (raw >= FULL_RESERVE + 8) {tx_write(g, m, 8, 0, 0); g->notify_sent = 1;}
        }
        if (g->kick_pending && g->kick) {g->kick_pending = 0; g->kick(g->kick_context);}
        return QcomGlinkAgain;
    }
    c->remote[best].busy = 1;
    if (g->kick) {g->kick_pending = 0; g->kick(g->kick_context);}
    return QcomGlinkOk;
}
