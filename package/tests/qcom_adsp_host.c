// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the ADSP bring-up cores: firmware image parsing, SMEM partitions and items, SMP2P, GLINK (two endpoints talking over
// shared FIFOs, plus the exact wire bytes), the AOSS QMP handshake, the cmd-db lookup and RSC request encoding, and the SCM ids.
// No hardware: every shared memory is a host buffer and every doorbell a counter.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../QcomGlink.h"
#include "../QcomMdt.h"
#include "../QcomQmp.h"
#include "../QcomRpmh.h"
#include "../QcomScm.h"
#include "../QcomSmem.h"
#include "../QcomSmp2p.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

static void put16(uint8_t *p, uint16_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);}
static void put32(uint8_t *p, uint32_t v) {put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16));}

/* ---- firmware images ---- */
typedef struct {const uint8_t *data; size_t size; unsigned reads;} Blob;
static int blob_read(void *context, uint64_t offset, void *to, size_t length)
{
    Blob *b = context;
    b->reads++;
    if (offset > b->size || length > b->size - offset) return -1;
    memcpy(to, b->data + offset, length);
    return 0;
}

/* An ELF32 image: PT_NULL header segment, the hash segment, two relocatable PT_LOAD segments (the second with a zero tail). */
static size_t make_elf32(uint8_t *f, int with_hash, uint32_t flags_extra)
{
    memset(f, 0, 4096);
    memcpy(f, "\x7f" "ELF", 4);
    f[4] = 1; f[5] = 1; f[6] = 1;
    put16(f + 16, 2); put16(f + 18, 164);
    put32(f + 28, 52);              /* phoff */
    put16(f + 40, 52); put16(f + 42, 32); put16(f + 44, 4);
    const uint32_t hdr = 52 + 4 * 32;
    struct {uint32_t type, off, paddr, filesz, memsz, flags;} ph[4] = {
        {0, 0, 0, hdr, 0, 7u << 24},
        {0, 0x200, 0x8a000000, 0x40, 0x1000, with_hash ? (2u << 24) : 0},
        {1, 0x400, 0x8a100000, 0x100, 0x100, 0x5 | flags_extra},
        {1, 0x600, 0x8a101000, 0x80, 0x200, 0x6 | flags_extra},
    };
    for (unsigned i = 0; i < 4; ++i) {
        uint8_t *p = f + 52 + 32 * i;
        put32(p, ph[i].type); put32(p + 4, ph[i].off); put32(p + 8, ph[i].paddr); put32(p + 12, ph[i].paddr);
        put32(p + 16, ph[i].filesz); put32(p + 20, ph[i].memsz); put32(p + 24, ph[i].flags); put32(p + 28, 0x1000);
    }
    for (unsigned i = 0; i < 0x40; ++i) f[0x200 + i] = (uint8_t)(0xa0 + i);
    for (unsigned i = 0; i < 0x100; ++i) f[0x400 + i] = (uint8_t)i;
    return 0x680;
}

