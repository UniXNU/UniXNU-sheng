// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the IPC router side (QcomQrtr.c): hello and service announcements, routing of data packets to our ports, flow control
// answers, the remote's service list, and the service locator's answer bytes. No hardware: sent packets are captured in a buffer.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../QcomQrtr.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

static uint32_t rd32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint32_t rd16(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8;}
static void wr32(uint8_t *p, uint32_t v) {for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));}

typedef struct {uint8_t packet[16][512]; uint32_t length[16]; unsigned count, refuse;} Wire;
static int capture(void *context, const uint8_t *packet, uint32_t length)
{
    Wire *w = context;
    if (w->refuse || w->count == 16) return -1;
    memcpy(w->packet[w->count], packet, length);
    w->length[w->count++] = length;
    return 0;
}

static uint32_t make(uint8_t *p, uint32_t type, uint32_t src_node, uint32_t src_port, uint32_t confirm, uint32_t dst_node, uint32_t dst_port,
                     const uint8_t *data, uint32_t size)
{
    wr32(p, 1); wr32(p + 4, type); wr32(p + 8, src_node); wr32(p + 12, src_port); wr32(p + 16, confirm); wr32(p + 20, size);
    wr32(p + 24, dst_node); wr32(p + 28, dst_port);
    memcpy(p + 32, data, size);
    const uint32_t padded = (size + 3) & ~3u;
    memset(p + 32 + size, 0, padded - size);
    return 32 + padded;
}
static uint32_t make_ctrl(uint8_t *p, uint32_t cmd, uint32_t node, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint8_t pkt[20];
    wr32(pkt, cmd); wr32(pkt + 4, a); wr32(pkt + 8, b); wr32(pkt + 12, c); wr32(pkt + 16, d);
    return make(p, cmd, node, QCOM_QRTR_PORT_CTRL, 0, 1, QCOM_QRTR_PORT_CTRL, pkt, sizeof pkt);
}

static struct {uint32_t port, node, from, length; uint8_t data[64]; unsigned calls;} got;
static void on_data(void *context, QcomQrtr *q, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    (void)context; (void)q;
    got.port = port; got.node = node; got.from = from; got.length = length; got.calls++;
    memcpy(got.data, data, length < sizeof got.data ? length : sizeof got.data);
}

