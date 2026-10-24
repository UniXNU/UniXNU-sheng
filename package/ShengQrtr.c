/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The IPC router link to the audio DSP (GLINK channel "IPCRTR", which the DSP opens) and the service locator on it. The DSP's protection
 * domains (audio, charger, sensors) ask the application processor's locator which domains provide a service before they start; the
 * battery manager runs in the charger domain. Started with sheng-adsp=2. Everything runs in the DSP thread's callbacks: each packet is
 * handled at once and any answer is queued with sheng_adsp_send, so nothing here waits. */
#include <stddef.h>
#include <stdint.h>
#include "QcomQrtr.h"
#include "ShengAdsp.h"

/* [0] stage (1 channel registered, 2 link up, 3 the DSP's hello, negative: registration failed), [1] packets in, [2] data packets in,
 * [3] bad packets, [4] packets out, [5] sends refused, [6] locator requests, [7] locator answers, [8] the DSP's node, [9] services the
 * DSP announced, [10] link ups, [11] link downs, [12] data for ports we do not have, [13] the last locator request's service name
 * length | its first byte << 8 */
volatile int32_t native_qrtr_stats[16];
/* the services the DSP announced, as service:instance in hex */
char native_qrtr_text[96];

enum {kLocalNode = 1, kLocatorPort = 1};

static const char *const kAudio[] = {"avs/audio", NULL};
static const char *const kGps[] = {"gps/gps_service", NULL};
static const QcomPdDomain kDomains[] = {
    {"msm/adsp/audio_pd", 74, kAudio},   {"msm/adsp/root_pd", 74, NULL}, {"msm/adsp/charger_pd", 74, NULL},
    {"msm/adsp/sensor_pd", 74, NULL},    {"msm/cdsp/root_pd", 76, NULL}, {"msm/modem/root_pd", 180, kGps},
};

static QcomQrtr gQrtr;
static ShengAdspChannel *gChannel;

static int send(void *context, const uint8_t *packet, uint32_t length)
{
    (void)context;
    return sheng_adsp_send(gChannel, packet, length) ? -1 : 0;
}

static void hex(char *out, unsigned *at, unsigned cap, uint32_t value)
{
    char digits[8];
    unsigned n = 0;
    do {digits[n++] = "0123456789abcdef"[value & 15]; value >>= 4;} while (value && n < 8);
    while (n && *at + 1 < cap) out[(*at)++] = digits[--n];
}

static void counters(void)
{
    native_qrtr_stats[1] = (int32_t)gQrtr.rx_packets;
    native_qrtr_stats[2] = (int32_t)gQrtr.rx_data;
    native_qrtr_stats[3] = (int32_t)gQrtr.rx_bad;
    native_qrtr_stats[4] = (int32_t)gQrtr.tx_packets;
    native_qrtr_stats[5] = (int32_t)gQrtr.tx_refused;
    native_qrtr_stats[8] = gQrtr.hello_received ? (int32_t)gQrtr.remote_node : -1;
    native_qrtr_stats[9] = (int32_t)gQrtr.remote_count;
    native_qrtr_stats[12] = (int32_t)gQrtr.rx_unrouted;
    if (gQrtr.hello_received && native_qrtr_stats[0] == 2) native_qrtr_stats[0] = 3;
    unsigned at = 0;
    const unsigned cap = sizeof native_qrtr_text;
    for (uint32_t i = 0; i < gQrtr.remote_count && at + 4 < cap; ++i) {
        if (at) native_qrtr_text[at++] = ' ';
        hex(native_qrtr_text, &at, cap, gQrtr.remote[i].service);
        if (at + 1 < cap) native_qrtr_text[at++] = ':';
        hex(native_qrtr_text, &at, cap, gQrtr.remote[i].instance);
    }
    native_qrtr_text[at] = 0;
}

static void locator(void *context, QcomQrtr *q, uint32_t port, uint32_t node, uint32_t from, const uint8_t *data, uint32_t length)
{
    (void)context;
    uint8_t answer[480];
    QcomQmiMessage m;
    uint32_t size = 0;
    native_qrtr_stats[6]++;
    if (!qcom_qmi_parse(data, length, &m)) {
        const uint8_t *name = qcom_qmi_tlv(&m, 0x01, &size);
        if (name) native_qrtr_stats[13] = (int32_t)(size | (size ? (uint32_t)name[0] << 8 : 0));
    }
    const uint32_t n = qcom_servloc_answer(kDomains, sizeof kDomains / sizeof kDomains[0], data, length, answer, sizeof answer);
    if (n && !qcom_qrtr_send_data(q, port, node, from, answer, n)) native_qrtr_stats[7]++;
}

static void linkState(void *context, int up)
{
    (void)context;
    if (up) {
        native_qrtr_stats[10]++;
        /* a fresh link: forget the old one's hello and services, keep ours */
        gQrtr.hello_sent = gQrtr.hello_received = 0;
        gQrtr.remote_count = 0;
        gQrtr.remote_node = QCOM_QRTR_NODE_BROADCAST;
        native_qrtr_stats[0] = 2;
        qcom_qrtr_start(&gQrtr);
    } else {
        native_qrtr_stats[11]++;
        native_qrtr_stats[0] = 1;
    }
    counters();
}

static void linkReceive(void *context, const uint8_t *data, uint32_t length)
{
    (void)context;
    qcom_qrtr_receive(&gQrtr, data, length);
    counters();
}

void sheng_qrtr_start(void)
{
    static const ShengAdspClient client = {linkState, linkReceive};
    qcom_qrtr_init(&gQrtr, kLocalNode, send, NULL);
    qcom_qrtr_add_service(&gQrtr, QCOM_SERVLOC_SERVICE, QCOM_SERVLOC_INSTANCE, kLocatorPort, locator, NULL);
    gChannel = sheng_adsp_channel("IPCRTR", 1024, 8, 0, &client, NULL);
    native_qrtr_stats[0] = gChannel ? 1 : -1;
}