static void test_mdt(void)
{
    static uint8_t f[4096];
    size_t n = make_elf32(f, 1, QCOM_MDT_RELOCATABLE);
    Blob b = {f, n, 0};
    QcomMdtImage image;
    CHECK(qcom_mdt_parse(&image, n, blob_read, &b) == QcomMdtOk);
    CHECK(!image.elf64 && image.count == 4 && image.hash == 1 && image.relocatable);
    CHECK(image.header_size == 52 + 4 * 32);
    CHECK(image.min_address == 0x8a100000 && image.max_address == 0x8a101200);
    CHECK(qcom_mdt_span(&image) == 0x2000);  /* the end rounded up to 4 KiB */
    CHECK(image.segment[2].load && image.segment[3].load && !image.segment[0].load && !image.segment[1].load);
    uint8_t meta[1024];
    CHECK(qcom_mdt_metadata_size(&image) == image.header_size + 0x40);
    CHECK(qcom_mdt_metadata(&image, blob_read, &b, meta, sizeof meta) == QcomMdtOk);
    CHECK(!memcmp(meta, f, image.header_size) && meta[image.header_size] == 0xa0 && meta[image.header_size + 0x3f] == 0xdf);
    CHECK(qcom_mdt_metadata(&image, blob_read, &b, meta, 10) == QcomMdtTooSmall);
    uint64_t at;
    CHECK(qcom_mdt_place(&image, 2, 0x9ea00000, 0x4680000, &at) == QcomMdtOk && at == 0);
    CHECK(qcom_mdt_place(&image, 3, 0x9ea00000, 0x4680000, &at) == QcomMdtOk && at == 0x1000);
    CHECK(qcom_mdt_place(&image, 3, 0x9ea00000, 0x1100, &at) == QcomMdtOutside);
    CHECK(qcom_mdt_place(&image, 1, 0x9ea00000, 0x4680000, &at) == QcomMdtOutside);
    /* Fixed (not relocatable) images go by address. */
    n = make_elf32(f, 1, 0);
    CHECK(qcom_mdt_parse(&image, n, blob_read, &b) == QcomMdtOk && !image.relocatable);
    CHECK(qcom_mdt_place(&image, 3, 0x8a100000, 0x2000, &at) == QcomMdtOk && at == 0x1000);
    CHECK(qcom_mdt_place(&image, 2, 0x8a200000, 0x2000, &at) == QcomMdtOutside);
    /* No hash segment, not an ELF, truncated program headers, file shorter than a segment. */
    n = make_elf32(f, 0, QCOM_MDT_RELOCATABLE);
    CHECK(qcom_mdt_parse(&image, n, blob_read, &b) == QcomMdtNoHash);
    n = make_elf32(f, 1, QCOM_MDT_RELOCATABLE);
    f[1] = 'X';
    CHECK(qcom_mdt_parse(&image, n, blob_read, &b) == QcomMdtNotElf);
    f[1] = 'E';
    CHECK(qcom_mdt_parse(&image, 100, blob_read, &b) == QcomMdtBadHeaders);
    CHECK(qcom_mdt_parse(&image, 0x500, blob_read, &b) == QcomMdtOk);
    CHECK(qcom_mdt_place(&image, 3, 0, 0x4680000, &at) == QcomMdtOutside);
    /* The split form: the hash packed right after the headers, nothing else in the file. */
    static uint8_t split[512];
    memcpy(split, f, image.header_size);
    memcpy(split + image.header_size, f + 0x200, 0x40);
    Blob sb = {split, image.header_size + 0x40, 0};
    QcomMdtImage s;
    CHECK(qcom_mdt_parse(&s, sb.size, blob_read, &sb) == QcomMdtOk);
    CHECK(qcom_mdt_metadata(&s, blob_read, &sb, meta, sizeof meta) == QcomMdtOk && meta[s.header_size] == 0xa0);
    /* ELF64 with one load segment. */
    memset(f, 0, sizeof f);
    memcpy(f, "\x7f" "ELF", 4);
    f[4] = 2; f[5] = 1;
    put32(f + 32, 64); put16(f + 54, 56); put16(f + 56, 3);
    const uint32_t hdr = 64 + 3 * 56;
    uint8_t *p = f + 64;
    put32(p, 0); put32(p + 4, 7u << 24); put32(p + 32, hdr);
    p += 56; put32(p, 0); put32(p + 4, 2u << 24); put32(p + 8, 0x300); put32(p + 32, 0x20); put32(p + 40, 0x20);
    p += 56; put32(p, 1); put32(p + 4, 5); put32(p + 8, 0x400); put32(p + 24, 0x1000); put32(p + 32, 0x10); put32(p + 40, 0x10);
    Blob b64 = {f, 0x410, 0};
    CHECK(qcom_mdt_parse(&image, 0x410, blob_read, &b64) == QcomMdtOk && image.elf64 && image.hash == 1);
    CHECK(image.min_address == 0x1000 && image.max_address == 0x1010 && image.segment[2].file_offset == 0x400);
}

/* ---- SMEM ---- */
#define SMEM_BYTES 0x40000u
static uint32_t smem_words[SMEM_BYTES / 4];
static QcomSmem smem = {(volatile uint8_t *)smem_words, SMEM_BYTES};
static void w32(uint32_t at, uint32_t v) {smem_words[at / 4] = v;}

static void make_partition(uint32_t toc_index, uint32_t offset, uint32_t size, uint16_t h0, uint16_t h1, uint32_t cacheline)
{
    const uint32_t e = SMEM_BYTES - 0x1000 + 32 + 48 * toc_index;
    w32(e, offset); w32(e + 4, size); w32(e + 8, 0); w32(e + 12, h0 | (uint32_t)h1 << 16); w32(e + 16, cacheline);
    w32(offset, 0x54525024u); w32(offset + 4, h0 | (uint32_t)h1 << 16); w32(offset + 8, size);
    w32(offset + 12, 32); w32(offset + 16, size);
}

