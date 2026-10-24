/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "ShengGpr.h"

static void put32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}

uint32_t gpr_encode(uint8_t *out, uint32_t capacity, const GprPacket *p, const void *payload, uint32_t payload_bytes)
{
    if (payload_bytes > GPR_MAX_PACKET - GPR_HEADER_BYTES || (payload_bytes && !payload)) return 0;
    const uint32_t size = GPR_HEADER_BYTES + payload_bytes;
    if (size > capacity) return 0;
    /* word 0: version 3:0, header size in words 7:4, packet size in bytes 31:8; word 1: destination domain 7:0, source domain 15:8 */
    put32(out, (GPR_VERSION & 0xfu) | (GPR_HEADER_BYTES / 4) << 4 | size << 8);
    put32(out + 4, (p->dst_domain & 0xffu) | (p->src_domain & 0xffu) << 8);
    put32(out + 8, p->src_port);
    put32(out + 12, p->dst_port);
    put32(out + 16, p->token);
    put32(out + 20, p->opcode);
    for (uint32_t i = 0; i < payload_bytes; ++i) out[GPR_HEADER_BYTES + i] = ((const uint8_t *)payload)[i];
    return size;
}

int gpr_decode(const uint8_t *in, uint32_t length, GprPacket *p)
{
    if (length < GPR_HEADER_BYTES) return GprShort;
    const uint32_t w0 = le32(in), w1 = le32(in + 4);
    p->version = w0 & 0xfu;
    p->header_bytes = ((w0 >> 4) & 0xfu) * 4;
    p->size = w0 >> 8;
    if (p->version != GPR_VERSION) return GprBadVersion;
    if (p->header_bytes < GPR_HEADER_BYTES) return GprBadHeader;
    if (p->size < p->header_bytes || p->size > length) return GprBadSize;
    p->dst_domain = w1 & 0xffu;
    p->src_domain = (w1 >> 8) & 0xffu;
    p->src_port = le32(in + 8);
    p->dst_port = le32(in + 12);
    p->token = le32(in + 16);
    p->opcode = le32(in + 20);
    p->payload = in + p->header_bytes;
    p->payload_bytes = p->size - p->header_bytes;
    return GprOk;
}

int gpr_basic_result(const GprPacket *p, uint32_t *opcode, uint32_t *status)
{
    if (p->opcode != GPR_OP_BASIC_RESULT || p->payload_bytes < 8) return -1;
    *opcode = le32(p->payload);
    *status = le32(p->payload + 4);
    return 0;
}

void gpr_spf_init(GprSpfProbe *s, uint32_t port, uint32_t max_queries)
{
    for (unsigned i = 0; i < sizeof *s; ++i) ((uint8_t *)s)[i] = 0;
    s->port = port;
    s->max_queries = max_queries;
    s->state = SpfIdle;
}

uint32_t gpr_spf_next(GprSpfProbe *s, uint8_t out[GPR_HEADER_BYTES])
{
    if (s->state != SpfIdle || s->queries >= s->max_queries) return 0;
    const GprPacket p = {.dst_domain = GPR_DOMAIN_ADSP, .src_domain = GPR_DOMAIN_APPS, .src_port = s->port, .dst_port = GPR_PORT_APM,
                         .token = ++s->token, .opcode = GPR_OP_APM_GET_SPF_STATE};
    s->queries++;
    s->state = SpfWaiting;
    return gpr_encode(out, GPR_HEADER_BYTES, &p, 0, 0);
}

static int retry_or_stop(GprSpfProbe *s)
{
    if (s->queries >= s->max_queries) {s->state = SpfGaveUp; return SpfDone;}
    s->state = SpfIdle;
    return SpfNotReady;
}

int gpr_spf_receive(GprSpfProbe *s, const uint8_t *data, uint32_t length)
{
    GprPacket p;
    if (gpr_decode(data, length, &p) != GprOk || p.dst_port != s->port || p.src_domain != GPR_DOMAIN_ADSP) {s->ignored++; return SpfIgnored;}
    s->last_opcode = p.opcode;
    if (s->state != SpfWaiting || p.token != s->token) {s->ignored++; return SpfIgnored;}
    uint32_t opcode = 0, status = 0;
    if (p.opcode == GPR_OP_APM_SPF_STATE && p.payload_bytes >= 4) {
        s->answers++;
        s->last_status = le32(p.payload);
        if (s->last_status == 1) {s->state = SpfReady; return SpfIsReady;}
        const int r = retry_or_stop(s);
        return r;
    }
    if (!gpr_basic_result(&p, &opcode, &status) && opcode == GPR_OP_APM_GET_SPF_STATE) {
        s->errors++;
        s->last_status = status;
        retry_or_stop(s);
        return SpfError;
    }
    s->ignored++;
    return SpfIgnored;
}

int gpr_spf_timeout(GprSpfProbe *s)
{
    if (s->state != SpfWaiting) return SpfIgnored;
    s->timeouts++;
    return retry_or_stop(s) == SpfDone ? SpfDone : SpfIgnored;
}
