// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the event-driven GENI I2C core (package/GeniI2c.c) against a model of the serial engine with one slave register file,
// and of its buffered transfers against a FIFO of limited depth that drains between events.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../GeniI2c.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

/* The engine model. */
typedef struct {
    uint32_t reg[0x1000 / 4];        /* plain registers */
    uint32_t status;                 /* M_IRQ status */
    int active, op, hold, length, address;
    uint32_t rx, rx_count;
    int present, mute, cancel_ack, abort_ack;
    uint8_t slave[256], pointer;
    uint32_t wide[16], wide_pointer;   /* a device at 0x40 with 32-bit big-endian registers and values */
    uint8_t tx[1024]; int tx_bytes;
    unsigned accesses, commands, fifo_writes;
    /* FIFO mode (writes over 8 bytes, reads over 4): words in the TX FIFO until the next status read drains them; the bytes a read
     * returns, how many are queued in the RX FIFO, and the RX FIFO itself. */
    uint32_t depth, tx_level, tx_peak;
    uint8_t stream[1024]; uint32_t stream_len, stream_pos;
    uint32_t rxq[64], rx_head, rx_level, rx_total, rx_sent;
    unsigned watermarks, rx_events;
    uint32_t short_read;             /* a read command that returns only this many bytes, then ends */
} Engine;

static void rx_refill(Engine *e)
{
    while (e->rx_level < e->depth && e->rx_sent < e->rx_total) {
        uint32_t w = 0;
        for (int k = 0; k < 4 && e->rx_sent < e->rx_total; ++k) w |= (uint32_t)e->stream[e->stream_pos++ % 1024] << (8 * k), ++e->rx_sent;
        e->rxq[(e->rx_head + e->rx_level++) % 64] = w;
    }
    if (e->rx_level) {
        if (e->rx_level >= e->reg[GENI_RX_WATERMARK / 4]) e->status |= GENI_M_RX_WATERMARK;
        if (e->rx_sent == e->rx_total) e->status |= GENI_M_RX_LAST;
        ++e->rx_events;
    }
    if (e->rx_sent == e->rx_total && e->active) {e->status |= GENI_M_CMD_DONE; e->active = 0;}
}

