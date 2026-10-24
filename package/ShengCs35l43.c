/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "ShengCs35l43.h"

typedef struct {Cs35Step *p; unsigned n;} P;
static void w(P *p, uint32_t reg, uint32_t value) {if (p->n < CS35_MAX_STEPS) p->p[p->n++] = (Cs35Step){Cs35Write, reg, 0, value};}
static void u(P *p, uint32_t reg, uint32_t mask, uint32_t value) {if (p->n < CS35_MAX_STEPS) p->p[p->n++] = (Cs35Step){Cs35Update, reg, mask, value & mask};}
static void r(P *p, uint32_t reg) {if (p->n < CS35_MAX_STEPS) p->p[p->n++] = (Cs35Step){Cs35Read, reg, 0, 0};}
static void d(P *p, uint32_t ms) {if (p->n < CS35_MAX_STEPS) p->p[p->n++] = (Cs35Step){Cs35DelayMs, 0, 0, ms};}

unsigned cs35_init_program(Cs35Step *out, unsigned slot)
{
    P p = {out, 0};
    r(&p, CS35_DEVID);
    r(&p, CS35_REVID);
    /* Errata the vendor applies after reset. */
    w(&p, CS35_TST_DAC_MSM_CONFIG, 0x11330000u);
    w(&p, CS35_BST_RSVD_1, 0x50000802u);
    /* Interrupts stay masked: the owner reads the status registers. */
    w(&p, CS35_IRQ1_MASK_1, 0xffffffffu);
    w(&p, CS35_IRQ1_MASK_2, 0xffffffffu);
    w(&p, CS35_IRQ1_MASK_3, 0xffffffffu);
    /* DC input watchdog on, threshold 1. */
    u(&p, CS35_ALIVE_DCIN_WD, 0x7fu, 1u << 1 | 1u);
    /* Board values: 4 A boost inductor peak current, SDOUT high impedance when unused, GPIO2 push-pull active low, brownout
     * protection (threshold 8, attack volume 5, maximum attenuation 3). */
    u(&p, CS35_BST_IPK_CTL, 0x7fu, (4000u - 1600u) / 50u + 0x10u);
    u(&p, CS35_ASP_CONTROL3, 0x3u, 0x3u);
    u(&p, CS35_GPIO_PAD_CONTROL, 0x07000000u, 4u << 24);
    u(&p, CS35_VPBR_CONFIG, 0x00ffff1fu, 0u << 21 | 0u << 19 | 0u << 16 | 5u << 12 | 3u << 8 | 8u);
    u(&p, CS35_BLOCK_ENABLES2, 0x1000u, 0x1000u);
    /* Serial port: I2S, clock and frame from the SoC, 16-bit slots and words, 48 kHz. */
    u(&p, CS35_ASP_CONTROL2, 0xff000000u | 0x700u | 0x40u | 0x10u | 0x4u | 0x1u, 16u << 24 | 2u << 8);
    u(&p, CS35_ASP_DATA_CONTROL5, 0x3fu, 16u);
    u(&p, CS35_GLOBAL_SAMPLE_RATE, 0x1fu, 0x03u);
    /* The PLL locks to the 1.536 MHz bit clock (reference select 0); frequency code 0x1B, FS monitor windows 71 and 115. */
    u(&p, CS35_REFCLK_INPUT, 0x800u, 0x800u);
    u(&p, CS35_REFCLK_INPUT, 0x7e0u, 0x1bu << 5);
    u(&p, CS35_REFCLK_INPUT, 0x10u, 0);
    u(&p, CS35_REFCLK_INPUT, 0x7u, 0);
    u(&p, CS35_REFCLK_INPUT, 0x800u, 0);
    u(&p, CS35_REFCLK_INPUT, 0x10u, 0x10u);
    w(&p, CS35_FS_MON_0, 71u | 115u << 12);
    /* The amplifier plays ASP RX1 directly; this amplifier's slot; gain; 4 ms soft ramp. */
    u(&p, CS35_DACPCM1_INPUT, 0x3fu, 0x08u);
    u(&p, CS35_ASP_FRAME_CONTROL5, 0x3fu, slot);
    u(&p, CS35_AMP_GAIN, 0x3e0u, CS35_GAIN_DB << 5);
    u(&p, CS35_AMP_CTRL, 0x7u, 4u);
    return p.n;
}

