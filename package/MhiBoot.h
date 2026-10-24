/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Booting a Qualcomm MHI device's firmware from its primary boot loader, as a state machine the owner drives from the device's
 * interrupt or a timer: prepare (interrupt vector, a reset when the device sits in an error state), the secondary boot loader through
 * the boot host interface (BHI, one image in one DMA buffer), the wait for the device's READY, a hand back to the owner to program the
 * MHI contexts before M0 is requested, then the full image through the vector interface (BHIe, a table of DMA segments) once the device
 * runs the secondary boot loader, and the wait for mission mode. No call waits: each reads or writes a few registers and returns the
 * phase; the owner keeps the deadlines and calls mhi_boot_timeout when one passes. */
#ifndef MHI_BOOT_H
#define MHI_BOOT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* MHI registers (offsets in the MHI register space) and the boot interfaces' (from BHIOFF and BHIEOFF). */
#define MHI_REGLEN 0x00u
#define MHI_BHIOFF 0x28u
#define MHI_BHIEOFF 0x2cu
#define MHI_CTRL 0x38u
#define MHI_STATUS 0x48u
#define MHI_CTRL_RESET (1u << 1)
#define MHI_STATUS_READY (1u << 0)
#define MHI_STATUS_SYSERR (1u << 2)
#define MHI_STATE_SHIFT 8u                   /* state field 15:8 in both MHI_CTRL and MHI_STATUS */
#define MHI_BHI_IMGADDR_LOW 0x08u
#define MHI_BHI_IMGADDR_HIGH 0x0cu
#define MHI_BHI_IMGSIZE 0x10u
#define MHI_BHI_IMGTXDB 0x18u
#define MHI_BHI_INTVEC 0x20u
#define MHI_BHI_EXECENV 0x28u
#define MHI_BHI_STATUS 0x2cu
#define MHI_BHI_ERRCODE 0x30u
#define MHI_BHIE_TXVECADDR_LOW 0x2cu
#define MHI_BHIE_TXVECADDR_HIGH 0x30u
#define MHI_BHIE_TXVECSIZE 0x34u
#define MHI_BHIE_TXVECDB 0x3cu
#define MHI_BHIE_TXVECSTATUS 0x44u
#define MHI_SEQ_MASK 0x3fffffffu             /* doorbell and status sequence 29:0, status 31:30 */
#define MHI_XFER_SUCCESS 2u
#define MHI_XFER_ERROR 3u

enum {MhiStateReset = 0, MhiStateReady = 1, MhiStateM0 = 2, MhiStateBhi = 7, MhiStateSysErr = 0xff};
enum {MhiEePbl = 0, MhiEeSbl = 1, MhiEeAmss = 2, MhiEeRddm = 3, MhiEeWfw = 4, MhiEePthru = 5, MhiEeEdl = 6, MhiEeFp = 7};

enum {
    MhiIdle, MhiResetting, MhiSblLoading, MhiWaitReady, MhiNeedContexts, MhiWaitSbl, MhiImageLoading, MhiWaitMission, MhiDone, MhiFailed
};
enum {MhiErrNone, MhiErrLayout, MhiErrNotPbl, MhiErrBhi, MhiErrBhie, MhiErrSysErr, MhiErrTimeout, MhiErrState, MhiErrRddm};

typedef struct {
    uint32_t (*read)(void *ctx, uint32_t reg);
    void (*write)(void *ctx, uint32_t reg, uint32_t value);
    void *ctx;
    uint32_t space;   /* bytes of register space the owner mapped (0: use the MHIREGLEN register, which only counts the MHI block itself) */
} MhiIo;

typedef struct {
    MhiIo io;
    uint32_t bhi, bhie, reglen;
    uint32_t phase, error, failed_phase, ee, state;
    uint32_t seq, errcode, last_status;
    uint64_t sbl_addr, table_addr;
    uint32_t sbl_size, table_size, msi_vector;
} MhiBoot;

/* One BHIe vector table entry: a DMA segment's address and size, as the device reads them (little endian). */
typedef struct {uint64_t address, size;} MhiBhieEntry;

/* Reads the register layout. Returns 0, or -1 when the boot interfaces are not inside the register space. */
int mhi_boot_open(MhiBoot *m, const MhiIo *io);
uint32_t mhi_boot_ee(MhiBoot *m);
uint32_t mhi_boot_state(MhiBoot *m);
/* Starts the boot: sbl_* the DMA buffer holding the secondary boot loader (the image's first part), table_* the BHIe vector table
 * describing the whole image (size: bytes of the table). seq: a nonzero value the doorbells carry (the owner's random number). */
uint32_t mhi_boot_begin(MhiBoot *m, uint32_t msi_vector, uint64_t sbl_addr, uint32_t sbl_size, uint64_t table_addr, uint32_t table_size,
                        uint32_t seq);
/* Re-reads what the current phase waits for and moves on; returns the phase. */
uint32_t mhi_boot_step(MhiBoot *m);
/* The owner programmed the MHI contexts (phase MhiNeedContexts): requests M0 and waits for the secondary boot loader. */
uint32_t mhi_boot_contexts_done(MhiBoot *m);
/* The owner's deadline for the current phase passed. */
uint32_t mhi_boot_timeout(MhiBoot *m);
/* Fills a BHIe table for an image of image_size bytes cut in segments of segment_size placed at addresses[]; returns the number of
 * entries, or 0 when count is too small. */
uint32_t mhi_bhie_table(MhiBhieEntry *table, uint32_t count, const uint64_t *addresses, uint32_t image_size, uint32_t segment_size);

#ifdef __cplusplus
}
#endif
#endif