static uint32_t eng_read(void *ctx, uint32_t r)
{
    Engine *e = ctx;
    e->accesses++;
    if (r == GENI_M_IRQ_STATUS) {
        /* Between events the wire takes what the FIFO holds. */
        if (e->active && e->op == GENI_OP_WRITE && e->length > 8 && e->tx_level) {
            e->tx_level = 0;
            if (e->tx_bytes == e->length) {e->tx_bytes = 0; e->active = 0; e->status |= GENI_M_CMD_DONE;}
            else if (e->reg[GENI_TX_WATERMARK / 4]) {e->status |= GENI_M_TX_WATERMARK; ++e->watermarks;}
        }
        if (e->active && e->op == GENI_OP_READ && e->rx_total) rx_refill(e);
        return e->status;
    }
    if (r == GENI_TX_FIFO_STATUS) return e->tx_level;
    if (r == GENI_RX_FIFO_STATUS) return e->rx_total ? e->rx_level : e->rx_count;
    if (r == GENI_RX_FIFO) {
        if (e->rx_total) {CHECK(e->rx_level); const uint32_t w = e->rxq[e->rx_head++ % 64]; --e->rx_level; return w;}
        e->rx_count = 0; return e->rx;
    }
    CHECK(r < sizeof e->reg && !(r & 3));
    return e->reg[r / 4];
}
static void eng_write(void *ctx, uint32_t r, uint32_t v)
{
    Engine *e = ctx;
    e->accesses++;
    switch (r) {
    case GENI_M_IRQ_CLEAR: e->status &= ~v; return;
    case GENI_M_CMD0:
        e->commands++;
        if (e->active) {e->status |= GENI_M_ILLEGAL_CMD | GENI_M_CMD_DONE; return;}
        e->op = (int)(v >> 27); e->address = (int)((v >> 9) & 0x7f); e->hold = (v & GENI_STOP_STRETCH) != 0; e->active = 1;
        if (e->op == GENI_OP_WRITE) {
            e->length = (int)e->reg[GENI_TX_LEN / 4];
            if (e->reg[GENI_TX_WATERMARK / 4] && !e->mute) e->status |= GENI_M_TX_WATERMARK;
        } else {
            const uint32_t n = e->reg[GENI_RX_LEN / 4];
            CHECK(e->op == GENI_OP_READ && (n == 1 || n > 4 || (e->address == 0x40 && n == 4)));
            if (e->mute) return;
            if (!e->present) {e->status |= (1u << 10) | GENI_M_CMD_DONE; e->active = 0; return;}
            if (n > 4) {
                CHECK((e->reg[GENI_M_IRQ_EN / 4] & (GENI_M_RX_WATERMARK | GENI_M_RX_LAST)) == (GENI_M_RX_WATERMARK | GENI_M_RX_LAST));
                CHECK(e->reg[GENI_RX_WATERMARK / 4] && e->reg[GENI_RX_WATERMARK / 4] < e->depth);
                e->rx_total = e->short_read ? e->short_read : n; e->rx_sent = 0; e->rx_level = 0; e->rx_head = 0; e->stream_pos = e->pointer;
                rx_refill(e);
                return;
            }
            e->rx_total = 0;
            if (n == 4) {const uint32_t v = e->wide[e->wide_pointer % 16];
                e->rx = (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);}
            else e->rx = e->slave[e->pointer++];
            e->rx_count = 1;
            e->status |= GENI_M_CMD_DONE; e->active = 0;
        }
        return;
    case GENI_TX_WATERMARK:
        e->reg[r / 4] = v;
        if (v && e->active && e->op == GENI_OP_WRITE && !e->mute) e->status |= GENI_M_TX_WATERMARK;
        return;
    case GENI_TX_FIFO:
        e->fifo_writes++;
        CHECK(e->active && e->op == GENI_OP_WRITE);
        for (int i = 0; i < 4 && e->tx_bytes < e->length; ++i) e->tx[e->tx_bytes++ % 1024] = (uint8_t)(v >> (8 * i));
        if (e->length > 8) {
            /* FIFO mode: the words wait in the FIFO, which must never overflow. */
            ++e->tx_level;
            CHECK(e->tx_level <= e->depth);
            if (e->tx_level > e->tx_peak) e->tx_peak = e->tx_level;
            if (e->tx_bytes == e->length) e->pointer = e->tx[0];
            return;
        }
        if (e->tx_bytes < e->length) return;
        e->tx_bytes = 0;
        e->active = 0;
        if (!e->present) {e->status |= (1u << 10) | GENI_M_CMD_DONE; return;}
        if (e->address == 0x40) {
            CHECK(e->length == 4 || e->length == 8);
            e->wide_pointer = (uint32_t)e->tx[0] << 24 | (uint32_t)e->tx[1] << 16 | (uint32_t)e->tx[2] << 8 | e->tx[3];
            if (e->length == 8) e->wide[e->wide_pointer % 16] = (uint32_t)e->tx[4] << 24 | (uint32_t)e->tx[5] << 16 | (uint32_t)e->tx[6] << 8 | e->tx[7];
            e->status |= GENI_M_CMD_DONE;
            return;
        }
        e->pointer = (uint8_t)v;
        if (e->length == 2) e->slave[e->pointer] = (uint8_t)(v >> 8);
        e->status |= GENI_M_CMD_DONE;
        return;
    case GENI_M_CMD_CTRL:
        if (v & GENI_CMD_CTRL_CANCEL && e->cancel_ack) {e->active = 0; e->status |= GENI_M_CMD_CANCEL;}
        if (v & GENI_CMD_CTRL_ABORT && e->abort_ack) {e->active = 0; e->status |= GENI_M_CMD_ABORT;}
        return;
    default:
        CHECK(r < sizeof e->reg && !(r & 3));
        e->reg[r / 4] = v;
    }
}

static void boot_state(Engine *e)
{
    memset(e, 0, sizeof *e);
    e->reg[GENI_FW_REVISION / 4] = GENI_PROTO_I2C << 8 | 0x47;
    e->reg[GENI_CLK_CFG / 4] = 0x00000041;   /* divider 4, enabled */
    e->reg[GENI_SCL_COUNTERS / 4] = 0x00b31e2a;
    e->reg[GENI_DMA_MODE / 4] = 1;
    e->reg[GENI_HW_PARAM_0 / 4] = 16u << 16; e->reg[GENI_HW_PARAM_1 / 4] = 16u << 16;
    e->depth = 16;
    e->present = 1; e->cancel_ack = 1; e->abort_ack = 1;
}
/* Polls until the transfer ends; returns its result and the number of polls in *n. */
static int run(GeniI2c *g, unsigned *n)
{
    for (unsigned i = 1; i <= 8; ++i) {
        const int r = geni_i2c_poll(g);
        if (r != GeniPending) {if (n) *n = i; return r;}
    }
    return GeniPending;
}

