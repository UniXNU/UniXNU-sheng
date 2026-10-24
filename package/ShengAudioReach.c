/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "ShengAudioReach.h"
#include "ShengGpr.h"

#define PARAM_HEADER 16u
#define CMD_HEADER 16u
#define PARAM_SG_CONFIG 0x08001001u
#define PARAM_CONTAINER_CONFIG 0x08001000u
#define PARAM_MODULE_LIST 0x08001002u
#define PARAM_MODULE_PROP 0x08001003u
#define PARAM_MODULE_CONN 0x08001004u
#define PARAM_SG_LIST 0x08001005u
#define PROP_SG_PERF 0x0800100Eu
#define PROP_SG_DIRECTION 0x0800100Fu
#define PROP_SG_SCENARIO 0x08001010u
#define PROP_CONT_CAPS 0x08001011u
#define PROP_CONT_POS 0x08001012u
#define PROP_CONT_STACK 0x08001013u
#define PROP_CONT_DOMAIN 0x08001014u
#define PROP_PORT_INFO 0x08001015u
#define PARAM_I2S_INTF 0x08001019u
#define PARAM_HW_EP_MF 0x08001017u
#define PARAM_FRAME_SIZE_FACTOR 0x08001018u
#define PARAM_PCM_OUTPUT 0x08001008u
#define PARAM_MEDIA_FORMAT 0x0800100Cu
#define PARAM_CODEC_DMA_INTF 0x08001063u
#define PARAM_HW_EP_POWER_MODE 0x08001176u
#define CONTAINER_GC 0x0B001001u
#define MEDIA_PCM 0x09001000u
#define PERF_LOW_LATENCY 2u
#define DIR_TX 1u
#define DIR_RX 2u
#define SCENARIO_PLAYBACK 1u
#define SCENARIO_RECORD 2u
#define POS_STREAM 1u
#define POS_GLOBAL_DEV 4u
#define DOMAIN_ADSP 2u
#define STACK 8192u
#define OUT_PORT 1u
#define IN_PORT 2u
#define LPAIF 0u
#define LPAIF_RXTX 1u
#define I2S_SECONDARY 1u
#define I2S_SD1 2u
#define WS_INTERNAL 1u
#define CODEC_TX3 4u
#define INTERLEAVED 1u
#define DEINTERLEAVED_UNPACKED 3u

typedef struct {uint8_t *p; uint32_t n, cap; int bad;} W;

static void put8(W *w, uint32_t v) {if (w->n + 1 > w->cap) {w->bad = 1; return;} w->p[w->n++] = (uint8_t)v;}
static void put16(W *w, uint32_t v) {put8(w, v); put8(w, v >> 8);}
static void put32(W *w, uint32_t v) {put16(w, v); put16(w, v >> 16);}
static void pad_to(W *w, uint32_t end) {while (!w->bad && w->n < end) put8(w, 0);}
static uint32_t align8(uint32_t v) {return (v + 7u) & ~7u;}
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static void set32(W *w, uint32_t at, uint32_t v) {if (at + 4 <= w->n) {w->p[at] = (uint8_t)v; w->p[at + 1] = (uint8_t)(v >> 8); w->p[at + 2] = (uint8_t)(v >> 16); w->p[at + 3] = (uint8_t)(v >> 24);}}

/* GPR header with the packet size patched in at the end (finish). */
static void begin(W *w, uint8_t *out, uint32_t capacity, uint32_t port, uint32_t dst, uint32_t token, uint32_t opcode)
{
    w->p = out; w->n = 0; w->cap = capacity < GPR_MAX_PACKET ? capacity : GPR_MAX_PACKET; w->bad = 0;
    put32(w, 0); put32(w, GPR_DOMAIN_ADSP | GPR_DOMAIN_APPS << 8); put32(w, port); put32(w, dst); put32(w, token); put32(w, opcode);
}
static uint32_t finish(W *w)
{
    if (w->bad) return 0;
    set32(w, 0, (GPR_VERSION & 0xfu) | (GPR_HEADER_BYTES / 4) << 4 | w->n << 8);
    return w->n;
}
/* The APM command header: in-band payload of the given size follows. */
static void cmd_header(W *w, uint32_t payload) {put32(w, 0); put32(w, 0); put32(w, 0); put32(w, payload);}
static uint32_t cmd_payload_start(W *w) {uint32_t at = w->n; cmd_header(w, 0); return at;}
static void cmd_payload_end(W *w, uint32_t at) {set32(w, at + 12, w->n - at - CMD_HEADER);}
/* One parameter block: header, body written by the caller, then padding to 8 bytes; param_size counts body and padding. */
static uint32_t param_begin(W *w, uint32_t iid, uint32_t id) {uint32_t at = w->n; put32(w, iid); put32(w, id); put32(w, 0); put32(w, 0); return at;}
static void param_end(W *w, uint32_t at) {pad_to(w, at + align8(w->n - at)); set32(w, at + 8, w->n - at - PARAM_HEADER);}