static void test_smem(void)
{
    memset(smem_words, 0, sizeof smem_words);
    QcomSmemPartition p;
    CHECK(qcom_smem_partition(&smem, 0, 2, &p) == QcomSmemNoTable);
    const uint32_t toc = SMEM_BYTES - 0x1000;
    w32(toc, 0x434f5424u); w32(toc + 4, 1); w32(toc + 8, 3);
    make_partition(0, 0x1000, 0x8000, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, 128);
    make_partition(1, 0x9000, 0x6000, 1, 0, 64);  /* apps-modem: not ours */
    make_partition(2, 0x10000, 0x10000, 2, 0, 64); /* adsp-apps, hosts listed the other way round */
    CHECK(qcom_smem_partition(&smem, 0, 2, &p) == QcomSmemOk && p.offset == 0x10000 && p.size == 0x10000 && p.cacheline == 64);
    CHECK(qcom_smem_partition(&smem, 0, 5, &p) == QcomSmemNoPartition);
    QcomSmemPartition g;
    CHECK(qcom_smem_partition(&smem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &g) == QcomSmemOk && g.offset == 0x1000);
    CHECK(qcom_smem_partition(&smem, 0, 2, &p) == QcomSmemOk);
    uint32_t at, size;
    CHECK(qcom_smem_find(&smem, &p, 478, &at, &size) == QcomSmemMissing);
    CHECK(qcom_smem_alloc(&smem, &p, 478, 32) == QcomSmemOk);
    CHECK(qcom_smem_alloc(&smem, &p, 479, 0x4000) == QcomSmemOk);
    CHECK(qcom_smem_alloc(&smem, &p, 429, 341) == QcomSmemOk);  /* rounded to 344, padding 3 */
    CHECK(qcom_smem_alloc(&smem, &p, 478, 32) == QcomSmemExists);
    CHECK(qcom_smem_find(&smem, &p, 478, &at, &size) == QcomSmemOk && at == 0x10000 + 32 + 16 && size == 32);
    CHECK(qcom_smem_find(&smem, &p, 479, &at, &size) == QcomSmemOk && at == 0x10000 + 32 + 48 + 16 && size == 0x4000);
    CHECK(qcom_smem_find(&smem, &p, 429, &at, &size) == QcomSmemOk && size == 341 && (at & 7) == 0);
    CHECK(smem_words[(0x10000 + 12) / 4] == 32 + 48 + 16 + 0x4000 + 16 + 344);
    /* A cached item made by the remote at the top of the partition: entry in the last cache line slot, data below it. */
    const uint32_t top = 0x10000 + 0x10000 - 64;
    w32(top, 0xa5a5u | 480u << 16); w32(top + 4, 0x2000); w32(top + 8, 0);
    w32(0x10000 + 16, 0x10000 - 64 - 0x2000);
    CHECK(qcom_smem_find(&smem, &p, 480, &at, &size) == QcomSmemOk && at == top - 0x2000 && size == 0x2000);
    CHECK(qcom_smem_alloc(&smem, &p, 480, 8) == QcomSmemExists);
    /* No room once the uncached list would run into the cached one. */
    CHECK(qcom_smem_alloc(&smem, &p, 600, 0x10000) == QcomSmemNoSpace);
    /* A broken canary is reported, not walked past. */
    smem_words[(0x10000 + 32) / 4] ^= 1;
    CHECK(qcom_smem_find(&smem, &p, 429, &at, &size) == QcomSmemCorrupt);
    CHECK(qcom_smem_alloc(&smem, &p, 601, 8) == QcomSmemCorrupt);
    smem_words[(0x10000 + 32) / 4] ^= 1;
    /* A partition header that disagrees with the table is refused. */
    w32(0x10000 + 8, 0x1234);
    CHECK(qcom_smem_partition(&smem, 0, 2, &p) == QcomSmemBadPartition);
}