static void router(void)
{
    static Wire w;
    QcomQrtr q;
    qcom_qrtr_init(&q, 1, capture, &w);
    CHECK(qcom_qrtr_add_service(&q, QCOM_SERVLOC_SERVICE, QCOM_SERVLOC_INSTANCE, 1, on_data, NULL) == 0);
    CHECK(w.count == 0);  /* nothing is announced before the remote's hello */
    qcom_qrtr_start(&q);
    CHECK(w.count == 1 && q.hello_sent && w.length[0] == 32 + 20);
    const uint8_t *h = w.packet[0];
    CHECK(rd32(h) == 1 && rd32(h + 4) == QCOM_QRTR_HELLO && rd32(h + 8) == 1 && rd32(h + 12) == QCOM_QRTR_PORT_CTRL && rd32(h + 16) == 0);
    CHECK(rd32(h + 20) == 20 && rd32(h + 24) == QCOM_QRTR_NODE_BROADCAST && rd32(h + 28) == QCOM_QRTR_PORT_CTRL && rd32(h + 32) == QCOM_QRTR_HELLO);

    /* the remote (node 5) says hello: our service is announced to its control port */
    uint8_t p[512];
    uint32_t n = make_ctrl(p, QCOM_QRTR_HELLO, 5, 0, 0, 0, 0);
    CHECK(qcom_qrtr_receive(&q, p, n) == 0 && q.remote_node == 5 && q.hello_received);
    CHECK(w.count == 2);
    const uint8_t *a = w.packet[1];
    CHECK(rd32(a + 4) == QCOM_QRTR_NEW_SERVER && rd32(a + 24) == 5 && rd32(a + 28) == QCOM_QRTR_PORT_CTRL && rd32(a + 20) == 20);
    CHECK(rd32(a + 32) == QCOM_QRTR_NEW_SERVER && rd32(a + 36) == 0x40 && rd32(a + 40) == 0x101 && rd32(a + 44) == 1 && rd32(a + 48) == 1);
    /* a service added later is announced at once */
    CHECK(qcom_qrtr_add_service(&q, 0x99, 1, 2, on_data, NULL) == 0 && w.count == 3 && rd32(w.packet[2] + 36) == 0x99);

    /* the remote's services: added, updated, removed, and the empty end-of-listing record ignored */
    n = make_ctrl(p, QCOM_QRTR_NEW_SERVER, 5, 0x42, 0x4a01, 5, 33); qcom_qrtr_receive(&q, p, n);
    n = make_ctrl(p, QCOM_QRTR_NEW_SERVER, 5, 0x190, 0x1, 5, 34); qcom_qrtr_receive(&q, p, n);
    n = make_ctrl(p, QCOM_QRTR_NEW_SERVER, 5, 0, 0, 0, 0); qcom_qrtr_receive(&q, p, n);
    CHECK(q.remote_count == 2 && qcom_qrtr_find_remote(&q, 0x190) == 1 && q.remote[1].port == 34);
    n = make_ctrl(p, QCOM_QRTR_DEL_SERVER, 5, 0x42, 0x4a01, 5, 33); qcom_qrtr_receive(&q, p, n);
    CHECK(q.remote_count == 1 && qcom_qrtr_find_remote(&q, 0x42) == -1 && qcom_qrtr_find_remote(&q, 0x190) == 0);

    /* data to our port is delivered; with confirm_rx a resume goes back to the sender */
    const uint8_t msg[5] = {1, 2, 3, 4, 5};
    n = make(p, QCOM_QRTR_DATA, 5, 77, 1, 1, 1, msg, sizeof msg);
    CHECK(qcom_qrtr_receive(&q, p, n) == 0 && got.calls == 1 && got.port == 1 && got.node == 5 && got.from == 77 && got.length == 5);
    CHECK(!memcmp(got.data, msg, 5) && w.count == 4 && q.resumes_sent == 1);
    const uint8_t *r = w.packet[3];
    CHECK(rd32(r + 4) == QCOM_QRTR_RESUME_TX && rd32(r + 32) == QCOM_QRTR_RESUME_TX && rd32(r + 36) == 5 && rd32(r + 40) == 77);
    /* without confirm_rx no answer; data to a port nobody has is counted */
    n = make(p, QCOM_QRTR_DATA, 5, 77, 0, 1, 2, msg, 3);
    CHECK(qcom_qrtr_receive(&q, p, n) == 0 && got.calls == 2 && got.length == 3 && w.count == 4);
    n = make(p, QCOM_QRTR_DATA, 5, 77, 0, 1, 9, msg, 3);
    CHECK(qcom_qrtr_receive(&q, p, n) == 0 && got.calls == 2 && q.rx_unrouted == 1);

    /* our data: padded to four bytes, size the unpadded length */
    CHECK(qcom_qrtr_send_data(&q, 1, 5, 77, msg, 5) == 0 && w.length[4] == 40);
    CHECK(rd32(w.packet[4] + 4) == QCOM_QRTR_DATA && rd32(w.packet[4] + 20) == 5 && rd32(w.packet[4] + 24) == 5 && rd32(w.packet[4] + 28) == 77);
    CHECK(!memcmp(w.packet[4] + 32, msg, 5) && !w.packet[4][37] && !w.packet[4][38] && !w.packet[4][39]);
    w.refuse = 1;
    CHECK(qcom_qrtr_send_data(&q, 1, 5, 77, msg, 5) == -1 && q.tx_refused == 1);
    w.refuse = 0;

    /* malformed packets */
    n = make(p, QCOM_QRTR_DATA, 5, 77, 0, 1, 1, msg, 5);
    CHECK(qcom_qrtr_receive(&q, p, 31) == -1);
    wr32(p + 20, 1000);
    CHECK(qcom_qrtr_receive(&q, p, n) == -1);
    wr32(p, 2);
    CHECK(qcom_qrtr_receive(&q, p, n) == -1 && q.rx_bad == 3);
    /* bye forgets the remote's services and its hello */
    n = make_ctrl(p, QCOM_QRTR_BYE, 5, 0, 0, 0, 0);
    CHECK(qcom_qrtr_receive(&q, p, n) == 0 && q.remote_count == 0 && !q.hello_received);
}

static const char *const audio[] = {"avs/audio", NULL};
static const QcomPdDomain table[] = {
    {"msm/adsp/audio_pd", 74, audio}, {"msm/adsp/root_pd", 74, NULL}, {"msm/adsp/charger_pd", 74, NULL},
    {"msm/adsp/sensor_pd", 74, NULL}, {"msm/cdsp/root_pd", 76, NULL},
};