unsigned cs35_power_up_program(Cs35Step *out)
{
    P p = {out, 0};
    u(&p, CS35_ASP_ENABLES1, 1u << 16, 1u << 16);
    w(&p, CS35_TEST_KEY, 0x55u);
    w(&p, CS35_TEST_KEY, 0xaau);
    w(&p, 0x2084u, 0x000f1aa0u);
    w(&p, CS35_TEST_KEY, 0xccu);
    w(&p, CS35_TEST_KEY, 0x33u);
    w(&p, CS35_GLOBAL_ENABLES, 1u);
    u(&p, CS35_BLOCK_ENABLES, 1u, 1u);
    return p.n;
}

unsigned cs35_power_down_program(Cs35Step *out)
{
    P p = {out, 0};
    u(&p, CS35_BLOCK_ENABLES, 1u, 0);
    w(&p, CS35_TEST_KEY, 0x55u);
    w(&p, CS35_TEST_KEY, 0xaau);
    w(&p, 0x2084u, 0x000f1aa3u);
    w(&p, CS35_TEST_KEY, 0xccu);
    w(&p, CS35_TEST_KEY, 0x33u);
    d(&p, 2);
    w(&p, CS35_GLOBAL_ENABLES, 0);
    u(&p, CS35_ASP_ENABLES1, 1u << 16, 0);
    return p.n;
}

unsigned cs35_status_program(Cs35Step *out)
{
    P p = {out, 0};
    r(&p, CS35_IRQ1_EINT_1);
    r(&p, CS35_IRQ1_EINT_2);
    w(&p, CS35_IRQ1_EINT_1, 0xffffffffu);
    w(&p, CS35_IRQ1_EINT_2, 0xffffffffu);
    return p.n;
}

void cs35_run_start(Cs35Run *r, const Cs35Step *steps, unsigned count)
{
    r->steps = steps; r->count = count; r->index = 0; r->phase = 0; r->read_count = 0; r->pending = 0;
    for (unsigned i = 0; i < 4; ++i) r->reads[i] = 0;
}

static void be32(uint8_t *b, uint32_t v) {b[0] = (uint8_t)(v >> 24); b[1] = (uint8_t)(v >> 16); b[2] = (uint8_t)(v >> 8); b[3] = (uint8_t)v;}

int cs35_run_next(Cs35Run *r, Cs35Action *a)
{
    a->length = a->read_length = a->delay_ms = 0;
    if (r->index >= r->count) return Cs35Finished;
    const Cs35Step *s = &r->steps[r->index];
    if (r->phase == 4) {   /* the write half of a read-modify-write */
        be32(a->bytes, s->reg); be32(a->bytes + 4, r->pending); a->length = 8;
        r->phase = 2;
        return Cs35Transfer;
    }
    switch (s->op) {
    case Cs35DelayMs:
        a->delay_ms = s->value;
        r->index++;
        return Cs35Wait;
    case Cs35Write:
        be32(a->bytes, s->reg); be32(a->bytes + 4, s->value); a->length = 8;
        r->phase = 2;
        return Cs35Transfer;
    case Cs35Update:
    case Cs35Read:
        be32(a->bytes, s->reg); a->length = 4; a->read_length = 4;
        r->phase = s->op == Cs35Update ? 1 : 3;
        return Cs35Transfer;
    default:
        r->index = r->count;
        return Cs35Finished;
    }
}

void cs35_run_result(Cs35Run *r, uint32_t value)
{
    if (r->index >= r->count) return;
    const Cs35Step *s = &r->steps[r->index];
    if (r->phase == 1) {
        /* The read half of a read-modify-write: the write half is the next transfer, unless the bits are already set. */
        r->pending = (value & ~s->mask) | s->value;
        if (r->pending == value) {r->phase = 0; r->index++;} else r->phase = 4;
        return;
    }
    if (r->phase == 3 && r->read_count < 4) r->reads[r->read_count++] = value;
    r->phase = 0;
    r->index++;
}
