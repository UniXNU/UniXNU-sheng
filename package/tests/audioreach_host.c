// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the graph manager commands (package/ShengAudioReach.c): packet sizes, parameter block layout and the fields the DSP
// reads, checked against the layout the DSP's reference client produces.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ShengAudioReach.h"
#include "../ShengGpr.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static uint32_t le16(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8;}

/* Walks the parameter blocks of an APM command; returns the count and checks each is 8-byte sized and stays inside the payload. */
static unsigned blocks(const uint8_t *pkt, uint32_t n, uint32_t *iid, uint32_t *id, uint32_t *offset, unsigned max)
{
    GprPacket p;
    CHECK(gpr_decode(pkt, n, &p) == GprOk && p.size == n);
    CHECK(p.payload_bytes >= 16 && le32(p.payload + 12) == p.payload_bytes - 16);   /* in-band, size matches */
    unsigned count = 0;
    for (uint32_t at = 16; at < p.payload_bytes; ) {
        CHECK(at + 16 <= p.payload_bytes);
        const uint32_t size = le32(p.payload + at + 8);
        CHECK(size % 8 == 0 && at + 16 + size <= p.payload_bytes && le32(p.payload + at + 12) == 0);
        CHECK(count < max);
        iid[count] = le32(p.payload + at); id[count] = le32(p.payload + at + 4); offset[count] = 24 + at + 16;
        ++count; at += 16 + size;
    }
    return count;
}

