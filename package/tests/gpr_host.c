// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the GPR packet codec and the audio framework readiness query (package/ShengGpr.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ShengGpr.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}

/* What the DSP would send back to our query. */
static uint32_t reply(uint8_t *out, const uint8_t *query, uint32_t opcode, const uint32_t *words, unsigned n)
{
    GprPacket q, p;
    CHECK(gpr_decode(query, GPR_HEADER_BYTES, &q) == GprOk);
    memset(&p, 0, sizeof p);
    p.dst_domain = q.src_domain; p.src_domain = q.dst_domain; p.src_port = q.dst_port; p.dst_port = q.src_port; p.token = q.token;
    p.opcode = opcode;
    return gpr_encode(out, GPR_MAX_PACKET, &p, words, 4 * n);
}

int main(void)
{
    uint8_t buffer[GPR_MAX_PACKET], query[GPR_HEADER_BYTES], answer[GPR_MAX_PACKET];
    GprPacket p, d;

    /* Exact header bytes. */
    memset(&p, 0, sizeof p);
    p.dst_domain = GPR_DOMAIN_ADSP; p.src_domain = GPR_DOMAIN_APPS; p.src_port = 1; p.dst_port = 1; p.token = 0x1234; p.opcode = GPR_OP_APM_GET_SPF_STATE;
    const uint8_t expected[24] = {0x60, 0x18, 0, 0, 0x02, 0x03, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0x34, 0x12, 0, 0, 0x21, 0x10, 0x00, 0x01};
    CHECK(gpr_encode(buffer, sizeof buffer, &p, NULL, 0) == 24 && !memcmp(buffer, expected, 24));
    /* With a payload; round trip. */
    const uint32_t words[3] = {7, 8, 9};
    CHECK(gpr_encode(buffer, sizeof buffer, &p, words, 12) == 36 && le32(buffer) == (0x60u | 36u << 8));
    CHECK(gpr_decode(buffer, 36, &d) == GprOk && d.size == 36 && d.header_bytes == 24 && d.payload_bytes == 12 && le32(d.payload + 8) == 9);
    CHECK(d.dst_domain == 2 && d.src_domain == 3 && d.src_port == 1 && d.dst_port == 1 && d.token == 0x1234 && d.opcode == GPR_OP_APM_GET_SPF_STATE);
    /* A received buffer can be longer than the packet (intent size). */
    CHECK(gpr_decode(buffer, 512, &d) == GprOk && d.payload_bytes == 12);
    /* Limits and malformed packets. */
    CHECK(gpr_encode(buffer, 35, &p, words, 12) == 0 && gpr_encode(buffer, sizeof buffer, &p, NULL, 4) == 0);
    CHECK(gpr_encode(buffer, sizeof buffer, &p, buffer, GPR_MAX_PACKET - 23) == 0);
    gpr_encode(buffer, sizeof buffer, &p, words, 12);
    CHECK(gpr_decode(buffer, 23, &d) == GprShort && gpr_decode(buffer, 35, &d) == GprBadSize);
    buffer[0] = 0x61; CHECK(gpr_decode(buffer, 36, &d) == GprBadVersion);
    buffer[0] = 0x50; CHECK(gpr_decode(buffer, 36, &d) == GprBadHeader);
    buffer[0] = 0x70; CHECK(gpr_decode(buffer, 36, &d) == GprOk && d.header_bytes == 28 && d.payload_bytes == 8 && le32(d.payload) == 8);
    buffer[0] = 0x60; buffer[1] = 0x10; CHECK(gpr_decode(buffer, 36, &d) == GprBadSize);  /* size 16 < header */
    /* Basic result. */
    uint32_t op = 0, status = 0;
    const uint32_t basic[2] = {GPR_OP_APM_GET_SPF_STATE, 0x0b};
    memset(&p, 0, sizeof p); p.opcode = GPR_OP_BASIC_RESULT;
    gpr_encode(buffer, sizeof buffer, &p, basic, 8);
    CHECK(gpr_decode(buffer, 32, &d) == GprOk && gpr_basic_result(&d, &op, &status) == 0 && op == GPR_OP_APM_GET_SPF_STATE && status == 0x0b);
    gpr_encode(buffer, sizeof buffer, &p, basic, 4);
    CHECK(gpr_decode(buffer, 28, &d) == GprOk && gpr_basic_result(&d, &op, &status) == -1);

    /* The readiness query: not ready twice, then ready. */
    GprSpfProbe s;
    gpr_spf_init(&s, GPR_PORT_APM, 5);
    CHECK(gpr_spf_next(&s, query) == 24 && le32(query + 20) == GPR_OP_APM_GET_SPF_STATE && le32(query + 16) == 1 && s.state == SpfWaiting);
    CHECK(gpr_spf_next(&s, query) == 0);   /* one in flight */
    uint32_t zero = 0, one = 1;
    uint32_t n = reply(answer, query, GPR_OP_APM_SPF_STATE, &zero, 1);
    CHECK(gpr_spf_receive(&s, answer, n) == SpfNotReady && s.state == SpfIdle && s.last_status == 0);
    CHECK(gpr_spf_receive(&s, answer, n) == SpfIgnored);   /* repeated answer: nothing in flight */
    CHECK(gpr_spf_next(&s, query) == 24 && le32(query + 16) == 2);
    /* A late answer to the first query (old token), another port's packet and a broken one are ignored. */
    uint8_t stale[GPR_MAX_PACKET];
    memcpy(stale, answer, n);
    CHECK(gpr_spf_receive(&s, stale, n) == SpfIgnored);
    n = reply(answer, query, GPR_OP_APM_SPF_STATE, &one, 1);
    answer[12] = 9;
    CHECK(gpr_spf_receive(&s, answer, n) == SpfIgnored);
    answer[12] = 1;
    CHECK(gpr_spf_receive(&s, answer, 10) == SpfIgnored && s.ignored == 4);
    /* A timeout lets the owner ask again. */
    CHECK(gpr_spf_timeout(&s) == SpfIgnored && s.timeouts == 1 && s.state == SpfIdle);
    CHECK(gpr_spf_next(&s, query) == 24 && le32(query + 16) == 3);
    /* The APM refuses the query: an error, and asking again is allowed. */
    n = reply(answer, query, GPR_OP_BASIC_RESULT, basic, 2);
    CHECK(gpr_spf_receive(&s, answer, n) == SpfError && s.errors == 1 && s.last_status == 0x0b && s.state == SpfIdle);
    CHECK(gpr_spf_next(&s, query) == 24);
    n = reply(answer, query, GPR_OP_APM_SPF_STATE, &one, 1);
    CHECK(gpr_spf_receive(&s, answer, n) == SpfIsReady && s.state == SpfReady && s.answers == 2);
    CHECK(gpr_spf_next(&s, query) == 0 && gpr_spf_timeout(&s) == SpfIgnored);

    /* Out of tries: the last "not ready" or timeout gives up. */
    gpr_spf_init(&s, 2, 2);
    CHECK(gpr_spf_next(&s, query) == 24 && le32(query + 8) == 2);
    CHECK(gpr_spf_timeout(&s) == SpfIgnored && gpr_spf_next(&s, query) == 24);
    CHECK(gpr_spf_timeout(&s) == SpfDone && s.state == SpfGaveUp && gpr_spf_next(&s, query) == 0);
    gpr_spf_init(&s, 1, 1);
    gpr_spf_next(&s, query);
    n = reply(answer, query, GPR_OP_APM_SPF_STATE, &zero, 1);
    CHECK(gpr_spf_receive(&s, answer, n) == SpfDone && s.state == SpfGaveUp);
    printf("PASS GPR: %u checks\n", checks);
    return 0;
}
