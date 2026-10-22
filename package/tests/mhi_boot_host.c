// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the MHI firmware boot state machine (package/MhiBoot.c) against a model of a device in its primary boot loader.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../MhiBoot.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

enum {BHI = 0x200, BHIE = 0x300};
typedef struct {
    uint32_t reg[0x1000 / 4];
    uint32_t ee, state, syserr, ready, reset;
    int countdown;                 /* ticks until the pending device action completes */
    int action;                    /* 1 reset, 2 SBL transfer, 3 ready, 4 SBL running, 5 image transfer, 6 mission */
    int bhi_fails, bhie_fails, wrong_seq, crash_after_m0, rddm;
    uint64_t sbl_addr, table_addr;
    uint32_t sbl_size, table_size, seq_bhi, seq_bhie, writes_ctrl;
} Dev;

static uint32_t dev_read(void *ctx, uint32_t r)
{
    Dev *d = ctx;
    CHECK(r < sizeof d->reg && !(r & 3));
    if (r == MHI_STATUS) return d->state << 8 | (d->syserr ? MHI_STATUS_SYSERR : 0) | (d->ready ? MHI_STATUS_READY : 0);
    if (r == MHI_CTRL) return d->reset ? MHI_CTRL_RESET : 0;
    if (r == BHI + MHI_BHI_EXECENV) return d->ee;
    return d->reg[r / 4];
}
static void dev_write(void *ctx, uint32_t r, uint32_t v)
{
    Dev *d = ctx;
    CHECK(r < sizeof d->reg && !(r & 3));
    d->reg[r / 4] = v;
    if (r == MHI_CTRL) {
        d->writes_ctrl++;
        if (v & MHI_CTRL_RESET) {d->reset = 1; d->action = 1; d->countdown = 3; return;}
        if (((v >> 8) & 0xff) == MhiStateM0) {
            CHECK(d->ready);
            d->state = MhiStateM0;
            if (d->crash_after_m0) {d->syserr = 1; d->state = MhiStateSysErr; return;}
            d->action = 4; d->countdown = 2;
        }
    } else if (r == BHI + MHI_BHI_IMGTXDB) {
        CHECK((d->ee == MhiEePbl || d->ee == MhiEeEdl) && !d->reset);
        d->sbl_addr = (uint64_t)d->reg[(BHI + MHI_BHI_IMGADDR_HIGH) / 4] << 32 | d->reg[(BHI + MHI_BHI_IMGADDR_LOW) / 4];
        d->sbl_size = d->reg[(BHI + MHI_BHI_IMGSIZE) / 4];
        d->seq_bhi = v;
        d->action = 2; d->countdown = 2;
    } else if (r == BHIE + MHI_BHIE_TXVECDB) {
        CHECK(d->ee == MhiEeSbl);
        d->table_addr = (uint64_t)d->reg[(BHIE + MHI_BHIE_TXVECADDR_HIGH) / 4] << 32 | d->reg[(BHIE + MHI_BHIE_TXVECADDR_LOW) / 4];
        d->table_size = d->reg[(BHIE + MHI_BHIE_TXVECSIZE) / 4];
        d->seq_bhie = v & MHI_SEQ_MASK;
        d->reg[(BHIE + MHI_BHIE_TXVECSTATUS) / 4] = d->wrong_seq ? (d->seq_bhie + 1) | 2u << 30 : 0;
        d->action = 5; d->countdown = 3;
    }
}
/* Time passes on the device between the owner's steps. */
static void dev_tick(Dev *d)
{
    if (!d->action || --d->countdown > 0) return;
    const int action = d->action;
    d->action = 0;
    switch (action) {
    case 1: d->reset = 0; d->syserr = 0; d->state = MhiStateReset; d->reg[(BHI + MHI_BHI_INTVEC) / 4] = 0; break;
    case 2:
        if (d->bhi_fails) {d->reg[(BHI + MHI_BHI_STATUS) / 4] = 3u << 30; d->reg[(BHI + MHI_BHI_ERRCODE) / 4] = 0x1d; break;}
        d->reg[(BHI + MHI_BHI_STATUS) / 4] = 2u << 30;
        d->action = 3; d->countdown = 2;
        break;
    case 3: d->ready = 1; d->state = MhiStateReady; break;
    case 4: d->ee = MhiEeSbl; break;
    case 5:
        if (d->rddm) {d->ee = MhiEeRddm; break;}
        if (d->wrong_seq) break;
        d->reg[(BHIE + MHI_BHIE_TXVECSTATUS) / 4] = d->seq_bhie | (d->bhie_fails ? 3u : 2u) << 30;
        if (!d->bhie_fails) {d->action = 6; d->countdown = 2;}
        break;
    case 6: d->ee = MhiEeAmss; break;
    }
}
static void dev_init(Dev *d)
{
    memset(d, 0, sizeof *d);
    d->reg[MHI_REGLEN / 4] = 0x1000; d->reg[MHI_BHIOFF / 4] = BHI; d->reg[MHI_BHIEOFF / 4] = BHIE;
    d->ee = MhiEePbl; d->state = MhiStateReset;
}
/* Ticks and steps until the phase leaves `phase` (at most n times); returns the new phase. */
static uint32_t run_while(Dev *d, MhiBoot *m, uint32_t phase, unsigned n)
{
    uint32_t p = m->phase;
    for (unsigned i = 0; i < n && p == phase; ++i) {dev_tick(d); p = mhi_boot_step(m);}
    return p;
}