int main(void)
{
    uint8_t b[GPR_MAX_PACKET];
    uint32_t iid[8], id[8], off[8];
    const ArFormat stereo = {48000, 16, 2}, mono = {48000, 16, 1};
    GprPacket p;

    /* Speaker device graph: subgraph, container, one I2S sink module, its ports, no connections. */
    uint32_t n = ar_open_speaker_device(b, sizeof b, 1, 7);
    CHECK(n && n <= GPR_MAX_PACKET);
    CHECK(gpr_decode(b, n, &p) == GprOk && p.opcode == AR_OP_GRAPH_OPEN && p.dst_port == AR_APM_IID && p.src_port == 1 && p.token == 7);
    CHECK(p.dst_domain == GPR_DOMAIN_ADSP && p.src_domain == GPR_DOMAIN_APPS);
    CHECK(blocks(b, n, iid, id, off, 8) == 5);
    CHECK(id[0] == 0x08001001 && id[1] == 0x08001000 && id[2] == 0x08001002 && id[3] == 0x08001003 && id[4] == 0x08001004);
    for (int i = 0; i < 5; ++i) CHECK(iid[i] == AR_APM_IID);
    /* subgraph: count, id, 3 properties: perf low latency, direction TX, scenario playback */
    CHECK(le32(b + off[0]) == 1 && le32(b + off[0] + 4) == AR_SG_SPEAKER && le32(b + off[0] + 8) == 3);
    CHECK(le32(b + off[0] + 12) == 0x0800100E && le32(b + off[0] + 20) == 2 && le32(b + off[0] + 32) == 1 && le32(b + off[0] + 44) == 1);
    CHECK(le32(b + off[0] - 8) == 48);                               /* 4 + 44, padded to 8 */
    /* container: GC type, stream position, 8 KiB stack, ADSP */
    CHECK(le32(b + off[1]) == 1 && le32(b + off[1] + 4) == AR_SG_SPEAKER && le32(b + off[1] + 8) == 4);
    CHECK(le32(b + off[1] + 12) == 0x08001011 && le32(b + off[1] + 16) == 8 && le32(b + off[1] + 20) == 1 && le32(b + off[1] + 24) == 0x0B001001);
    CHECK(le32(b + off[1] + 36) == 1 && le32(b + off[1] + 48) == 8192 && le32(b + off[1] + 60) == 2);
    /* module list and properties */
    CHECK(le32(b + off[2]) == 1 && le32(b + off[2] + 4) == AR_SG_SPEAKER && le32(b + off[2] + 12) == 1);
    CHECK(le32(b + off[2] + 16) == AR_MOD_I2S_SINK && le32(b + off[2] + 20) == AR_IID_SPEAKER_I2S);
    CHECK(le32(b + off[3]) == 1 && le32(b + off[3] + 4) == AR_IID_SPEAKER_I2S && le32(b + off[3] + 12) == 0x08001015);
    CHECK(le32(b + off[3] + 20) == 1 && le32(b + off[3] + 24) == 0);
    CHECK(le32(b + off[4]) == 0);

    /* Playback stream graph: three modules and the connection into the speaker device graph. */
    n = ar_open_playback_stream(b, sizeof b, 1, 8);
    CHECK(n && n <= GPR_MAX_PACKET && blocks(b, n, iid, id, off, 8) == 5);
    CHECK(le32(b + off[0] + 4) == AR_SG_PLAY && le32(b + off[0] + 32) == 2);     /* direction RX */
    CHECK(le32(b + off[2] + 12) == 3 && le32(b + off[2] + 16) == AR_MOD_WR_SHMEM && le32(b + off[2] + 32) == AR_MOD_PCM_CNV);
    CHECK(le32(b + off[3]) == 3 && le32(b + off[3] + 20) == 0 && le32(b + off[3] + 24) == 1);   /* shared memory: no input */
    CHECK(le32(b + off[4]) == 3);
    CHECK(le32(b + off[4] + 4 + 32) == AR_IID_PLAY_CNV && le32(b + off[4] + 4 + 36) == 1 && le32(b + off[4] + 4 + 40) == AR_IID_SPEAKER_I2S &&
          le32(b + off[4] + 4 + 44) == 2);

    /* I2S configuration: LPAIF secondary, SD1, internal word select, 48 kHz 16-bit stereo, frame size factor 1. */
    n = ar_config_speaker_device(b, sizeof b, 1, 9, &stereo);
    CHECK(n && blocks(b, n, iid, id, off, 8) == 3);
    CHECK(iid[0] == AR_IID_SPEAKER_I2S && id[0] == 0x08001019 && le32(b + off[0] - 8) == 16);
    CHECK(le32(b + off[0]) == 0 && le32(b + off[0] + 4) == 1 && le16(b + off[0] + 8) == 2 && le16(b + off[0] + 10) == 1);
    CHECK(id[1] == 0x08001017 && le32(b + off[1]) == 48000 && le16(b + off[1] + 4) == 16 && le16(b + off[1] + 6) == 2 && le32(b + off[1] + 8) == 1);
    CHECK(id[2] == 0x08001018 && le32(b + off[2]) == 1 && le32(b + off[2] - 8) == 8);

    /* Decoder and converter output formats. */
    n = ar_config_playback_stream(b, sizeof b, 1, 10, &stereo);
    CHECK(n && blocks(b, n, iid, id, off, 8) == 2);
    CHECK(iid[0] == AR_IID_PLAY_DEC && iid[1] == AR_IID_PLAY_CNV && id[0] == 0x08001008 && le32(b + off[0] - 8) == 32);
    CHECK(le32(b + off[0]) == 1 && le32(b + off[0] + 4) == 0x09001000 && le32(b + off[0] + 8) == 20);
    CHECK(le16(b + off[0] + 12) == 16 && le16(b + off[0] + 14) == 1 && le16(b + off[0] + 18) == 15 && le16(b + off[0] + 22) == 1);
    CHECK(le16(b + off[1] + 22) == 3 && le16(b + off[1] + 26) == 2 && b[off[1] + 28] == 1 && b[off[1] + 29] == 2);

    /* Shared memory endpoint media format goes to the endpoint's own port. */
    n = ar_config_playback_shmem(b, sizeof b, 1, 11, &stereo);
    CHECK(n && gpr_decode(b, n, &p) == GprOk && p.dst_port == AR_IID_PLAY_SHMEM && blocks(b, n, iid, id, off, 8) == 1);
    CHECK(id[0] == 0x0800100C && le32(b + off[0] - 8) == 32 && le32(b + off[0] + 8) == 36 && le32(b + off[0] + 12) == 48000);
    CHECK(le16(b + off[0] + 26) == 2 && b[off[0] + 28] == 1 && b[off[0] + 29] == 2);

    /* Graph commands list one subgraph. */
    n = ar_graph_command(b, sizeof b, 1, 12, AR_OP_GRAPH_START, AR_SG_PLAY);
    CHECK(n == 24 + 16 + 24 && blocks(b, n, iid, id, off, 8) == 1 && id[0] == 0x08001005 && le32(b + off[0]) == 1 && le32(b + off[0] + 4) == AR_SG_PLAY);

    /* Memory map: region right after the GPR header; 4 KiB rounding. */
    n = ar_map(b, sizeof b, 1, 13, 0x8812345000ull, 5000);
    CHECK(n == 24 + 20 + 16 && gpr_decode(b, n, &p) == GprOk && p.opcode == AR_OP_MEM_MAP);
    CHECK(le16(b + 24) == 3 && le16(b + 26) == 1 && le32(b + 28) == 0 && le32(b + 32) == 0x12345000 && le32(b + 36) == 0x88 && le32(b + 40) == 8192);

    /* Write buffer. */
    n = ar_write(b, sizeof b, 1, 14, 0x9000ull, 0x55, 3840);
    CHECK(n == 24 + 44 && gpr_decode(b, n, &p) == GprOk && p.dst_port == AR_IID_PLAY_SHMEM && le32(b + 24) == 0x9000 && le32(b + 32) == 0x55 &&
          le32(b + 36) == 3840);

    /* Capture side fits and connects from the codec DMA source. */
    n = ar_open_mic_device(b, sizeof b, 1, 15);
    CHECK(n && blocks(b, n, iid, id, off, 8) == 5 && le32(b + off[2] + 16) == AR_MOD_CODEC_DMA_SOURCE && le32(b + off[3] + 24) == 1);
    n = ar_open_capture_stream(b, sizeof b, 1, 16);
    CHECK(n && blocks(b, n, iid, id, off, 8) == 5 && le32(b + off[4] + 4) == AR_IID_MIC_DMA && le32(b + off[4] + 12) == AR_IID_CAP_CNV);
    n = ar_config_mic_device(b, sizeof b, 1, 17, &mono, 1);
    CHECK(n && blocks(b, n, iid, id, off, 8) == 4 && id[2] == 0x08001063 && le32(b + off[2]) == 1 && le32(b + off[2] + 4) == 4 && le32(b + off[2] + 8) == 1);
    n = ar_config_capture_stream(b, sizeof b, 1, 18, &mono);
    CHECK(n && blocks(b, n, iid, id, off, 8) == 2 && iid[1] == AR_IID_CAP_ENC);
    n = ar_read(b, sizeof b, 1, 19, 0xa000ull, 0x56, 1920);
    CHECK(n == 24 + 32 && gpr_decode(b, n, &p) == GprOk && p.dst_port == AR_IID_CAP_SHMEM);

    /* Too small a buffer refuses. */
    CHECK(ar_open_playback_stream(b, 100, 1, 1) == 0);

    /* Answers. */
    GprPacket r = {.dst_domain = GPR_DOMAIN_APPS, .src_domain = GPR_DOMAIN_ADSP, .src_port = 1, .dst_port = 1, .token = 5, .opcode = GPR_OP_BASIC_RESULT};
    uint32_t words[10] = {AR_OP_GRAPH_OPEN, 0};
    ArAnswer a;
    n = gpr_encode(b, sizeof b, &r, words, 8);
    CHECK(ar_parse(b, n, &a) == ArBasicResult && a.result_opcode == AR_OP_GRAPH_OPEN && a.status == 0 && a.token == 5);
    r.opcode = AR_OP_MEM_MAP_DONE; words[0] = 0x1234;
    n = gpr_encode(b, sizeof b, &r, words, 4);
    CHECK(ar_parse(b, n, &a) == ArMapped && a.handle == 0x1234);
    r.opcode = AR_OP_WRITE_DONE; words[0] = 0x9000; words[1] = 0; words[2] = 0x55; words[3] = 0;
    n = gpr_encode(b, sizeof b, &r, words, 32);
    CHECK(ar_parse(b, n, &a) == ArWriteDone && a.address == 0x9000 && a.handle == 0x55);
    r.opcode = AR_OP_READ_DONE; words[0] = 0; words[1] = 0xa000; words[2] = 0; words[3] = 0x56; words[4] = 1920;
    n = gpr_encode(b, sizeof b, &r, words, 40);
    CHECK(ar_parse(b, n, &a) == ArReadDone && a.address == 0xa000 && a.data_bytes == 1920);
    CHECK(ar_parse(b, 10, &a) == ArBad);
    r.opcode = GPR_OP_APM_SPF_STATE; words[0] = 1;
    n = gpr_encode(b, sizeof b, &r, words, 4);
    CHECK(ar_parse(b, n, &a) == ArFrameworkState && a.status == 1);
    r.opcode = 0x12345678;
    n = gpr_encode(b, sizeof b, &r, words, 4);
    CHECK(ar_parse(b, n, &a) == ArUnknown && a.opcode == 0x12345678);

    /* PRM: a clock request (one clock: id, hz, attribute 1, root 0), its release (id only), a hardware vote. */
    n = ar_prm_clock(b, sizeof b, 2, 20, AR_CLK_TX_CORE_MCLK, 19200000);
    CHECK(n && gpr_decode(b, n, &p) == GprOk && p.dst_port == AR_PRM_IID && p.opcode == AR_OP_PRM_REQUEST);
    CHECK(le32(b + 24 + 12) == 36 && le32(b + 40) == AR_PRM_IID && le32(b + 44) == 0x0800102C && le32(b + 48) == 20);
    CHECK(le32(b + 56) == 1 && le32(b + 60) == 57 && le32(b + 64) == 19200000 && le32(b + 68) == 1 && le32(b + 72) == 0);
    n = ar_prm_clock(b, sizeof b, 2, 21, AR_CLK_TX_CORE_MCLK, 0);
    CHECK(n && gpr_decode(b, n, &p) == GprOk && p.opcode == AR_OP_PRM_RELEASE && le32(b + 48) == 8 && le32(b + 60) == 57);
    n = ar_prm_vote(b, sizeof b, 2, 22, AR_HW_MACRO, 1);
    CHECK(n == 24 + 16 + 20 && le32(b + 44) == 0x08001032 && le32(b + 48) == 4 && le32(b + 56) == 1);
    r.opcode = AR_OP_PRM_REQUEST_DONE; words[0] = AR_OP_PRM_REQUEST; words[1] = 0;
    n = gpr_encode(b, sizeof b, &r, words, 8);
    CHECK(ar_parse(b, n, &a) == ArPrmDone && a.status == 0);

    printf("PASS audioreach: %u checks\n", checks);
    return 0;
}