/* ---- SMP2P ---- */
static void test_smp2p(void)
{
    static uint32_t ours[QCOM_SMP2P_ITEM_BYTES / 4], theirs[QCOM_SMP2P_ITEM_BYTES / 4];
    QcomSmp2p s, remote;
    qcom_smp2p_init_out(&s, ours, 0, 2);
    CHECK(ours[0] == QCOM_SMP2P_MAGIC && ours[1] == (1u | 1u << 8) && ours[2] == (0u | 2u << 16) && ours[3] == 16);
    const int stop = qcom_smp2p_add_entry(&s, "master-kernel");
    CHECK(stop == 0 && ours[3] == (16u | 1u << 16) && !memcmp(&ours[5], "master-kernel", 13));
    CHECK(qcom_smp2p_update(&s, stop, 1, 1) == 1 && qcom_smp2p_out_value(&s, stop) == 1);
    CHECK(qcom_smp2p_update(&s, stop, 1, 1) == 0);
    CHECK(qcom_smp2p_update(&s, stop, 1, 0) == 1 && qcom_smp2p_out_value(&s, stop) == 0);
    /* The DSP's item, as the DSP writes it (pid 2 local, 0 remote), with its slave-kernel entry. */
    qcom_smp2p_init_out(&remote, theirs, 2, 0);
    const int slave = qcom_smp2p_add_entry(&remote, "slave-kernel");
    CHECK(qcom_smp2p_check_in(theirs, 0, 2) == 0);
    CHECK(qcom_smp2p_check_in(theirs, 0, 3) == -3);
    uint32_t value, changed;
    CHECK(qcom_smp2p_service(&s, "slave-kernel", &value, &changed) == 0);  /* no inbound item yet */
    s.in = theirs;
    CHECK(qcom_smp2p_find_in(&s, "slave-kernel") == 0 && qcom_smp2p_find_in(&s, "sleepstate") == -1);
    qcom_smp2p_service(&s, "slave-kernel", &value, &changed);
    CHECK(s.negotiated && s.ssr_ack_enabled && value == 0 && changed == 0);
    /* Ready and handover (bits 1 and 2) arrive, then fatal (bit 0). */
    qcom_smp2p_update(&remote, slave, 6, 6);
    qcom_smp2p_service(&s, "slave-kernel", &value, &changed);
    CHECK(value == 6 && changed == 6);
    qcom_smp2p_service(&s, "slave-kernel", &value, &changed);
    CHECK(value == 6 && changed == 0);
    qcom_smp2p_update(&remote, slave, 1, 1);
    qcom_smp2p_service(&s, "slave-kernel", &value, &changed);
    CHECK(value == 7 && changed == 1);
    /* A restart: the DSP sets restart done, we answer with restart ack (and ring), then both clear. */
    theirs[4] |= QCOM_SMP2P_FLAG_RESTART_DONE;
    CHECK(qcom_smp2p_service(&s, "slave-kernel", &value, &changed) == 1 && (ours[4] & QCOM_SMP2P_FLAG_RESTART_ACK));
    CHECK(qcom_smp2p_service(&s, "slave-kernel", &value, &changed) == 0);
    theirs[4] &= ~QCOM_SMP2P_FLAG_RESTART_DONE;
    CHECK(qcom_smp2p_service(&s, "slave-kernel", &value, &changed) == 1 && !(ours[4] & QCOM_SMP2P_FLAG_RESTART_ACK));
    /* A remote without the SSR ack feature: our features drop to the common set. */
    QcomSmp2p t;
    static uint32_t ours2[QCOM_SMP2P_ITEM_BYTES / 4];
    qcom_smp2p_init_out(&t, ours2, 0, 2);
    theirs[1] = 1;
    t.in = theirs;
    CHECK(qcom_smp2p_service(&t, "slave-kernel", &value, &changed) == 1 && ours2[1] == 1 && !t.ssr_ack_enabled);
    for (int i = 0; i < 16; ++i) CHECK(qcom_smp2p_add_entry(&t, "x") == i);
    CHECK(qcom_smp2p_add_entry(&t, "x") == -1);
}

/* ---- GLINK ---- */
#define FIFO 1024u
static uint32_t desc[4];
static uint8_t fifo_a[FIFO], fifo_b[FIFO];  /* a: A's transmit FIFO (item 479), b: B's (item 480) */
static unsigned kicks_a, kicks_b;
static void kick_a(void *c) {(void)c; kicks_a++;}
static void kick_b(void *c) {(void)c; kicks_b++;}

typedef struct {unsigned ups, downs, messages; uint8_t last[256]; uint32_t last_length;} Seen;
static void on_state(void *context, QcomGlinkChannel *c, int up) {(void)c; Seen *s = context; if (up) s->ups++; else s->downs++;}
static void on_receive(void *context, QcomGlinkChannel *c, const uint8_t *data, uint32_t length)
{
    (void)c;
    Seen *s = context;
    s->messages++;
    s->last_length = length;
    memcpy(s->last, data, length < sizeof s->last ? length : sizeof s->last);
}