int main(void)
{
    Dev d;
    MhiBoot m;
    const MhiIo io = {dev_read, dev_write, &d};

    /* A layout outside the register space is refused. */
    dev_init(&d); d.reg[MHI_BHIEOFF / 4] = 0xff0;
    CHECK(mhi_boot_open(&m, &io) == -1 && m.error == MhiErrLayout && m.phase == MhiFailed);
    dev_init(&d); d.reg[MHI_REGLEN / 4] = 0;
    CHECK(mhi_boot_open(&m, &io) == -1);
    dev_init(&d); d.reg[MHI_BHIOFF / 4] = 0x202;
    CHECK(mhi_boot_open(&m, &io) == -1);

    /* The whole boot. */
    dev_init(&d);
    CHECK(mhi_boot_open(&m, &io) == 0 && m.bhi == BHI && m.bhie == BHIE && mhi_boot_ee(&m) == MhiEePbl && mhi_boot_state(&m) == MhiStateReset);
    CHECK(mhi_boot_step(&m) == MhiIdle && mhi_boot_contexts_done(&m) == MhiFailed && m.error == MhiErrState);
    mhi_boot_open(&m, &io);
    CHECK(mhi_boot_begin(&m, 3, 0x1234500000ull, 0x80000, 0x8800000ull, 64, 0x4bcdef12) == MhiSblLoading);
    CHECK(d.sbl_addr == 0x1234500000ull && d.sbl_size == 0x80000 && d.seq_bhi == (0x4bcdef12u & MHI_SEQ_MASK));
    CHECK(d.reg[(BHI + MHI_BHI_INTVEC) / 4] == 3);
    CHECK(run_while(&d, &m, MhiSblLoading, 10) == MhiWaitReady);
    CHECK(run_while(&d, &m, MhiWaitReady, 10) == MhiNeedContexts);
    CHECK(mhi_boot_step(&m) == MhiNeedContexts);   /* waits for the owner */
    CHECK(mhi_boot_contexts_done(&m) == MhiWaitSbl && d.state == MhiStateM0);
    CHECK(run_while(&d, &m, MhiWaitSbl, 10) == MhiImageLoading);
    CHECK(d.table_addr == 0x8800000ull && d.table_size == 64 && d.seq_bhie == m.seq && m.seq != d.seq_bhi);
    CHECK(run_while(&d, &m, MhiImageLoading, 10) == MhiWaitMission);
    CHECK(run_while(&d, &m, MhiWaitMission, 10) == MhiDone && m.ee == MhiEeAmss);
    CHECK(mhi_boot_step(&m) == MhiDone && mhi_boot_timeout(&m) == MhiDone);

    /* A device left in an error state is reset first; the interrupt vector is written again after the reset cleared it. */
    dev_init(&d); d.syserr = 1; d.state = MhiStateSysErr;
    mhi_boot_open(&m, &io);
    CHECK(mhi_boot_begin(&m, 5, 0x1000, 0x80000, 0x2000, 32, 0) == MhiResetting && d.reset && m.seq == 1);
    CHECK(run_while(&d, &m, MhiResetting, 10) == MhiSblLoading && d.reg[(BHI + MHI_BHI_INTVEC) / 4] == 5 && d.seq_bhi == 1);

    /* Not in the primary boot loader. */
    dev_init(&d); d.ee = MhiEeAmss;
    mhi_boot_open(&m, &io);
    CHECK(mhi_boot_begin(&m, 0, 1, 1, 1, 1, 1) == MhiFailed && m.error == MhiErrNotPbl);
    /* The EDL loader accepts an image too. */
    dev_init(&d); d.ee = MhiEeEdl;
    mhi_boot_open(&m, &io);
    CHECK(mhi_boot_begin(&m, 0, 1, 1, 1, 1, 1) == MhiSblLoading);
    CHECK(mhi_boot_begin(&m, 0, 1, 1, 1, 1, 1) == MhiFailed && m.error == MhiErrState);

    /* The BHI transfer fails: the device's error code is kept. */
    dev_init(&d); d.bhi_fails = 1;
    mhi_boot_open(&m, &io);
    mhi_boot_begin(&m, 0, 0x1000, 0x80000, 0x2000, 32, 9);
    CHECK(run_while(&d, &m, MhiSblLoading, 10) == MhiFailed && m.error == MhiErrBhi && m.errcode == 0x1d && m.failed_phase == MhiSblLoading);

    /* A BHIe status for another sequence is not this transfer's: the owner's deadline ends it. */
    dev_init(&d); d.wrong_seq = 1;
    mhi_boot_open(&m, &io);
    mhi_boot_begin(&m, 0, 0x1000, 0x80000, 0x2000, 32, 9);
    run_while(&d, &m, MhiSblLoading, 10); run_while(&d, &m, MhiWaitReady, 10); mhi_boot_contexts_done(&m);
    CHECK(run_while(&d, &m, MhiWaitSbl, 10) == MhiImageLoading && run_while(&d, &m, MhiImageLoading, 10) == MhiImageLoading);
    CHECK(mhi_boot_timeout(&m) == MhiFailed && m.error == MhiErrTimeout && m.failed_phase == MhiImageLoading);

    /* The BHIe transfer fails. */
    dev_init(&d); d.bhie_fails = 1;
    mhi_boot_open(&m, &io);
    mhi_boot_begin(&m, 0, 0x1000, 0x80000, 0x2000, 32, 9);
    run_while(&d, &m, MhiSblLoading, 10); run_while(&d, &m, MhiWaitReady, 10); mhi_boot_contexts_done(&m);
    run_while(&d, &m, MhiWaitSbl, 10);
    CHECK(run_while(&d, &m, MhiImageLoading, 10) == MhiFailed && m.error == MhiErrBhie);

    /* The device crashes into its dump mode during the image transfer. */
    dev_init(&d); d.rddm = 1;
    mhi_boot_open(&m, &io);
    mhi_boot_begin(&m, 0, 0x1000, 0x80000, 0x2000, 32, 9);
    run_while(&d, &m, MhiSblLoading, 10); run_while(&d, &m, MhiWaitReady, 10); mhi_boot_contexts_done(&m);
    run_while(&d, &m, MhiWaitSbl, 10);
    CHECK(run_while(&d, &m, MhiImageLoading, 10) == MhiFailed && m.error == MhiErrRddm);

    /* A system error after M0. */
    dev_init(&d); d.crash_after_m0 = 1;
    mhi_boot_open(&m, &io);
    mhi_boot_begin(&m, 0, 0x1000, 0x80000, 0x2000, 32, 9);
    run_while(&d, &m, MhiSblLoading, 10); run_while(&d, &m, MhiWaitReady, 10);
    CHECK(mhi_boot_contexts_done(&m) == MhiFailed && m.error == MhiErrSysErr && m.failed_phase == MhiWaitSbl);

    /* No READY: the deadline. */
    dev_init(&d);
    mhi_boot_open(&m, &io);
    mhi_boot_begin(&m, 0, 0x1000, 0x80000, 0x2000, 32, 9);
    CHECK(run_while(&d, &m, MhiSblLoading, 10) == MhiWaitReady);
    d.action = 0;   /* the device never gets ready */
    CHECK(run_while(&d, &m, MhiWaitReady, 10) == MhiWaitReady && mhi_boot_timeout(&m) == MhiFailed && m.failed_phase == MhiWaitReady);

    /* The BHIe table: 512 KiB segments, the last one the remainder. */
    MhiBhieEntry table[8];
    const uint64_t addresses[8] = {0x80000000, 0x80080000, 0x80100000, 0x80180000, 0x80200000, 0x80280000, 0x80300000, 0x80380000};
    CHECK(mhi_bhie_table(table, 8, addresses, 3 * 0x80000 + 100, 0x80000) == 4);
    CHECK(table[0].address == 0x80000000 && table[0].size == 0x80000 && table[3].address == 0x80180000 && table[3].size == 100);
    CHECK(mhi_bhie_table(table, 8, addresses, 2 * 0x80000, 0x80000) == 2 && table[1].size == 0x80000);
    CHECK(mhi_bhie_table(table, 3, addresses, 3 * 0x80000 + 1, 0x80000) == 0 && mhi_bhie_table(table, 8, addresses, 0, 0x80000) == 0);
    CHECK(sizeof(MhiBhieEntry) == 16);
    printf("PASS MHI boot: %u checks\n", checks);
    return 0;
}
