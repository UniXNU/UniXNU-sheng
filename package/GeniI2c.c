/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "GeniI2c.h"

static uint32_t rd(GeniI2c *g, uint32_t reg) {return g->io.read(g->io.ctx, reg);}
static void wr(GeniI2c *g, uint32_t reg, uint32_t value) {g->io.write(g->io.ctx, reg, value);}

/* A FIFO depth field (bits 21:16); a field that reads zero takes the smallest depth these engines have. */
static uint32_t fifo_words(uint32_t param)
{
    const uint32_t words = (param >> 16) & 0x3fu;
    return words ? words : 16u;
}

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
    g->tx_fifo_words = fifo_words(rd(g, GENI_HW_PARAM_0));
    g->rx_fifo_words = fifo_words(rd(g, GENI_HW_PARAM_1));
    wr(g, GENI_M_IRQ_CLEAR, 0xffffffffu);
    /* Exactly the bits this core handles: one the boot loader left enabled (RX watermark, RX last) would hold the level line up. */
    wr(g, GENI_M_IRQ_EN, GENI_M_IRQ_WANTED);
    wr(g, GENI_SE_IRQ_EN, rd(g, GENI_SE_IRQ_EN) | GENI_SE_M_IRQ_EN);
    return 0;
}

/* Starts one write command of length bytes (at most 8); the data goes in when the engine asks for it (TX watermark). */
static void write_command(GeniI2c *g, const uint8_t *bytes, uint32_t length, int hold)
{
    g->tx[0] = g->tx[1] = 0;
    for (uint32_t i = 0; i < length; ++i) g->tx[i / 4] |= (uint32_t)bytes[i] << (8 * (i % 4));
    g->tx_words = (uint8_t)((length + 3) / 4);
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
    const uint8_t bytes[2] = {reg, value};
    write_command(g, bytes, 2, 0);
    return 0;
}

int geni_i2c_start_read(GeniI2c *g, uint8_t reg)
{
    if (g->phase != GeniIdle) return -1;
    g->reading = 1; g->rx_length = 1; g->reg = reg; g->error = GeniOk;
    write_command(g, &reg, 1, 1);
    return 0;
}

int geni_i2c_start_write_bytes(GeniI2c *g, const uint8_t *bytes, uint32_t length)
{
    if (g->phase != GeniIdle || !length || length > 8) return -1;
    g->reading = 0; g->error = GeniOk;
    write_command(g, bytes, length, 0);
    return 0;
}

int geni_i2c_start_read_bytes(GeniI2c *g, const uint8_t *bytes, uint32_t length, uint32_t read_length)
{
    if (g->phase != GeniIdle || !length || length > 4 || !read_length || read_length > 4) return -1;
    g->reading = 1; g->rx_length = (uint8_t)read_length; g->error = GeniOk;
    write_command(g, bytes, length, 1);
    return 0;
}

int geni_i2c_start_write_buffer(GeniI2c *g, const uint8_t *bytes, uint32_t length)
{
    if (g->phase != GeniIdle || !bytes || !length) return -1;
    g->reading = 0; g->error = GeniOk;
    g->tx_buffer = bytes; g->tx_done = 0;
    g->tx_words = 0;
    g->length = length;
    g->hold = 0;
    wr(g, GENI_TX_LEN, length);
    wr(g, GENI_M_CMD0, (GENI_OP_WRITE << 27) | ((uint32_t)g->address << 9));
    wr(g, GENI_TX_WATERMARK, 1);
    g->phase = GeniTxWait;
    return 0;
}

int geni_i2c_start_read_buffer(GeniI2c *g, const uint8_t *command, uint32_t command_length, uint8_t *buffer, uint32_t length)
{
    if (g->phase != GeniIdle || !command || !command_length || command_length > 8 || !buffer || !length) return -1;
    g->reading = 1; g->error = GeniOk;
    g->rx_buffer = buffer; g->rx_done = 0; g->rx_total = length;
    write_command(g, command, command_length, 1);
    return 0;
}