int main(void)
{
    Engine e;
    GeniI2c g;
    const GeniIo io = {eng_read, eng_write, &e};
    uint32_t config = 0;
    unsigned n = 0;

    /* Not usable: another protocol, disabled interface, clock off, no SCL timing. */
    boot_state(&e); e.reg[GENI_FW_REVISION / 4] = 2 << 8;
    CHECK(geni_i2c_prepare(&g, &io, 0x11, &config) == -1 && config >> 24 == 2);
    boot_state(&e); e.reg[GENI_IF_DISABLE / 4] = 1;
    CHECK(geni_i2c_prepare(&g, &io, 0x11, &config) == -1);
    boot_state(&e); e.reg[GENI_CLK_CFG / 4] = 0x40;
    CHECK(geni_i2c_prepare(&g, &io, 0x11, &config) == -1);
    boot_state(&e); e.reg[GENI_SCL_COUNTERS / 4] = 0;
    CHECK(geni_i2c_prepare(&g, &io, 0x11, &config) == -1);
    CHECK(e.reg[GENI_DMA_MODE / 4] == 1);   /* nothing was changed */

    boot_state(&e);
    e.reg[GENI_M_IRQ_EN / 4] = 1u << 26; e.status = GENI_M_CMD_DONE;  /* left over from the boot loader */
    CHECK(geni_i2c_prepare(&g, &io, 0x11, &config) == 0 && config == (3u << 24 | 0x41));
    CHECK(e.reg[GENI_DMA_MODE / 4] == 0 && e.reg[GENI_TX_PACK0 / 4] == 0x7f8fe && e.reg[GENI_RX_PACK1 / 4] == 0xffefe && e.status == 0);
    /* Exactly the wanted bits: a stale RX watermark enable would hold the level interrupt up. */
    CHECK(e.reg[GENI_M_IRQ_EN / 4] == GENI_M_IRQ_WANTED && (e.reg[GENI_SE_IRQ_EN / 4] & GENI_SE_M_IRQ_EN));
    CHECK(!geni_i2c_busy(&g) && geni_i2c_poll(&g) == GeniPending);

    /* A register write: command, then the data on the watermark, then done. */
    e.slave[4] = 0xaa;
    CHECK(geni_i2c_start_write(&g, 0x04, 0x05) == 0 && geni_i2c_busy(&g));
    CHECK(geni_i2c_start_write(&g, 0x05, 0x01) == -1 && geni_i2c_start_read(&g, 0x05) == -1);
    CHECK(e.address == 0x11 && e.length == 2 && !e.hold && e.commands == 1);
    CHECK(run(&g, &n) == GeniDone && n == 2 && e.slave[4] == 0x05 && !geni_i2c_busy(&g) && g.done == 1);
    CHECK(e.reg[GENI_TX_WATERMARK / 4] == 0);

    /* The core never touches the engine between events: no polling inside a call. */
    const unsigned before = e.accesses;
    CHECK(geni_i2c_start_write(&g, 0x05, 0xff) == 0 && e.accesses - before <= 4);
    CHECK(run(&g, NULL) == GeniDone && e.slave[5] == 0xff);

    /* A register read: a write holding the stop, then a 1-byte read. */
    e.slave[0x05] = 0x7d;
    CHECK(geni_i2c_start_read(&g, 0x05) == 0 && e.hold && e.length == 1);
    CHECK(run(&g, &n) == GeniDone && n == 3 && g.data == 0x7d && e.rx_count == 0);

    /* Status gathered over a while (the interrupt handler ORs what it cleared) is handled in one go. */
    CHECK(geni_i2c_start_write(&g, 0x06, 0x12) == 0);
    uint32_t gathered = e.status; e.status = 0;
    CHECK(geni_i2c_event(&g, gathered) == GeniPending);
    gathered = e.status; e.status = 0;
    CHECK(geni_i2c_event(&g, gathered) == GeniDone && e.slave[6] == 0x12);
    /* Done before the data went in is a protocol error. */
    CHECK(geni_i2c_start_write(&g, 0x06, 0x13) == 0);
    e.status = 0; e.active = 0;
    CHECK(geni_i2c_event(&g, GENI_M_CMD_DONE) == GeniFailed && g.error == GeniErrState);

    /* No slave: the NACK ends the write and the read with a bus error. */
    e.present = 0;
    CHECK(geni_i2c_start_write(&g, 0x04, 1) == 0 && run(&g, NULL) == GeniFailed && g.error == GeniErrBus && (g.error_status & (1u << 10)));
    CHECK(geni_i2c_start_read(&g, 0x04) == 0 && run(&g, NULL) == GeniFailed && g.error == GeniErrBus);
    e.present = 1;
    /* A read that brings no data. */
    CHECK(geni_i2c_start_read(&g, 0x04) == 0 && geni_i2c_poll(&g) == GeniPending && geni_i2c_poll(&g) == GeniPending);
    CHECK(g.phase == GeniReadDone);
    e.status = 0; e.rx_count = 0;
    CHECK(geni_i2c_event(&g, GENI_M_CMD_DONE) == GeniFailed && g.error == GeniErrNoData);

    /* Events while idle are counted and ignored. */
    const uint32_t stray = g.stray;
    CHECK(geni_i2c_event(&g, GENI_M_CMD_DONE) == GeniPending && g.stray == stray + 1);
    CHECK(geni_i2c_timeout(&g) == GeniPending && !geni_i2c_busy(&g));

    /* A silent engine: the timeout cancels; the cancel is acknowledged. */
    e.mute = 1; e.status = 0;
    CHECK(geni_i2c_start_write(&g, 0x04, 2) == 0 && run(&g, NULL) == GeniPending);
    CHECK(geni_i2c_timeout(&g) == GeniPending && g.phase == GeniCancelling && g.timeouts == 1);
    CHECK(run(&g, NULL) == GeniFailed && g.error == GeniErrTimeout && !geni_i2c_busy(&g));
    /* The cancel is not acknowledged: abort; that is. */
    e.cancel_ack = 0;
    CHECK(geni_i2c_start_read(&g, 0x04) == 0 && geni_i2c_timeout(&g) == GeniPending && run(&g, NULL) == GeniPending);
    CHECK(geni_i2c_timeout(&g) == GeniPending && g.phase == GeniAborting);
    CHECK(run(&g, NULL) == GeniFailed && g.error == GeniErrTimeout);
    /* Neither is acknowledged: the third deadline ends the transfer anyway. */
    e.abort_ack = 0; e.active = 0;
    CHECK(geni_i2c_start_write(&g, 0x04, 3) == 0);
    CHECK(geni_i2c_timeout(&g) == GeniPending && geni_i2c_timeout(&g) == GeniPending && geni_i2c_timeout(&g) == GeniFailed);
    CHECK(!geni_i2c_busy(&g) && g.timeouts == 3);
    /* The engine recovers: the next transfer works. */
    e.mute = 0; e.active = 0; e.status = 0; e.cancel_ack = e.abort_ack = 1;
    CHECK(geni_i2c_start_write(&g, 0x04, 6) == 0 && run(&g, NULL) == GeniDone && e.slave[4] == 6);
    CHECK(geni_i2c_start_read(&g, 0x04) == 0 && run(&g, NULL) == GeniDone && g.data == 6);

    /* Every write and read value over many transfers. */
    for (unsigned v = 0; v < 256; ++v) {
        CHECK(geni_i2c_start_write(&g, (uint8_t)(v ^ 0x5a), (uint8_t)v) == 0 && run(&g, NULL) == GeniDone);
        CHECK(geni_i2c_start_read(&g, (uint8_t)(v ^ 0x5a)) == 0 && run(&g, NULL) == GeniDone && g.data == v);
    }
    /* 32-bit registers: an 8-byte write in two FIFO words, then a 4-byte address write and a 4-byte read. */
    GeniI2c w;
    CHECK(geni_i2c_prepare(&w, &io, 0x40, NULL) == 0);
    const uint8_t put[8] = {0, 0, 0, 0x0c, 0x12, 0x34, 0x56, 0x78}, at[4] = {0, 0, 0, 0x0c};
    CHECK(geni_i2c_start_write_bytes(&w, put, 9) == -1 && geni_i2c_start_read_bytes(&w, at, 4, 5) == -1);
    CHECK(geni_i2c_start_write_bytes(&w, put, 8) == 0 && e.length == 8 && run(&w, NULL) == GeniDone && e.wide[12] == 0x12345678);
    CHECK(geni_i2c_start_read_bytes(&w, at, 4, 4) == 0 && e.hold && e.length == 4 && run(&w, NULL) == GeniDone);
    CHECK(w.rx == 0x78563412u && w.data == 0x12);   /* first byte (the most significant) in bits 7:0 */

    /* Buffered writes: a 67-byte command and a 513-byte block through a 16-word FIFO, never more than the free words per event. */
    GeniI2c b;
    CHECK(geni_i2c_prepare(&b, &io, 0x4c, NULL) == 0 && b.tx_fifo_words == 16 && b.rx_fifo_words == 16);
    static uint8_t big[513], frame[68];
    for (unsigned i = 0; i < sizeof big; ++i) big[i] = (uint8_t)(i * 7 + 3);
    big[0] = 0x5c;
    for (unsigned size = 9; size <= sizeof big; size = size == 9 ? 67 : size == 67 ? 64 : size == 64 ? 65 : size == 65 ? 513 : 1000) {
        e.tx_peak = 0; e.watermarks = 0; memset(e.tx, 0, sizeof e.tx);
        CHECK(geni_i2c_start_write_buffer(&b, big, size) == 0 && e.length == (int)size && !e.hold);
        CHECK(geni_i2c_start_write_buffer(&b, big, size) == -1);
        unsigned polls = 0;
        int r = GeniPending;
        while (r == GeniPending && polls < 400) {r = geni_i2c_poll(&b); ++polls;}
        CHECK(r == GeniDone && memcmp(e.tx, big, size) == 0 && e.tx_peak <= 16 && !geni_i2c_busy(&b));
        CHECK(e.reg[GENI_TX_WATERMARK / 4] == 0 && b.tx_buffer == NULL);
        if (size > 64) CHECK(e.watermarks >= (size - 1) / 64);
    }
    /* A watermark event while the FIFO is still full writes nothing. */
    CHECK(geni_i2c_start_write_buffer(&b, big, 513) == 0 && geni_i2c_poll(&b) == GeniPending && e.tx_level == 16);
    const unsigned writes = e.fifo_writes;
    CHECK(geni_i2c_event(&b, GENI_M_TX_WATERMARK) == GeniPending && e.fifo_writes == writes);
    { int r = GeniPending; for (unsigned i = 0; r == GeniPending && i < 400; ++i) r = geni_i2c_poll(&b);
      CHECK(r == GeniDone && memcmp(e.tx, big, 513) == 0); }
    /* Done before all words went in is a protocol error. */
    CHECK(geni_i2c_start_write_buffer(&b, big, 513) == 0);
    e.status = 0; e.active = 0; e.tx_level = 0;
    CHECK(geni_i2c_event(&b, GENI_M_CMD_DONE) == GeniFailed && b.error == GeniErrState && b.tx_buffer == NULL);
    e.tx_bytes = 0;

    /* Buffered reads: register 0x4c held, then 68 bytes over several RX watermark events; the done event drains the rest. */
    for (unsigned i = 0; i < sizeof e.stream; ++i) e.stream[i] = (uint8_t)(i ^ 0xa5);
    for (unsigned size = 5; size <= 68; size = size == 5 ? 64 : size == 64 ? 67 : size == 67 ? 68 : 100) {
        const uint8_t command = 0x4c;
        memset(frame, 0, sizeof frame);
        CHECK(geni_i2c_start_read_buffer(&b, &command, 1, frame, size) == 0 && e.hold && e.length == 1);
        int r = GeniPending;
        for (unsigned i = 0; r == GeniPending && i < 64; ++i) r = geni_i2c_poll(&b);
        CHECK(r == GeniDone && b.rx_done == size && e.rx_level == 0);
        for (unsigned i = 0; i < size; ++i) CHECK(frame[i] == (uint8_t)((0x4c + i) ^ 0xa5));
        CHECK(e.reg[GENI_M_IRQ_EN / 4] == GENI_M_IRQ_WANTED && b.rx_buffer == NULL);
    }
    CHECK(geni_i2c_start_read_buffer(&b, big, 9, frame, 68) == -1 && geni_i2c_start_read_buffer(&b, big, 1, frame, 0) == -1);
    /* A read that ends short fails and leaves no words for the next transfer. */
    {
        const uint8_t command = 0x4c;
        e.short_read = 40;   /* the engine stops after 40 bytes */
        CHECK(geni_i2c_start_read_buffer(&b, &command, 1, frame, 68) == 0);
        int r = GeniPending;
        for (unsigned i = 0; r == GeniPending && i < 64; ++i) r = geni_i2c_poll(&b);
        CHECK(r == GeniFailed && b.error == GeniErrNoData && e.rx_level == 0 && e.reg[GENI_M_IRQ_EN / 4] == GENI_M_IRQ_WANTED);
    }
    /* The register transfers still work after buffered ones. */
    e.rx_total = 0; e.short_read = 0;
    CHECK(geni_i2c_start_write(&b, 0x5c, 0x04) == 0 && run(&b, NULL) == GeniDone && e.slave[0x5c] == 0x04);
    CHECK(geni_i2c_start_read(&b, 0x5c) == 0 && run(&b, NULL) == GeniDone && b.data == 0x04);
    printf("PASS GENI I2C: %u checks\n", checks);
    return 0;
}