static uint32_t request(uint8_t *out, uint16_t txn, const char *name, int with_offset, uint32_t offset)
{
    const uint32_t n = (uint32_t)strlen(name);
    uint8_t *p = out + 7;
    p[0] = 1; p[1] = (uint8_t)n; p[2] = 0; memcpy(p + 3, name, n); p += 3 + n;
    if (with_offset) {p[0] = 0x10; p[1] = 4; p[2] = 0; wr32(p + 3, offset); p += 7;}
    out[0] = 0; out[1] = (uint8_t)txn; out[2] = (uint8_t)(txn >> 8); out[3] = 0x21; out[4] = 0;
    out[5] = (uint8_t)(p - out - 7); out[6] = 0;
    return (uint32_t)(p - out);
}

static void locator(void)
{
    uint8_t req[128], ans[1024];
    /* "avs/audio": one domain; the exact bytes */
    uint32_t n = qcom_servloc_answer(table, 5, ans, 0, ans, sizeof ans);
    CHECK(n == 0);
    n = qcom_servloc_answer(table, 5, req, request(req, 0x1234, "avs/audio", 0, 0), ans, sizeof ans);
    static const uint8_t expect[] = {
        2, 0x34, 0x12, 0x21, 0, 48, 0,
        0x02, 4, 0, 0, 0, 0, 0,
        0x10, 2, 0, 1, 0,
        0x11, 2, 0, 1, 0,
        0x12, 28, 0, 1, 17, 'm', 's', 'm', '/', 'a', 'd', 's', 'p', '/', 'a', 'u', 'd', 'i', 'o', '_', 'p', 'd', 74, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    CHECK(n == sizeof expect && !memcmp(ans, expect, n));
    /* "tms/servreg": every domain, in table order */
    n = qcom_servloc_answer(table, 5, req, request(req, 7, "tms/servreg", 0, 0), ans, sizeof ans);
    CHECK(n > 0 && ans[0] == 2 && rd16(ans + 1) == 7 && rd16(ans + 5) == n - 7);
    CHECK(ans[14] == 0x10 && rd16(ans + 17) == 5 && ans[24] == 0x12 && ans[27] == 5);
    const uint8_t *e = ans + 28;
    for (unsigned i = 0; i < 5; ++i) {
        const uint32_t len = e[0];
        CHECK(len == strlen(table[i].domain) && !memcmp(e + 1, table[i].domain, len) && rd32(e + 1 + len) == table[i].instance);
        e += 1 + len + 9;
    }
    CHECK(e == ans + n && rd16(ans + 25) == (uint32_t)(e - (ans + 27)));
    /* an offset skips the first entries but the total stays */
    n = qcom_servloc_answer(table, 5, req, request(req, 8, "tms/servreg", 1, 3), ans, sizeof ans);
    CHECK(n > 0 && rd16(ans + 17) == 5 && ans[27] == 2 && ans[28] == strlen("msm/adsp/sensor_pd"));
    /* a name counted with its terminating zero is the same name */
    uint32_t m = request(req, 9, "avs/audio", 0, 0);
    memmove(req + 7 + 3 + 10, req + 7 + 3 + 9, m - (7 + 3 + 9)); req[7 + 3 + 9] = 0; req[8] = 10; req[5]++;
    n = qcom_servloc_answer(table, 5, req, m + 1, ans, sizeof ans);
    CHECK(n == sizeof expect && rd16(ans + 1) == 9 && !memcmp(ans + 7, expect + 7, n - 7));
    /* an unknown service: total 0 and no list */
    n = qcom_servloc_answer(table, 5, req, request(req, 10, "no/such", 0, 0), ans, sizeof ans);
    CHECK(n == 7 + 7 + 5 + 5 && rd16(ans + 17) == 0);
    /* not a locator request, a short buffer */
    m = request(req, 11, "avs/audio", 0, 0);
    req[3] = 0x20;
    CHECK(qcom_servloc_answer(table, 5, req, m, ans, sizeof ans) == 0);
    req[3] = 0x21;
    CHECK(qcom_servloc_answer(table, 5, req, m, ans, 40) == 0);
    CHECK(qcom_servloc_answer(table, 5, req, m - 1, ans, sizeof ans) == 0);
}

int main(void)
{
    router();
    locator();
    printf("PASS qrtr and service locator: %u checks\n", checks);
    return 0;
}