static void settle(QcomGlink *a, QcomGlink *b)
{
    for (int i = 0; i < 20; ++i) {
        int n = qcom_glink_poll(a);
        CHECK(n >= 0);
        int m = qcom_glink_poll(b);
        CHECK(m >= 0);
        if (!n && !m && i > 2) break;
    }
}

static void test_glink(void)
{
    static QcomGlink a, b;
    static uint8_t store_a[4 * 256], store_b[4 * 256];
    memset(desc, 0xee, sizeof desc);  /* garbage before init */
    QcomGlinkFifo a_tx = {&desc[1], &desc[0], fifo_a, FIFO}, a_rx = {&desc[3], &desc[2], 0, 0};
    QcomGlinkFifo b_tx = {&desc[3], &desc[2], fifo_b, FIFO}, b_rx = {&desc[1], &desc[0], fifo_a, FIFO};
    qcom_glink_init(&a, a_tx, a_rx, kick_a, 0);
    desc[3] = 0; desc[0] = 0;  /* the remote's own indices */
    qcom_glink_init(&b, b_tx, b_rx, kick_b, 0);
    const QcomGlinkClient client = {on_state, on_receive};
    Seen sa = {0}, sb = {0};
    QcomGlinkChannel *ca = qcom_glink_channel(&a, "PMIC_RTR_ADSP_APPS", 256, 4, store_a, &client, &sa, 0);
    QcomGlinkChannel *cb = qcom_glink_channel(&b, "PMIC_RTR_ADSP_APPS", 256, 4, store_b, &client, &sb, 1);
    CHECK(ca && cb && ca->lcid == 1);
    /* A's first message is VERSION 1 with the intent reuse feature, exactly these bytes. */
    CHECK(qcom_glink_poll(&a) == 0 && kicks_a == 1);
    static const uint8_t version[8] = {0, 0, 1, 0, 1, 0, 0, 0};
    CHECK(!memcmp(fifo_a, version, 8) && desc[1] == 8);
    /* The remote's FIFO is not there yet: A only gets it once B exists. */
    qcom_glink_set_rx(&a, fifo_b, FIFO);
    settle(&a, &b);
    CHECK(a.version_agreed && b.version_agreed && a.features == 1);
    CHECK(ca->up && cb->up && sa.ups == 1 && sb.ups == 1 && ca->rcid == cb->lcid && cb->rcid == ca->lcid);
    /* Requests and answers, many times over so both FIFOs wrap. */
    for (unsigned i = 0; i < 200; ++i) {
        uint8_t m[40];
        for (unsigned k = 0; k < sizeof m; ++k) m[k] = (uint8_t)(i + k);
        CHECK(qcom_glink_send(&b, cb, m, 12 + i % 29) == QcomGlinkOk);
        settle(&a, &b);
        CHECK(sa.messages == i + 1 && sa.last_length == 12 + i % 29 && !memcmp(sa.last, m, sa.last_length));
        CHECK(qcom_glink_send(&a, ca, m, 3 + i % 7) == QcomGlinkOk);
        settle(&a, &b);
        CHECK(sb.messages == i + 1 && sb.last_length == 3 + i % 7);
    }
    CHECK(a.dropped == 0 && b.dropped == 0 && a.rx_data == 200 && b.rx_data == 200);
    /* Four sends with no answer in between use up B's four intents of A; the fifth waits, asks once, and goes after A is done. */
    for (unsigned i = 0; i < 4; ++i) CHECK(qcom_glink_send(&b, cb, "abcd", 4) == QcomGlinkOk);
    CHECK(qcom_glink_send(&b, cb, "efgh", 4) == QcomGlinkAgain && cb->intent_asked);
    CHECK(qcom_glink_send(&b, cb, "efgh", 4) == QcomGlinkAgain);
    settle(&a, &b);
    CHECK(sa.messages == 204);
    CHECK(qcom_glink_send(&b, cb, "efgh", 4) == QcomGlinkOk);
    settle(&a, &b);
    CHECK(sa.messages == 205 && !memcmp(sa.last, "efgh", 4));
    /* Bigger than every intent: stays pending. */
    uint8_t big[300] = {0};
    CHECK(qcom_glink_send(&b, cb, big, sizeof big) == QcomGlinkAgain);
    settle(&a, &b);
    /* The remote closes: both sides go down, A answers with CLOSE_ACK and its own CLOSE, then waits for a new OPEN. */
    QcomGlinkChannel saved = *cb;
    (void)saved;
    {
        uint8_t m[8] = {QCOM_GLINK_CLOSE, 0, (uint8_t)cb->lcid, 0, 0, 0, 0, 0};
        uint32_t head = desc[3];
        for (unsigned i = 0; i < 8; ++i) {fifo_b[head] = m[i]; head = (head + 1) % FIFO;}
        desc[3] = head;
        cb->up = 0;
    }
    qcom_glink_poll(&a);
    CHECK(sa.downs == 1 && !ca->up && !ca->remote_open && ca->open_sent == 2);
    CHECK(qcom_glink_send(&a, ca, "x", 1) == QcomGlinkNotOpen);
    /* An unknown channel name opened by the remote is counted and left alone. */
    {
        uint8_t m[8 + 8] = {QCOM_GLINK_OPEN, 0, 9, 0, 6, 0, 0, 0, 'q', 'r', 't', 'r', 'x', 0, 0, 0};
        uint32_t head = desc[3];
        for (unsigned i = 0; i < sizeof m; ++i) {fifo_b[head] = m[i]; head = (head + 1) % FIFO;}
        desc[3] = head;
    }
    CHECK(qcom_glink_poll(&a) == 1 && a.unknown_opens == 1 && !strcmp(a.last_unknown, "qrtrx"));
    /* Every distinct unknown name is listed once. */
    {
        const char *names[3] = {"apr", "qrtrx", "IPCRTR"};
        for (unsigned k = 0; k < 3; ++k) {
            uint8_t m[8 + 8] = {QCOM_GLINK_OPEN, 0, (uint8_t)(10 + k), 0, (uint8_t)(strlen(names[k]) + 1), 0, 0, 0};
            memcpy(m + 8, names[k], strlen(names[k]));
            uint32_t head = desc[3];
            for (unsigned i = 0; i < sizeof m; ++i) {fifo_b[head] = m[i]; head = (head + 1) % FIFO;}
            desc[3] = head;
        }
    }
    CHECK(qcom_glink_poll(&a) == 3 && a.unknown_opens == 4 && !strcmp(a.unknown_names, "qrtrx apr IPCRTR"));
    /* The remote opens our channel again: OPEN_ACK, then our OPEN with the name, padded. */
    {
        const uint32_t before = desc[1];
        uint8_t m[8 + 24] = {QCOM_GLINK_OPEN, 0, 7, 0, 19, 0, 0, 0};
        memcpy(m + 8, "PMIC_RTR_ADSP_APPS", 19);
        uint32_t head = desc[3];
        for (unsigned i = 0; i < sizeof m; ++i) {fifo_b[head] = m[i]; head = (head + 1) % FIFO;}
        desc[3] = head;
        CHECK(qcom_glink_poll(&a) == 1 && ca->remote_open && ca->rcid == 7 && ca->open_sent == 1);
        uint8_t ack[8], open[8 + 24];
        for (unsigned i = 0; i < 8; ++i) ack[i] = fifo_a[(before + i) % FIFO];
        for (unsigned i = 0; i < sizeof open; ++i) open[i] = fifo_a[(before + 8 + i) % FIFO];
        static const uint8_t want_ack[8] = {QCOM_GLINK_OPEN_ACK, 0, 7, 0, 0, 0, 0, 0};
        CHECK(!memcmp(ack, want_ack, 8));
        CHECK(open[0] == QCOM_GLINK_OPEN && open[2] == ca->lcid && open[4] == 19 && !memcmp(open + 8, "PMIC_RTR_ADSP_APPS", 19));
        CHECK(desc[1] == (before + 8 + 32) % FIFO);
    }
    /* A command that is not GLINK stops the link for good. */
    {
        uint8_t m[8] = {0x77, 0, 0, 0, 0, 0, 0, 0};
        uint32_t head = desc[3];
        for (unsigned i = 0; i < 8; ++i) {fifo_b[head] = m[i]; head = (head + 1) % FIFO;}
        desc[3] = head;
    }
    CHECK(qcom_glink_poll(&a) == QcomGlinkCorrupt && a.broken);
    CHECK(qcom_glink_poll(&a) == QcomGlinkCorrupt);
    /* FIFO accounting: 8 bytes are never used and 16 more stay for a read notification. */
    uint32_t h = 0, t = 0;
    QcomGlinkFifo f = {&h, &t, fifo_a, FIFO};
    CHECK(qcom_glink_tx_avail(&f) == FIFO - 24 && qcom_glink_rx_avail(&f) == 0);
    h = 100; t = 40;
    CHECK(qcom_glink_tx_avail(&f) == FIFO - 60 - 24 && qcom_glink_rx_avail(&f) == 60);
    h = 40; t = 100;
    CHECK(qcom_glink_tx_avail(&f) == 60 - 24 && qcom_glink_rx_avail(&f) == FIFO - 60);
    h = FIFO; /* out of range indices are never trusted */
    CHECK(qcom_glink_tx_avail(&f) == 0 && qcom_glink_rx_avail(&f) == 0);
}

