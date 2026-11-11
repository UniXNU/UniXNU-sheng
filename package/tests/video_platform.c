/* SPDX-License-Identifier: GPL-2.0-only */
/* The SM8550 video clock controller sequences against a register model: PLLs lock only after RUN, RCG updates and root enables
 * complete, branches report CLK_OFF from their enable bit, GDSCs report power-up and power-down in their CFG register. */
#include "Sm8550VideoCc.h"
#include "Sm8550AppsSmmu.h"
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct model {
    uint32_t gcc[0x40000 / 4], cc[0x10000 / 4];
    unsigned delays, writes;
    int stuck_pll;          /* PLL0 never locks */
    uint32_t order[64];     /* videocc offsets written to an enable bit, in order */
    unsigned ordered;
};
static uint32_t *reg(struct model *m, enum sm8550_video_cc_window w, uint32_t offset)
{
    if (w == Sm8550VideoGcc) {assert(offset >= 0x30000 && offset < 0x40000); return &m->gcc[offset / 4];}
    assert(offset < sizeof m->cc);
    return &m->cc[offset / 4];
}
static uint32_t rd(void *p, enum sm8550_video_cc_window w, uint32_t offset)
{
    struct model *m = p;
    uint32_t *r = reg(m, w, offset);
    if (w == Sm8550VideoCc) {
        /* PLL MODE: lock detect after RUN */
        if ((offset == 0x0000 || offset == 0x1000) && !(m->stuck_pll && offset == 0)) {
            if (m->cc[(offset + 4) / 4] & 1) *r |= 1u << 31; else *r &= ~(1u << 31);
        }
        /* RCG CMD: update completes, root follows ROOT_EN */
        if (offset == 0x8000) {
            *r &= ~1u;
            if (*r & 2) *r &= ~(1u << 31); else *r |= 1u << 31;
        }
        /* GDSC CFG: power state follows SW_COLLAPSE */
        if (offset == 0x804c + 4 || offset == 0x80a4 + 4) {
            const uint32_t gdscr = m->cc[(offset - 4) / 4];
            *r = (gdscr & 1) ? (1u << 15) : (1u << 16);
        }
    }
    /* branches: CLK_OFF follows the enable bit */
    if ((w == Sm8550VideoCc && (offset == 0x8064 || offset == 0x80b8)) || (w == Sm8550VideoGcc && offset == 0x32018)) {
        if (*r & 1) *r &= ~(1u << 31); else *r |= 1u << 31;
    }
    return *r;
}
static void wr(void *p, enum sm8550_video_cc_window w, uint32_t offset, uint32_t value)
{
    struct model *m = p;
    uint32_t *r = reg(m, w, offset);
    if (w == Sm8550VideoCc && (offset == 0x8064 || offset == 0x80b8 || offset == 0x804c || offset == 0x80a4) && m->ordered < 64)
        m->order[m->ordered++] = offset | (value & 1) << 31;
    *r = value;
    m->writes++;
}
static void delay(void *p, unsigned us) {struct model *m = p; m->delays += us;}

static void check_power_on(void)
{
    static struct model m;
    memset(&m, 0, sizeof m);
    m.cc[0x804c / 4] = 1;   /* both domains collapsed at reset */
    m.cc[0x80a4 / 4] = 1;
    m.cc[0x80c4 / 4] = 2;   /* hardware dividers: /3 and /2 */
    m.cc[0x8070 / 4] = 1;
    struct sm8550_video_cc_io io = {&m, rd, wr, delay};
    struct sm8550_video_cc_state s = {0};
    assert(!sm8550_video_cc_configure(&io, &s, 1000000000u));   /* not a table rate */
    assert(sm8550_video_cc_configure(&io, &s, 1332000000u));
    /* PLL0: L 69 with both calibration fields, alpha 0.375 in 16 bits; PLL1 at 1050 MHz as mainline's config */
    assert(m.cc[0x0010 / 4] == (0x45u | 0x44u << 16 | 0x44u << 24));
    assert(m.cc[0x0014 / 4] == 0x6000);
    assert(m.cc[0x1010 / 4] == (0x36u | 0x44u << 16 | 0x44u << 24) && m.cc[0x1014 / 4] == 0xb000);
    assert(m.cc[0x0020 / 4] == 0x20485699 && m.cc[0x0038 / 4] == 0x34 && m.cc[0x001c / 4] == 5);
    assert((m.cc[0x0000 / 4] & 5) == 4 && m.cc[0x0004 / 4] == 0);   /* reset released, output off, standby */
    assert((m.gcc[0x32004 / 4] & 1) && (m.gcc[0x32030 / 4] & 1));
    assert((m.cc[0x80f4 / 4] & 1) && (m.cc[0x8124 / 4] & 1) && (m.cc[0x8140 / 4] & 1));
    assert((m.cc[0x804c / 4] & 0xfff006) == (2u << 20 | 2u << 16 | 6u << 12));
    assert(s.mvs0_div == 3 && s.mvs0c_div == 2);

    assert(!sm8550_video_cc_hardware_on(&io, &s));   /* the controller first */
    m.ordered = 0;
    assert(sm8550_video_cc_controller_on(&io, &s));
    assert((m.cc[0x0000 / 4] & 1) && m.cc[0x0004 / 4] == 1 && (m.cc[0x0018 / 4] & 7) == 7);
    assert((m.cc[0x8004 / 4] & 0x71f) == (1u << 8 | 1u));    /* PLL0, pre-divider 1 */
    assert(!(m.cc[0x8000 / 4] & 2));                          /* force enable released */
    assert((m.gcc[0x32018 / 4] & 5) == 1);                    /* AXI on, reset released */
    assert(sm8550_video_cc_hardware_on(&io, &s));
    /* MVS0C domain powered (collapse cleared, then retention), then its clock */
    assert(m.order[0] == 0x804cu && m.order[1] == 0x804cu && m.order[2] == (0x8064u | 1u << 31));
    assert(sm8550_video_cc_hardware_mode(&io, &s, true) && (m.cc[0x80a4 / 4] & 2));
    sm8550_video_cc_controller_off(&io, &s);
    assert(s.controller_on);                                   /* the core stays on while the hardware is */
    sm8550_video_cc_hardware_off(&io, &s);
    assert(!(m.cc[0x80a4 / 4] & 2) && (m.cc[0x80a4 / 4] & 1) && !s.hardware_on);
    sm8550_video_cc_controller_off(&io, &s);
    assert(!s.controller_on && (m.cc[0x804c / 4] & 1) && !(m.cc[0x0000 / 4] & 1) && (m.cc[0x8004 / 4] & 0x71f) == 0);
    printf("video clocks: configure, controller and core on, hardware mode, off: ok (%u writes, %u us)\n", m.writes, m.delays);
}

