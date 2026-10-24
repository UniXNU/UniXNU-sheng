/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "MhiBoot.h"

static uint32_t rd(MhiBoot *m, uint32_t reg) {return m->io.read(m->io.ctx, reg);}
static void wr(MhiBoot *m, uint32_t reg, uint32_t value) {m->io.write(m->io.ctx, reg, value);}

int mhi_boot_open(MhiBoot *m, const MhiIo *io)
{
    for (unsigned i = 0; i < sizeof *m; ++i) ((uint8_t *)m)[i] = 0;
    m->io = *io;
    m->reglen = io->space ? io->space : rd(m, MHI_REGLEN);
    m->bhi = rd(m, MHI_BHIOFF);
    m->bhie = rd(m, MHI_BHIEOFF);
    const int good = m->reglen >= 0x100 && m->reglen <= 0x100000 && m->bhi >= 0x100 && m->bhi + MHI_BHI_ERRCODE + 4 <= m->reglen &&
                     m->bhie >= 0x100 && m->bhie + MHI_BHIE_TXVECSTATUS + 4 <= m->reglen && !(m->bhi & 3) && !(m->bhie & 3);
    if (!good) {m->phase = MhiFailed; m->error = MhiErrLayout; return -1;}
    m->phase = MhiIdle;
    return 0;
}

uint32_t mhi_boot_ee(MhiBoot *m) {return m->ee = rd(m, m->bhi + MHI_BHI_EXECENV);}
uint32_t mhi_boot_state(MhiBoot *m) {return m->state = (rd(m, MHI_STATUS) >> MHI_STATE_SHIFT) & 0xffu;}

static uint32_t fail(MhiBoot *m, uint32_t error)
{
    m->failed_phase = m->phase;
    m->error = error;
    m->phase = MhiFailed;
    return m->phase;
}

static uint32_t start_sbl(MhiBoot *m)
{
    wr(m, m->bhi + MHI_BHI_INTVEC, m->msi_vector);  /* a reset clears it */
    wr(m, m->bhi + MHI_BHI_IMGADDR_HIGH, (uint32_t)(m->sbl_addr >> 32));
    wr(m, m->bhi + MHI_BHI_IMGADDR_LOW, (uint32_t)m->sbl_addr);
    wr(m, m->bhi + MHI_BHI_IMGSIZE, m->sbl_size);
    wr(m, m->bhi + MHI_BHI_IMGTXDB, m->seq);
    m->phase = MhiSblLoading;
    return m->phase;
}

uint32_t mhi_boot_begin(MhiBoot *m, uint32_t msi_vector, uint64_t sbl_addr, uint32_t sbl_size, uint64_t table_addr, uint32_t table_size,
                        uint32_t seq)
{
    if (m->phase != MhiIdle) return fail(m, MhiErrState);
    m->msi_vector = msi_vector;
    m->sbl_addr = sbl_addr; m->sbl_size = sbl_size;
    m->table_addr = table_addr; m->table_size = table_size;
    m->seq = seq & MHI_SEQ_MASK;
    if (!m->seq) m->seq = 1;
    const uint32_t ee = mhi_boot_ee(m);
    if (ee != MhiEePbl && ee != MhiEeEdl) return fail(m, MhiErrNotPbl);
    const uint32_t status = rd(m, MHI_STATUS);
    m->state = (status >> MHI_STATE_SHIFT) & 0xffu;
    wr(m, m->bhi + MHI_BHI_INTVEC, msi_vector);
    if ((status & MHI_STATUS_SYSERR) || m->state == MhiStateSysErr) {
        wr(m, MHI_CTRL, MHI_CTRL_RESET);
        m->phase = MhiResetting;
        return m->phase;
    }
    return start_sbl(m);
}

static uint32_t start_image(MhiBoot *m)
{
    m->seq = (m->seq + 1) & MHI_SEQ_MASK;
    if (!m->seq) m->seq = 1;
    wr(m, m->bhie + MHI_BHIE_TXVECADDR_HIGH, (uint32_t)(m->table_addr >> 32));
    wr(m, m->bhie + MHI_BHIE_TXVECADDR_LOW, (uint32_t)m->table_addr);
    wr(m, m->bhie + MHI_BHIE_TXVECSIZE, m->table_size);
    wr(m, m->bhie + MHI_BHIE_TXVECDB, m->seq);
    m->phase = MhiImageLoading;
    return m->phase;
}