typedef struct {uint32_t id, perf, direction, scenario;} Sg;
typedef struct {uint32_t id, position;} Container;
typedef struct {uint32_t module, iid, inputs, outputs;} Module;
typedef struct {uint32_t from, to;} Link;

/* One subgraph with one container (all the board topology uses). */
static uint32_t graph_open(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, const Sg *sg, const Container *c,
                           const Module *m, unsigned modules, const Link *l, unsigned links)
{
    W w;
    begin(&w, out, capacity, port, AR_APM_IID, token, AR_OP_GRAPH_OPEN);
    const uint32_t cmd = cmd_payload_start(&w);
    uint32_t at = param_begin(&w, AR_APM_IID, PARAM_SG_CONFIG);
    put32(&w, 1);
    put32(&w, sg->id); put32(&w, 3);
    put32(&w, PROP_SG_PERF); put32(&w, 4); put32(&w, sg->perf);
    put32(&w, PROP_SG_DIRECTION); put32(&w, 4); put32(&w, sg->direction);
    put32(&w, PROP_SG_SCENARIO); put32(&w, 4); put32(&w, sg->scenario);
    param_end(&w, at);
    at = param_begin(&w, AR_APM_IID, PARAM_CONTAINER_CONFIG);
    put32(&w, 1);
    put32(&w, c->id); put32(&w, 4);
    put32(&w, PROP_CONT_CAPS); put32(&w, 8); put32(&w, 1); put32(&w, CONTAINER_GC);
    put32(&w, PROP_CONT_POS); put32(&w, 4); put32(&w, c->position);
    put32(&w, PROP_CONT_STACK); put32(&w, 4); put32(&w, STACK);
    put32(&w, PROP_CONT_DOMAIN); put32(&w, 4); put32(&w, DOMAIN_ADSP);
    param_end(&w, at);
    at = param_begin(&w, AR_APM_IID, PARAM_MODULE_LIST);
    put32(&w, 1);
    put32(&w, sg->id); put32(&w, c->id); put32(&w, modules);
    for (unsigned i = 0; i < modules; ++i) {put32(&w, m[i].module); put32(&w, m[i].iid);}
    param_end(&w, at);
    at = param_begin(&w, AR_APM_IID, PARAM_MODULE_PROP);
    put32(&w, modules);
    for (unsigned i = 0; i < modules; ++i) {
        put32(&w, m[i].iid); put32(&w, 1); put32(&w, PROP_PORT_INFO); put32(&w, 8); put32(&w, m[i].inputs); put32(&w, m[i].outputs);
    }
    param_end(&w, at);
    at = param_begin(&w, AR_APM_IID, PARAM_MODULE_CONN);
    put32(&w, links);
    for (unsigned i = 0; i < links; ++i) {put32(&w, l[i].from); put32(&w, OUT_PORT); put32(&w, l[i].to); put32(&w, IN_PORT);}
    param_end(&w, at);
    cmd_payload_end(&w, cmd);
    return finish(&w);
}

uint32_t ar_open_speaker_device(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token)
{
    static const Sg sg = {AR_SG_SPEAKER, PERF_LOW_LATENCY, DIR_TX, SCENARIO_PLAYBACK};
    static const Container c = {AR_SG_SPEAKER, POS_STREAM};
    static const Module m[] = {{AR_MOD_I2S_SINK, AR_IID_SPEAKER_I2S, 1, 0}};
    return graph_open(out, capacity, port, token, &sg, &c, m, 1, 0, 0);
}