/* ---- AOSS QMP ---- */
static unsigned qmp_kicks;
static void qmp_kick(void *c) {(void)c; qmp_kicks++;}
static void test_qmp(void)
{
    static uint32_t ram[0x400 / 4];
    QcomQmp q;
    qcom_qmp_init(&q, ram, sizeof ram, qmp_kick, 0);
    CHECK(qcom_qmp_step(&q) == QcomQmpBadDescriptor);
    memset(ram, 0, sizeof ram);
    ram[0] = 0x4d41494cu; ram[1] = 1; ram[3] = 0xffff; ram[13] = 0x100; ram[14] = 0x100;
    qcom_qmp_init(&q, ram, sizeof ram, qmp_kick, 0);
    CHECK(qcom_qmp_step(&q) == QcomQmpLinkWait && ram[4] == 0xffff && ram[9] == 0xffff && qmp_kicks == 1);
    CHECK(qcom_qmp_step(&q) == QcomQmpLinkWait && qmp_kicks == 1);
    ram[10] = 0xffff;  /* the AOP acks our link */
    CHECK(qcom_qmp_step(&q) == QcomQmpChannelWait && ram[11] == 0xffff && qmp_kicks == 2);
    ram[5] = 0xffff;   /* the AOP's channel */
    CHECK(qcom_qmp_step(&q) == QcomQmpAckWait && ram[6] == 0xffff && qmp_kicks == 3);
    ram[12] = 0xffff;  /* our channel acked */
    CHECK(qcom_qmp_step(&q) == QcomQmpOpen);
    CHECK(qcom_qmp_send(&q, "{class: image, res: load_state, name: adsp, val: on}") == QcomQmpSending);
    CHECK(ram[0x100 / 4] == QCOM_QMP_MESSAGE && !memcmp(&ram[0x104 / 4], "{class: image", 13) && qmp_kicks == 4);
    CHECK(qcom_qmp_send(&q, "again") == QcomQmpBusy);
    CHECK(qcom_qmp_step(&q) == QcomQmpSending);
    ram[0x100 / 4] = 0;  /* taken */
    CHECK(qcom_qmp_step(&q) == QcomQmpOpen);
    char longer[200];
    memset(longer, 'a', sizeof longer - 1);
    longer[sizeof longer - 1] = 0;
    CHECK(qcom_qmp_send(&q, longer) == QcomQmpTooLong);
    CHECK(qcom_qmp_send(&q, "x") == QcomQmpSending);
    qcom_qmp_abandon(&q);
    CHECK(q.state == QcomQmpOpen && ram[0x100 / 4] == 0);
    /* Opened by an earlier boot stage: used as it is, no kick. */
    const unsigned before = qmp_kicks;
    qcom_qmp_init(&q, ram, sizeof ram, qmp_kick, 0);
    CHECK(qcom_qmp_step(&q) == QcomQmpOpen && qmp_kicks == before);
    /* A mailbox outside the RAM is refused. */
    ram[14] = 0x3f0;
    qcom_qmp_init(&q, ram, sizeof ram, qmp_kick, 0);
    CHECK(qcom_qmp_step(&q) == QcomQmpBadDescriptor);
}