uint32_t mhi_boot_step(MhiBoot *m)
{
    if (m->phase == MhiIdle || m->phase == MhiDone || m->phase == MhiFailed || m->phase == MhiNeedContexts) return m->phase;
    const uint32_t status = rd(m, MHI_STATUS);
    if (status == 0xffffffffu) return m->phase;   /* the device does not answer (its link is retraining while its boot loader starts): not a status */
    m->state = (status >> MHI_STATE_SHIFT) & 0xffu;
    if (m->phase == MhiResetting) {
        if (rd(m, MHI_CTRL) & MHI_CTRL_RESET) return m->phase;
        return start_sbl(m);
    }
    const uint32_t ee = mhi_boot_ee(m);
    if (ee == MhiEeRddm) return fail(m, MhiErrRddm);
    if ((status & MHI_STATUS_SYSERR) || m->state == MhiStateSysErr) return fail(m, MhiErrSysErr);
    switch (m->phase) {
    case MhiSblLoading: {
        const uint32_t bhi = rd(m, m->bhi + MHI_BHI_STATUS);
        m->last_status = bhi;
        if (bhi == 0xffffffffu) return m->phase;
        if (bhi >> 30 == MHI_XFER_ERROR) {m->errcode = rd(m, m->bhi + MHI_BHI_ERRCODE); return fail(m, MhiErrBhi);}
        if (bhi >> 30 != MHI_XFER_SUCCESS) return m->phase;
        m->phase = MhiWaitReady;
        return mhi_boot_step(m);
    }
    case MhiWaitReady:
        if ((rd(m, MHI_CTRL) & MHI_CTRL_RESET) || !(status & MHI_STATUS_READY)) return m->phase;
        m->phase = MhiNeedContexts;
        return m->phase;
    case MhiWaitSbl:
        if (ee == MhiEeAmss || ee == MhiEeWfw) {m->phase = MhiDone; return m->phase;}  /* the device had the full image already */
        if (ee != MhiEeSbl) return m->phase;
        return start_image(m);
    case MhiImageLoading: {
        const uint32_t tx = rd(m, m->bhie + MHI_BHIE_TXVECSTATUS);
        m->last_status = tx;
        if ((tx & MHI_SEQ_MASK) != m->seq) return m->phase;  /* not this transfer's status yet */
        if (tx >> 30 == MHI_XFER_ERROR) return fail(m, MhiErrBhie);
        if (tx >> 30 != MHI_XFER_SUCCESS) return m->phase;
        m->phase = MhiWaitMission;
        return mhi_boot_step(m);
    }
    case MhiWaitMission:
        if (ee == MhiEeAmss || ee == MhiEeWfw) m->phase = MhiDone;
        return m->phase;
    default:
        return m->phase;
    }
}

uint32_t mhi_boot_contexts_done(MhiBoot *m)
{
    if (m->phase != MhiNeedContexts) return fail(m, MhiErrState);
    wr(m, MHI_CTRL, (rd(m, MHI_CTRL) & ~(0xffu << MHI_STATE_SHIFT) & ~MHI_CTRL_RESET) | (uint32_t)MhiStateM0 << MHI_STATE_SHIFT);
    m->phase = MhiWaitSbl;
    return mhi_boot_step(m);
}

uint32_t mhi_boot_timeout(MhiBoot *m)
{
    if (m->phase == MhiIdle || m->phase == MhiDone || m->phase == MhiFailed) return m->phase;
    if (m->phase == MhiSblLoading) m->errcode = rd(m, m->bhi + MHI_BHI_ERRCODE);
    return fail(m, MhiErrTimeout);
}

uint32_t mhi_bhie_table(MhiBhieEntry *table, uint32_t count, const uint64_t *addresses, uint32_t image_size, uint32_t segment_size)
{
    if (!segment_size || !image_size) return 0;
    const uint32_t needed = (image_size + segment_size - 1) / segment_size;
    if (needed > count) return 0;
    for (uint32_t i = 0; i < needed; ++i) {
        const uint32_t left = image_size - i * segment_size;
        table[i].address = addresses[i];
        table[i].size = left < segment_size ? left : segment_size;
    }
    return needed;
}