uint32_t ar_open_playback_stream(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token)
{
    static const Sg sg = {AR_SG_PLAY, PERF_LOW_LATENCY, DIR_RX, SCENARIO_PLAYBACK};
    static const Container c = {AR_SG_PLAY, POS_STREAM};
    static const Module m[] = {{AR_MOD_WR_SHMEM, AR_IID_PLAY_SHMEM, 0, 1}, {AR_MOD_PCM_DEC, AR_IID_PLAY_DEC, 1, 1},
                               {AR_MOD_PCM_CNV, AR_IID_PLAY_CNV, 1, 1}};
    static const Link l[] = {{AR_IID_PLAY_SHMEM, AR_IID_PLAY_DEC}, {AR_IID_PLAY_DEC, AR_IID_PLAY_CNV}, {AR_IID_PLAY_CNV, AR_IID_SPEAKER_I2S}};
    return graph_open(out, capacity, port, token, &sg, &c, m, 3, l, 3);
}

uint32_t ar_open_mic_device(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token)
{
    static const Sg sg = {AR_SG_MIC, PERF_LOW_LATENCY, DIR_TX, SCENARIO_RECORD};
    static const Container c = {AR_SG_MIC, POS_GLOBAL_DEV};
    static const Module m[] = {{AR_MOD_CODEC_DMA_SOURCE, AR_IID_MIC_DMA, 0, 1}};
    return graph_open(out, capacity, port, token, &sg, &c, m, 1, 0, 0);
}

uint32_t ar_open_capture_stream(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token)
{
    static const Sg sg = {AR_SG_CAPTURE, PERF_LOW_LATENCY, DIR_TX, SCENARIO_RECORD};
    static const Container c = {AR_SG_CAPTURE, POS_GLOBAL_DEV};
    static const Module m[] = {{AR_MOD_PCM_CNV, AR_IID_CAP_CNV, 1, 1}, {AR_MOD_PCM_ENC, AR_IID_CAP_ENC, 1, 1},
                               {AR_MOD_RD_SHMEM, AR_IID_CAP_SHMEM, 1, 0}};
    static const Link l[] = {{AR_IID_MIC_DMA, AR_IID_CAP_CNV}, {AR_IID_CAP_CNV, AR_IID_CAP_ENC}, {AR_IID_CAP_ENC, AR_IID_CAP_SHMEM}};
    return graph_open(out, capacity, port, token, &sg, &c, m, 3, l, 3);
}

static void hw_ep_mf(W *w, uint32_t iid, const ArFormat *f)
{
    const uint32_t at = param_begin(w, iid, PARAM_HW_EP_MF);
    put32(w, f->rate); put16(w, f->bits); put16(w, f->channels); put32(w, 1);
    param_end(w, at);
}
static void frame_size_factor(W *w, uint32_t iid)
{
    const uint32_t at = param_begin(w, iid, PARAM_FRAME_SIZE_FACTOR);
    put32(w, 1);
    param_end(w, at);
}
static void channel_map(W *w, unsigned channels) {for (unsigned i = 0; i < channels; ++i) put8(w, i + 1);}   /* FL = 1, FR = 2 */

uint32_t ar_config_speaker_device(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, const ArFormat *f)
{
    W w;
    begin(&w, out, capacity, port, AR_APM_IID, token, AR_OP_SET_CFG);
    const uint32_t cmd = cmd_payload_start(&w);
    const uint32_t at = param_begin(&w, AR_IID_SPEAKER_I2S, PARAM_I2S_INTF);
    put32(&w, LPAIF); put32(&w, I2S_SECONDARY); put16(&w, I2S_SD1); put16(&w, WS_INTERNAL);
    param_end(&w, at);
    hw_ep_mf(&w, AR_IID_SPEAKER_I2S, f);
    frame_size_factor(&w, AR_IID_SPEAKER_I2S);
    cmd_payload_end(&w, cmd);
    return finish(&w);
}

uint32_t ar_config_mic_device(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, const ArFormat *f, uint32_t channel_mask)
{
    W w;
    begin(&w, out, capacity, port, AR_APM_IID, token, AR_OP_SET_CFG);
    const uint32_t cmd = cmd_payload_start(&w);
    hw_ep_mf(&w, AR_IID_MIC_DMA, f);
    frame_size_factor(&w, AR_IID_MIC_DMA);
    uint32_t at = param_begin(&w, AR_IID_MIC_DMA, PARAM_CODEC_DMA_INTF);
    put32(&w, LPAIF_RXTX); put32(&w, CODEC_TX3); put32(&w, channel_mask);
    param_end(&w, at);
    at = param_begin(&w, AR_IID_MIC_DMA, PARAM_HW_EP_POWER_MODE);
    put32(&w, 0);
    param_end(&w, at);
    cmd_payload_end(&w, cmd);
    return finish(&w);
}