static void check_pll_timeout(void)
{
    static struct model m;
    memset(&m, 0, sizeof m);
    m.stuck_pll = 1;
    m.cc[0x804c / 4] = 1;
    struct sm8550_video_cc_io io = {&m, rd, wr, delay};
    struct sm8550_video_cc_state s = {0};
    assert(sm8550_video_cc_configure(&io, &s, 1332000000u));
    assert(!sm8550_video_cc_controller_on(&io, &s));
    assert(s.failed_step == Sm8550VideoStepPll0Lock && !s.controller_on);
    printf("video clocks: a PLL that never locks fails the controller: ok\n");
}

/* The apps SMMU domain: a bank no stream uses and the firmware left unconfigured, an ASID no enabled bank holds, one free slot per
 * SID; a SID already routed is refused; route and unroute. */
static uint32_t smmu[0xd0000 / 4];
static uint32_t srd(void *c, uint32_t o) {(void)c; assert(o < sizeof smmu); return smmu[o / 4];}
static void swr(void *c, uint32_t o, uint32_t v) {(void)c; assert(o < sizeof smmu); smmu[o / 4] = v;}
static void check_smmu(void)
{
    memset(smmu, 0, sizeof smmu);
    smmu[0] = 1u << 10;                                   /* sCR0: enabled, unmatched streams fault */
    smmu[0x20 / 4] = (1u << 30) | (1u << 27) | 16;      /* 16 stream-match groups */
    smmu[0x24 / 4] = (3u << 28) | 8;                     /* 16 register pages, 8 banks */
    smmu[0x28 / 4] = 1u << 12;
    const unsigned pages = 16;
    smmu[0x800 / 4] = 0x80000040u; smmu[0xc00 / 4] = 1;  /* USB's stream on bank 1 */
    smmu[0x804 / 4] = 0x80000100u; smmu[0xc04 / 4] = 7;  /* a bypass stream on the last bank */
    smmu[0x1000 / 4 + 2] = 0x1f300;                      /* bank 2 configured by firmware */
    smmu[(pages + 3) * 1024] = 1;                        /* bank 3 enabled */
    smmu[(pages + 3) * 1024 + 0x24 / 4] = 1u << 16;      /* holding ASID 1 */
    struct sm8550_apps_smmu_io io = {NULL, srd, swr};
    struct sm8550_apps_smmu_domain d;
    const uint16_t sids[2] = {0x1940, 0x1947};
    assert(!sm8550_apps_smmu_pick(&io, sids, 2, &d));
    assert(d.bank == 4 && d.asid == 2 && d.slots[0] == 2 && d.slots[1] == 3 && d.count == 2 && d.pages == pages);
    assert(!sm8550_apps_smmu_route(&io, &d) && d.routed);
    assert(smmu[0xc08 / 4] == 4 && smmu[0x808 / 4] == 0x80001940u && smmu[0xc0c / 4] == 4 && smmu[0x80c / 4] == 0x80001947u);
    struct sm8550_apps_smmu_domain again;
    assert(sm8550_apps_smmu_pick(&io, sids, 2, &again) == -EBUSY);
    sm8550_apps_smmu_unroute(&io, &d);
    assert(smmu[0x808 / 4] == 0 && smmu[0xc08 / 4] == (2u << 16) && !d.routed);
    smmu[0] = 1;   /* the SMMU in client bypass: not driven */
    assert(sm8550_apps_smmu_pick(&io, sids, 2, &again) == -ENOTSUP);
    printf("apps SMMU: bank, ASID and slots picked, routed, refused when taken, unrouted: ok\n");
}

int main(void)
{
    check_power_on();
    check_pll_timeout();
    check_smmu();
    return 0;
}