/* ---- cmd-db and RSC ---- */
static void test_rpmh(void)
{
    static uint8_t db[0x1000];
    memset(db, 0, sizeof db);
    put32(db, 1); put32(db + 4, QCOM_CMD_DB_MAGIC);
    /* Slave 3 (ARC): 3 entries at data offset 0, aux area at 0x200. */
    put16(db + 8, 3); put16(db + 10, 0); put16(db + 12, 0x200); put16(db + 14, 3);
    const char *names[3] = {"cx.lvl", "lcx.lvl", "lmx.lvl"};
    for (unsigned i = 0; i < 3; ++i) {
        uint8_t *e = db + 144 + 24 * i;
        memcpy(e, names[i], strlen(names[i]));
        put32(e + 16, 0x30000 + 0x10 * i); put16(e + 20, 16); put16(e + 22, (uint16_t)(32 * i));
        static const uint16_t levels[8] = {0, 16, 48, 64, 128, 192, 256, 384};
        for (unsigned l = 0; l < 8; ++l) put16(db + 144 + 0x200 + 32 * i + 2 * l, levels[l]);
    }
    QcomRpmhResource r;
    CHECK(qcom_cmd_db_find(db, sizeof db, "lcx.lvl", &r) == QcomRpmhOk && r.address == 0x30010 && r.level_count == 8);
    CHECK(r.levels[0] == 0 && r.levels[7] == 384);
    uint32_t index;
    CHECK(qcom_rpmh_level_index(&r, 0x7fffffff, &index) == QcomRpmhOk && index == 7);
    CHECK(qcom_rpmh_level_index(&r, 100, &index) == QcomRpmhOk && index == 4);
    CHECK(qcom_rpmh_level_index(&r, 0, &index) == QcomRpmhOk && index == 0);
    CHECK(qcom_cmd_db_find(db, sizeof db, "mx.lvl", &r) == QcomRpmhMissing);
    put16(db + 14, 300);  /* entries past the end */
    CHECK(qcom_cmd_db_find(db, sizeof db, "lmx.lvl", &r) == QcomRpmhCorrupt);
    put32(db + 4, 0);
    CHECK(qcom_cmd_db_find(db, sizeof db, "lmx.lvl", &r) == QcomRpmhNoDb);
    /* Request writes, both layouts. */
    QcomRpmhWrite w[32];
    const uint32_t addresses[2] = {0x30010, 0x30020}, values[2] = {7, 7};
    QcomRscLayout v2 = qcom_rsc_layout(0x00020700), v3 = qcom_rsc_layout(0x00030000);
    CHECK(v2.cmd_stride == 20 && v3.cmd_stride == 24 && v3.control == 0x24);
    unsigned n = qcom_rpmh_active_request(&v3, 1, addresses, values, 2, w, 32);
    CHECK(n == 12);
    CHECK(w[0].offset == 672 + 0x24 && w[0].value == 0);
    CHECK(w[1].offset == 0x08 && w[1].value == 2);
    CHECK(w[2].offset == 672 + 0x34 && w[2].value == 0x10108 && w[3].offset == 672 + 0x38 && w[3].value == 0x30010);
    CHECK(w[5].offset == 672 + 24 + 0x34 && w[7].offset == 672 + 24 + 0x3c && w[7].value == 7);
    CHECK(w[8].offset == 672 + 0x20 && w[8].value == 3 && w[9].offset == 672 + 0x2c && w[9].value == 3);
    CHECK(w[10].value == 1u << 16 && w[11].value == (1u << 16 | 1u << 24) && w[11].offset == 672 + 0x24);
    n = qcom_rpmh_active_request(&v2, 0, addresses, values, 2, w, 32);
    CHECK(n == 12 && w[2].offset == 0x30 && w[5].offset == 20 + 0x30 && w[11].offset == 0x14);
    CHECK(qcom_rpmh_active_request(&v2, 0, addresses, values, 5, w, 32) == 0);
    CHECK(qcom_rpmh_finish(&v3, 1, w, 32) == 3 && w[0].value == 2 && w[1].offset == 672 + 0x24);
}

static void test_scm(void)
{
    CHECK(qcom_scm_fn(QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_INIT_IMAGE) == 0x42000201u);
    CHECK(qcom_scm_fn(QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_AUTH_AND_RESET) == 0x42000205u);
    CHECK(qcom_scm_fn(QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_SHUTDOWN) == 0x42000206u);
    CHECK(qcom_scm_args(2, QCOM_SCM_VAL, QCOM_SCM_RW, 0) == 0x82u);
    CHECK(qcom_scm_args(3, 0, 0, 0) == 3u && qcom_scm_args(1, 0, 0, 0) == 1u);
    CHECK(qcom_ipcc_id(QCOM_IPCC_CLIENT_LPASS, QCOM_IPCC_SIGNAL_SMP2P) == 0x30002u);
}

int main(void)
{
    test_mdt();
    test_smem();
    test_smp2p();
    test_glink();
    test_qmp();
    test_rpmh();
    test_scm();
    printf("PASS qcom adsp cores: %u checks\n", checks);
    return 0;
}