/* PCM output format of a decoder, converter or encoder. */
static void pcm_output(W *w, uint32_t iid, const ArFormat *f, uint32_t interleaving)
{
    const uint32_t at = param_begin(w, iid, PARAM_PCM_OUTPUT);
    put32(w, 1); put32(w, MEDIA_PCM); put32(w, (16u + f->channels + 3u) & ~3u);
    put16(w, f->bits); put16(w, 1); put16(w, f->bits); put16(w, f->bits - 1u); put16(w, 1); put16(w, interleaving); put16(w, 0);
    put16(w, f->channels);
    channel_map(w, f->channels);
    param_end(w, at);
}

uint32_t ar_config_playback_stream(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, const ArFormat *f)
{
    W w;
    begin(&w, out, capacity, port, AR_APM_IID, token, AR_OP_SET_CFG);
    const uint32_t cmd = cmd_payload_start(&w);
    pcm_output(&w, AR_IID_PLAY_DEC, f, INTERLEAVED);
    pcm_output(&w, AR_IID_PLAY_CNV, f, DEINTERLEAVED_UNPACKED);
    cmd_payload_end(&w, cmd);
    return finish(&w);
}

uint32_t ar_config_capture_stream(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, const ArFormat *f)
{
    W w;
    begin(&w, out, capacity, port, AR_APM_IID, token, AR_OP_SET_CFG);
    const uint32_t cmd = cmd_payload_start(&w);
    pcm_output(&w, AR_IID_CAP_CNV, f, INTERLEAVED);
    pcm_output(&w, AR_IID_CAP_ENC, f, INTERLEAVED);
    cmd_payload_end(&w, cmd);
    return finish(&w);
}

/* The input media format of the playback shared memory endpoint, sent to the endpoint itself. */
uint32_t ar_config_playback_shmem(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, const ArFormat *f)
{
    W w;
    begin(&w, out, capacity, port, AR_IID_PLAY_SHMEM, token, AR_OP_SET_CFG);
    const uint32_t cmd = cmd_payload_start(&w);
    const uint32_t at = param_begin(&w, AR_IID_PLAY_SHMEM, PARAM_MEDIA_FORMAT);
    const uint32_t body = align8(12u + 16u + f->channels);
    put32(&w, 1); put32(&w, MEDIA_PCM); put32(&w, body + PARAM_HEADER - 12u);
    put32(&w, f->rate); put16(&w, f->bits); put16(&w, 1); put16(&w, f->bits); put16(&w, f->bits - 1u); put16(&w, 1);
    put16(&w, f->channels);
    channel_map(&w, f->channels);
    param_end(&w, at);
    cmd_payload_end(&w, cmd);
    return finish(&w);
}

uint32_t ar_graph_command(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t opcode, uint32_t sg)
{
    W w;
    begin(&w, out, capacity, port, AR_APM_IID, token, opcode);
    const uint32_t cmd = cmd_payload_start(&w);
    const uint32_t at = param_begin(&w, AR_APM_IID, PARAM_SG_LIST);
    put32(&w, 1); put32(&w, sg);
    param_end(&w, at);
    cmd_payload_end(&w, cmd);
    return finish(&w);
}

/* As the DSP's own client builds it: the region follows the GPR header directly; the command header space trails the packet. */
uint32_t ar_map(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint64_t address, uint32_t bytes)
{
    W w;
    begin(&w, out, capacity, port, AR_APM_IID, token, AR_OP_MEM_MAP);
    put16(&w, 3); put16(&w, 1); put32(&w, 0);                      /* shared 8-byte-aligned 4 KiB pool, one region, no flags */
    put32(&w, (uint32_t)address); put32(&w, (uint32_t)(address >> 32)); put32(&w, (bytes + 4095u) & ~4095u);
    pad_to(&w, w.n + CMD_HEADER);
    return finish(&w);
}

uint32_t ar_unmap(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t handle)
{
    W w;
    begin(&w, out, capacity, port, AR_APM_IID, token, AR_OP_MEM_UNMAP);
    put32(&w, handle);
    pad_to(&w, w.n + CMD_HEADER);
    return finish(&w);
}

