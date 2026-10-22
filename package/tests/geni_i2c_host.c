// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the event-driven GENI I2C core (package/GeniI2c.c) against a model of the serial engine with one slave register file.
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
    unsigned accesses, commands, fifo_writes;
} Engine;

static uint32_t eng_read(void *ctx, uint32_t r)
{
    Engine *e = ctx;
    e->accesses++;
    if (r == GENI_M_IRQ_STATUS) return e->status;
    if (r == GENI_RX_FIFO_STATUS) return e->rx_count;
    if (r == GENI_RX_FIFO) {e->rx_count = 0; return e->rx;}
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
            CHECK(e->op == GENI_OP_READ && e->reg[GENI_RX_LEN / 4] == 1);
            if (e->mute) return;
            if (!e->present) {e->status |= (1u << 10) | GENI_M_CMD_DONE; e->active = 0; return;}
            e->rx = e->slave[e->pointer++]; e->rx_count = 1;
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
        e->active = 0;
        if (!e->present) {e->status |= (1u << 10) | GENI_M_CMD_DONE; return;}
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
    CHECK(e.reg[GENI_M_IRQ_EN / 4] == ((1u << 26) | GENI_M_IRQ_WANTED) && (e.reg[GENI_SE_IRQ_EN / 4] & GENI_SE_M_IRQ_EN));
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
    printf("PASS GENI I2C: %u checks\n", checks);
    return 0;
}
