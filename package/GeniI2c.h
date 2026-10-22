/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* One register transfer at a time on a Qualcomm GENI serial engine running the I2C firmware, as an event-driven state machine: the owner
 * starts a transfer, then feeds the engine's M_IRQ status (from its interrupt handler or a timer poll) until the transfer is done or
 * failed, and calls the timeout step when its deadline passes. Nothing here waits or spins: every call does a few register accesses and
 * returns. A register write is one 2-byte write {register, value}; a register read is a 1-byte write {register} that holds the stop
 * condition, then a 1-byte read. A transfer that stops answering is cancelled, then aborted, as the engine documents. */
#ifndef GENI_I2C_H
#define GENI_I2C_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Serial engine registers (offsets in the engine's 16 KiB window). */
#define GENI_FW_REVISION 0x68u
#define GENI_IF_DISABLE 0x64u
#define GENI_CLK_CFG 0x48u
#define GENI_BYTE_GRAN 0x254u
#define GENI_DMA_MODE 0x258u
#define GENI_TX_PACK0 0x260u
#define GENI_TX_PACK1 0x264u
#define GENI_TX_LEN 0x26cu
#define GENI_RX_LEN 0x270u
#define GENI_SCL_COUNTERS 0x278u
#define GENI_RX_PACK0 0x284u
#define GENI_RX_PACK1 0x288u
#define GENI_M_CMD0 0x600u
#define GENI_M_CMD_CTRL 0x604u
#define GENI_M_IRQ_STATUS 0x610u
#define GENI_M_IRQ_EN 0x614u
#define GENI_M_IRQ_CLEAR 0x618u
#define GENI_TX_FIFO 0x700u
#define GENI_RX_FIFO 0x780u
#define GENI_RX_FIFO_STATUS 0x804u
#define GENI_TX_WATERMARK 0x80cu
#define GENI_SE_IRQ_EN 0xe1cu

/* M_IRQ bits. */
#define GENI_M_CMD_DONE (1u << 0)
#define GENI_M_CMD_OVERRUN (1u << 1)
#define GENI_M_ILLEGAL_CMD (1u << 2)
#define GENI_M_CMD_FAILURE (1u << 3)
#define GENI_M_CMD_CANCEL (1u << 4)
#define GENI_M_CMD_ABORT (1u << 5)
#define GENI_M_GP_IRQ (0x3fu << 9)      /* I2C: NACK, bus error, arbitration lost and the like */
#define GENI_M_TX_WATERMARK (1u << 30)
#define GENI_M_ERRORS (GENI_M_CMD_OVERRUN | GENI_M_ILLEGAL_CMD | GENI_M_CMD_FAILURE | GENI_M_GP_IRQ)
/* What the engine is set to raise its line for. */
#define GENI_M_IRQ_WANTED (GENI_M_CMD_DONE | GENI_M_ERRORS | GENI_M_CMD_CANCEL | GENI_M_CMD_ABORT | GENI_M_TX_WATERMARK)
#define GENI_SE_M_IRQ_EN (1u << 2)
#define GENI_CMD_CTRL_ABORT (1u << 1)
#define GENI_CMD_CTRL_CANCEL (1u << 2)
#define GENI_PROTO_I2C 3u
#define GENI_OP_WRITE 1u
#define GENI_OP_READ 2u
#define GENI_STOP_STRETCH (1u << 2)

typedef struct {
    uint32_t (*read)(void *ctx, uint32_t reg);
    void (*write)(void *ctx, uint32_t reg, uint32_t value);
    void *ctx;
} GeniIo;

enum {GeniIdle, GeniTxWait, GeniWriteDone, GeniReadDone, GeniCancelling, GeniAborting};
enum {GeniPending = 0, GeniDone = 1, GeniFailed = 2};
/* Why the last transfer failed. */
enum {GeniOk = 0, GeniErrBusy = 1, GeniErrBus = 2, GeniErrTimeout = 3, GeniErrNoData = 4, GeniErrState = 5};

typedef struct {
    GeniIo io;
    uint8_t address, phase, reading, hold;
    uint8_t reg, value, error, data;
    uint32_t word, length;
    uint32_t error_status;           /* the M_IRQ status of the last failure */
    uint32_t done, failed, timeouts, stray;
} GeniI2c;

/* Checks the engine runs the I2C firmware with its clock and timing set (the boot loader's), selects FIFO mode with 8-bit packing and
 * enables the interrupts this core needs. Returns 0, or -1 when the engine is not usable. *config: proto << 24 | M_CLK_CFG bits 23:0. */
int geni_i2c_prepare(GeniI2c *g, const GeniIo *io, uint8_t address, uint32_t *config);
int geni_i2c_start_write(GeniI2c *g, uint8_t reg, uint8_t value);
int geni_i2c_start_read(GeniI2c *g, uint8_t reg);
/* Feeds M_IRQ status bits (already cleared by whoever read them). Returns GeniPending, GeniDone (read value in g->data) or GeniFailed. */
int geni_i2c_event(GeniI2c *g, uint32_t status);
/* Reads and clears the status itself, then as geni_i2c_event (for a timer poll; the interrupt handler does the same in its context). */
int geni_i2c_poll(GeniI2c *g);
/* The owner's deadline for the current step passed: a pending transfer is cancelled, a cancel that is not acknowledged is aborted and an
 * abort that is not acknowledged ends the transfer anyway. Returns as geni_i2c_event. */
int geni_i2c_timeout(GeniI2c *g);
static inline int geni_i2c_busy(const GeniI2c *g) {return g->phase != GeniIdle;}

#ifdef __cplusplus
}
#endif
#endif