uint32_t ar_write(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint64_t address, uint32_t handle, uint32_t bytes)
{
    W w;
    begin(&w, out, capacity, port, AR_IID_PLAY_SHMEM, token, AR_OP_WRITE);
    put32(&w, (uint32_t)address); put32(&w, (uint32_t)(address >> 32)); put32(&w, handle); put32(&w, bytes);
    put32(&w, 0); put32(&w, 0); put32(&w, 0);                      /* no time stamp, no flags */
    put32(&w, 0); put32(&w, 0); put32(&w, 0); put32(&w, 0);         /* no metadata */
    return finish(&w);
}

uint32_t ar_read(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint64_t address, uint32_t handle, uint32_t bytes)
{
    W w;
    begin(&w, out, capacity, port, AR_IID_CAP_SHMEM, token, AR_OP_READ);
    put32(&w, (uint32_t)address); put32(&w, (uint32_t)(address >> 32)); put32(&w, handle); put32(&w, bytes);
    put32(&w, 0); put32(&w, 0); put32(&w, 0); put32(&w, 0);
    return finish(&w);
}

uint32_t ar_prm_clock(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t clock_id, uint32_t hz)
{
    W w;
    begin(&w, out, capacity, port, AR_PRM_IID, token, hz ? AR_OP_PRM_REQUEST : AR_OP_PRM_RELEASE);
    const uint32_t cmd = cmd_payload_start(&w);
    const uint32_t at = param_begin(&w, AR_PRM_IID, 0x0800102Cu);    /* audio hardware clock */
    put32(&w, 1); put32(&w, clock_id);
    if (hz) {put32(&w, hz); put32(&w, 1); put32(&w, 0);}            /* frequency, attribute "couple no", default root */
    set32(&w, at + 8, w.n - at - PARAM_HEADER);                      /* exact size: the reference client does not pad these */
    cmd_payload_end(&w, cmd);
    return finish(&w);
}

uint32_t ar_prm_vote(uint8_t *out, uint32_t capacity, uint32_t port, uint32_t token, uint32_t block, int enable)
{
    W w;
    begin(&w, out, capacity, port, AR_PRM_IID, token, enable ? AR_OP_PRM_REQUEST : AR_OP_PRM_RELEASE);
    const uint32_t cmd = cmd_payload_start(&w);
    const uint32_t at = param_begin(&w, AR_PRM_IID, 0x08001032u);    /* hardware core */
    put32(&w, block);
    set32(&w, at + 8, w.n - at - PARAM_HEADER);
    cmd_payload_end(&w, cmd);
    return finish(&w);
}

int ar_parse(const uint8_t *in, uint32_t length, ArAnswer *a)
{
    GprPacket p;
    for (unsigned i = 0; i < sizeof *a; ++i) ((uint8_t *)a)[i] = 0;
    if (gpr_decode(in, length, &p) != GprOk) return ArBad;
    a->opcode = p.opcode; a->token = p.token; a->src_port = p.src_port;
    const uint8_t *d = p.payload;
    switch (p.opcode) {
    case GPR_OP_BASIC_RESULT:
        if (p.payload_bytes < 8) return ArBad;
        a->result_opcode = le32(d); a->status = le32(d + 4);
        return ArBasicResult;
    case AR_OP_PRM_REQUEST_DONE:
    case AR_OP_PRM_RELEASE_DONE:
        if (p.payload_bytes < 4) return ArBad;
        a->status = le32(d + (p.payload_bytes >= 8 ? 4 : 0));
        return ArPrmDone;
    case GPR_OP_APM_SPF_STATE:
        if (p.payload_bytes < 4) return ArBad;
        a->status = le32(d);
        return ArFrameworkState;
    case AR_OP_MEM_MAP_DONE:
        if (p.payload_bytes < 4) return ArBad;
        a->handle = le32(d);
        return ArMapped;
    case AR_OP_WRITE_DONE:
        if (p.payload_bytes < 16) return ArBad;
        a->address = le32(d) | (uint64_t)le32(d + 4) << 32; a->handle = le32(d + 8); a->status = le32(d + 12);
        return ArWriteDone;
    case AR_OP_READ_DONE:
        if (p.payload_bytes < 20) return ArBad;
        a->status = le32(d); a->address = le32(d + 4) | (uint64_t)le32(d + 8) << 32; a->handle = le32(d + 12); a->data_bytes = le32(d + 16);
        return ArReadDone;
    default:
        return ArUnknown;
    }
}
