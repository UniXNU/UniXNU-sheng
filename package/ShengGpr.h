/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The audio DSP's packet router (GPR) on the GLINK channel "adsp_apps": every packet starts with a 24-byte header (version and header
 * size in words, total size, destination and source domain, source and destination port, token, opcode) followed by the payload. This
 * is the codec and the first exchange an audio client makes: asking the DSP's audio framework manager (APM) whether the framework is
 * ready. No I/O and no waits: the owner sends the bytes, feeds back what arrives and runs the retry and answer timers. */
#ifndef SHENG_GPR_H
#define SHENG_GPR_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define GPR_HEADER_BYTES 24u
#define GPR_VERSION 0u
#define GPR_DOMAIN_MODEM 1u
#define GPR_DOMAIN_ADSP 2u
#define GPR_DOMAIN_APPS 3u
#define GPR_PORT_APM 1u                       /* the APM module instance, and the port its client uses */
#define GPR_OP_BASIC_RESULT 0x02001005u      /* payload {opcode, status} */
#define GPR_OP_APM_GET_SPF_STATE 0x01001021u
#define GPR_OP_APM_SPF_STATE 0x02001007u     /* payload {status}: 1 ready */
#define GPR_MAX_PACKET 512u

typedef struct {
    uint32_t version, header_bytes, size;
    uint32_t dst_domain, src_domain, src_port, dst_port, token, opcode;
    const uint8_t *payload;
    uint32_t payload_bytes;
} GprPacket;

enum {GprOk = 0, GprShort = -1, GprBadVersion = -2, GprBadHeader = -3, GprBadSize = -4};

/* Writes header and payload into out; returns the packet size, or 0 when it does not fit. */
uint32_t gpr_encode(uint8_t *out, uint32_t capacity, const GprPacket *p, const void *payload, uint32_t payload_bytes);
/* Parses a received packet (length: the bytes received; the packet may be shorter). Returns GprOk or an error. */
int gpr_decode(const uint8_t *in, uint32_t length, GprPacket *p);
/* A basic result's {opcode, status}; returns 0 when the packet is one. */
int gpr_basic_result(const GprPacket *p, uint32_t *opcode, uint32_t *status);

/* Asking the APM whether the framework is ready: one query in flight; a "not ready" answer or a timeout lets the owner ask again after
 * its retry pause, up to max_queries. */
enum {SpfIdle, SpfWaiting, SpfReady, SpfGaveUp};
enum {SpfIgnored = 0, SpfNotReady = 1, SpfIsReady = 2, SpfError = 3, SpfDone = 4};
typedef struct {
    uint32_t port, token, state, queries, max_queries;
    uint32_t answers, errors, timeouts, ignored, last_status, last_opcode;
} GprSpfProbe;

void gpr_spf_init(GprSpfProbe *s, uint32_t port, uint32_t max_queries);
/* The query to send now, or 0 (one in flight, ready, or out of tries). */
uint32_t gpr_spf_next(GprSpfProbe *s, uint8_t out[GPR_HEADER_BYTES]);
/* A packet from the channel; returns one of the Spf* results. SpfError: the APM refused the query (status in last_status). */
int gpr_spf_receive(GprSpfProbe *s, const uint8_t *data, uint32_t length);
/* The owner's answer timer ran out. Returns SpfDone when out of tries, else SpfIgnored. */
int gpr_spf_timeout(GprSpfProbe *s);

#ifdef __cplusplus
}
#endif
#endif