/* Fills the free TX FIFO words (a watermark event that finds the FIFO full writes nothing); the last word in ends the TX wait. */
static void feed(GeniI2c *g)
{
    const uint32_t used = GENI_TX_FIFO_WORDS(rd(g, GENI_TX_FIFO_STATUS));
    uint32_t room = used < g->tx_fifo_words ? g->tx_fifo_words - used : 0;
    while (room && g->tx_done < g->length) {
        uint32_t word = 0;
        for (unsigned k = 0; k < 4 && g->tx_done < g->length; ++k) word |= (uint32_t)g->tx_buffer[g->tx_done++] << (8 * k);
        wr(g, GENI_TX_FIFO, word);
        --room;
    }
    if (g->tx_done == g->length) {
        wr(g, GENI_TX_WATERMARK, 0);
        g->phase = GeniWriteDone;
    }
}

/* Empties the RX FIFO into the read buffer; words past the requested length are dropped. */
static void drain(GeniI2c *g)
{
    for (uint32_t words = GENI_RX_FIFO_WORDS(rd(g, GENI_RX_FIFO_STATUS)); words; --words) {
        const uint32_t word = rd(g, GENI_RX_FIFO);
        for (unsigned k = 0; k < 4 && g->rx_buffer && g->rx_done < g->rx_total; ++k) g->rx_buffer[g->rx_done++] = (uint8_t)(word >> (8 * k));
    }
}

static int finish(GeniI2c *g, int error, uint32_t status)
{
    g->phase = GeniIdle;
    wr(g, GENI_TX_WATERMARK, 0);
    if (g->rx_buffer) {
        /* A failed or cancelled read can leave words behind; the next transfer must not take them. */
        if (error) {uint8_t *keep = g->rx_buffer; g->rx_buffer = 0; drain(g); g->rx_buffer = keep;}
        wr(g, GENI_M_IRQ_EN, GENI_M_IRQ_WANTED);
    }
    g->tx_buffer = 0; g->rx_buffer = 0;
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
        if ((status & GENI_M_TX_WATERMARK) && g->tx_buffer) feed(g);
        else if (status & GENI_M_TX_WATERMARK) {
            for (unsigned i = 0; i < g->tx_words; ++i) wr(g, GENI_TX_FIFO, g->tx[i]);
            wr(g, GENI_TX_WATERMARK, 0);
            g->phase = GeniWriteDone;
        }
        if (!(status & GENI_M_CMD_DONE)) return GeniPending;
        if (g->phase == GeniTxWait) return finish(g, GeniErrState, status);  /* done before the data went in */
    }
    if (g->phase == GeniReadDone && g->rx_buffer) {
        drain(g);
        if (!(status & GENI_M_CMD_DONE)) return GeniPending;
        drain(g);   /* the last words can land after the status was read */
        return g->rx_done == g->rx_total ? finish(g, GeniOk, 0) : finish(g, GeniErrNoData, status);
    }
    if (!(status & GENI_M_CMD_DONE)) return GeniPending;
    if (g->phase == GeniWriteDone) {
        if (!g->reading) return finish(g, GeniOk, 0);
        if (g->rx_buffer) {
            const uint32_t mark = g->rx_fifo_words > 1 ? g->rx_fifo_words - 1 : 1;
            wr(g, GENI_RX_WATERMARK, mark);
            wr(g, GENI_RX_RFR_WATERMARK, g->rx_fifo_words);
            wr(g, GENI_M_IRQ_EN, GENI_M_IRQ_WANTED | GENI_M_RX_WATERMARK | GENI_M_RX_LAST);
            wr(g, GENI_RX_LEN, g->rx_total);
        } else wr(g, GENI_RX_LEN, g->rx_length);
        wr(g, GENI_M_CMD0, (GENI_OP_READ << 27) | ((uint32_t)g->address << 9));
        g->phase = GeniReadDone;
        return GeniPending;
    }
    /* GeniReadDone */
    if (!(rd(g, GENI_RX_FIFO_STATUS) & 0x1ffffffu)) return finish(g, GeniErrNoData, status);
    g->rx = rd(g, GENI_RX_FIFO);
    g->data = (uint8_t)g->rx;
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
