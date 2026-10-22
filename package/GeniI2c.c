/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "GeniI2c.h"

static uint32_t rd(GeniI2c *g, uint32_t reg) {return g->io.read(g->io.ctx, reg);}
static void wr(GeniI2c *g, uint32_t reg, uint32_t value) {g->io.write(g->io.ctx, reg, value);}

int geni_i2c_prepare(GeniI2c *g, const GeniIo *io, uint8_t address, uint32_t *config)
{
    for (unsigned i = 0; i < sizeof *g; ++i) ((uint8_t *)g)[i] = 0;
    g->io = *io;
    g->address = address;
    const uint32_t proto = (rd(g, GENI_FW_REVISION) >> 8) & 0xffu, clock = rd(g, GENI_CLK_CFG);
    if (config) *config = (proto << 24) | (clock & 0xffffffu);
    if (proto != GENI_PROTO_I2C || (rd(g, GENI_IF_DISABLE) & 1u) || !(clock & 1u) || !rd(g, GENI_SCL_COUNTERS)) return -1;
    wr(g, GENI_DMA_MODE, 0);
    /* 8-bit words, 4 per FIFO word, least significant byte first on the wire. */
    wr(g, GENI_TX_PACK0, 0x7f8fe); wr(g, GENI_TX_PACK1, 0xffefe);
    wr(g, GENI_RX_PACK0, 0x7f8fe); wr(g, GENI_RX_PACK1, 0xffefe);
    wr(g, GENI_BYTE_GRAN, 0);
    wr(g, GENI_TX_WATERMARK, 0);
    wr(g, GENI_M_IRQ_CLEAR, 0xffffffffu);
    /* Exactly the bits this core handles: one the boot loader left enabled (RX watermark, RX last) would hold the level line up. */
    wr(g, GENI_M_IRQ_EN, GENI_M_IRQ_WANTED);
    wr(g, GENI_SE_IRQ_EN, rd(g, GENI_SE_IRQ_EN) | GENI_SE_M_IRQ_EN);
    return 0;
}

/* Starts one write command of length bytes; the data goes in when the engine asks for it (TX watermark). */
static void write_command(GeniI2c *g, uint32_t word, uint32_t length, int hold)
{
    g->word = word;
    g->length = length;
    g->hold = (uint8_t)hold;
    wr(g, GENI_TX_LEN, length);
    wr(g, GENI_M_CMD0, (GENI_OP_WRITE << 27) | ((uint32_t)g->address << 9) | (hold ? GENI_STOP_STRETCH : 0));
    wr(g, GENI_TX_WATERMARK, 1);
    g->phase = GeniTxWait;
}

int geni_i2c_start_write(GeniI2c *g, uint8_t reg, uint8_t value)
{
    if (g->phase != GeniIdle) return -1;
    g->reading = 0; g->reg = reg; g->value = value; g->error = GeniOk;
    write_command(g, (uint32_t)reg | (uint32_t)value << 8, 2, 0);
    return 0;
}

int geni_i2c_start_read(GeniI2c *g, uint8_t reg)
{
    if (g->phase != GeniIdle) return -1;
    g->reading = 1; g->reg = reg; g->error = GeniOk;
    write_command(g, reg, 1, 1);
    return 0;
}

static int finish(GeniI2c *g, int error, uint32_t status)
{
    g->phase = GeniIdle;
    wr(g, GENI_TX_WATERMARK, 0);
    if (!error) {g->done++; return GeniDone;}
    g->error = (uint8_t)error;
    g->error_status = status;
    g->failed++;
    return GeniFailed;
}

int geni_i2c_event(GeniI2c *g, uint32_t status)
{
    if (!status) return GeniPending;
    switch (g->phase) {
    case GeniIdle:
        g->stray++;
        return GeniPending;
    case GeniCancelling:
        if (status & GENI_M_CMD_CANCEL) return finish(g, GeniErrTimeout, g->error_status);
        return GeniPending;
    case GeniAborting:
        if (status & GENI_M_CMD_ABORT) return finish(g, GeniErrTimeout, g->error_status);
        return GeniPending;
    default:
        break;
    }
    /* A failed command also ends with CMD_DONE; the error bits decide. */
    if (status & GENI_M_ERRORS) return finish(g, GeniErrBus, status);
    if (g->phase == GeniTxWait) {
        if (status & GENI_M_TX_WATERMARK) {
            wr(g, GENI_TX_FIFO, g->word);
            wr(g, GENI_TX_WATERMARK, 0);
            g->phase = GeniWriteDone;
        }
        if (!(status & GENI_M_CMD_DONE)) return GeniPending;
        if (g->phase == GeniTxWait) return finish(g, GeniErrState, status);  /* done before the data went in */
    }
    if (!(status & GENI_M_CMD_DONE)) return GeniPending;
    if (g->phase == GeniWriteDone) {
        if (!g->reading) return finish(g, GeniOk, 0);
        wr(g, GENI_RX_LEN, 1);
        wr(g, GENI_M_CMD0, (GENI_OP_READ << 27) | ((uint32_t)g->address << 9));
        g->phase = GeniReadDone;
        return GeniPending;
    }
    /* GeniReadDone */
    if (!(rd(g, GENI_RX_FIFO_STATUS) & 0x1ffffffu)) return finish(g, GeniErrNoData, status);
    g->data = (uint8_t)rd(g, GENI_RX_FIFO);
    return finish(g, GeniOk, 0);
}

int geni_i2c_poll(GeniI2c *g)
{
    const uint32_t status = rd(g, GENI_M_IRQ_STATUS);
    if (status) wr(g, GENI_M_IRQ_CLEAR, status);
    return g->phase == GeniIdle ? GeniPending : geni_i2c_event(g, status);
}

int geni_i2c_timeout(GeniI2c *g)
{
    switch (g->phase) {
    case GeniIdle:
        return GeniPending;
    case GeniCancelling:
        wr(g, GENI_M_CMD_CTRL, GENI_CMD_CTRL_ABORT);
        g->phase = GeniAborting;
        return GeniPending;
    case GeniAborting:
        return finish(g, GeniErrTimeout, g->error_status);
    default:
        g->timeouts++;
        g->error_status = rd(g, GENI_M_IRQ_STATUS);
        wr(g, GENI_TX_WATERMARK, 0);
        wr(g, GENI_M_CMD_CTRL, GENI_CMD_CTRL_CANCEL);
        g->phase = GeniCancelling;
        return GeniPending;
    }
}
