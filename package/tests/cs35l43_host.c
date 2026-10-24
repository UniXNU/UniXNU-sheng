// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the CS35L43 programs and their runner (package/ShengCs35l43.c) against a register model.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ShengCs35l43.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

typedef struct {uint32_t reg[0x20000 / 4]; unsigned writes, reads, waits; uint32_t key[8]; unsigned keys;} Amp;
static uint32_t be(const uint8_t *b) {return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];}

/* Runs a program to the end against the model; returns the transfer count. */
static unsigned play(Amp *m, Cs35Run *r)
{
    Cs35Action a;
    unsigned transfers = 0;
    for (int guard = 0; guard < 200; ++guard) {
        const int k = cs35_run_next(r, &a);
        if (k == Cs35Finished) return transfers;
        if (k == Cs35Wait) {m->waits += a.delay_ms; continue;}
        CHECK(k == Cs35Transfer);
        ++transfers;
        const uint32_t reg = be(a.bytes);
        CHECK(reg < sizeof m->reg && !(reg & 3));
        if (a.read_length) {
            CHECK(a.length == 4 && a.read_length == 4);
            m->reads++;
            /* the owner converts the wire bytes with cs35_wire_value before feeding them back */
            const uint32_t v = m->reg[reg / 4];
            cs35_run_result(r, v);
        } else {
            CHECK(a.length == 8);
            m->writes++;
            if (reg == CS35_TEST_KEY && m->keys < 8) m->key[m->keys++] = be(a.bytes + 4);
            m->reg[reg / 4] = be(a.bytes + 4);
            cs35_run_result(r, 0);
        }
    }
    CHECK(0);
    return 0;
}

int main(void)
{
    static Amp m;
    Cs35Step steps[CS35_MAX_STEPS];
    Cs35Run r;
    CHECK(cs35_wire_value(0x78563412u) == 0x12345678u);

    memset(&m, 0, sizeof m);
    m.reg[CS35_DEVID / 4] = 0x35a43; m.reg[CS35_REVID / 4] = 0xb0;
    m.reg[CS35_ASP_CONTROL2 / 4] = 0x20200200u;          /* reset value: 32-bit slots, DSP A */
    m.reg[CS35_AMP_GAIN / 4] = 0x233u;                    /* other gain fields are kept */
    unsigned n = cs35_init_program(steps, 1);
    CHECK(n > 10 && n <= CS35_MAX_STEPS);
    cs35_run_start(&r, steps, n);
    play(&m, &r);
    CHECK(r.read_count == 2 && r.reads[0] == 0x35a43 && r.reads[1] == 0xb0);
    CHECK(m.reg[CS35_TST_DAC_MSM_CONFIG / 4] == 0x11330000u && m.reg[CS35_BST_RSVD_1 / 4] == 0x50000802u);
    CHECK((m.reg[CS35_ASP_CONTROL2 / 4] & 0xff000700u) == (16u << 24 | 2u << 8) && (m.reg[CS35_ASP_CONTROL2 / 4] & 0x00200000u));
    CHECK(m.reg[CS35_ASP_DATA_CONTROL5 / 4] == 16 && m.reg[CS35_GLOBAL_SAMPLE_RATE / 4] == 3);
    CHECK(m.reg[CS35_REFCLK_INPUT / 4] == (0x1bu << 5 | 0x10u));   /* closed loop, enabled, bit clock */
    CHECK(m.reg[CS35_DACPCM1_INPUT / 4] == 0x08 && m.reg[CS35_ASP_FRAME_CONTROL5 / 4] == 1);
    CHECK(m.reg[CS35_AMP_GAIN / 4] == ((0x233u & ~0x3e0u) | 18u << 5) && (m.reg[CS35_AMP_CTRL / 4] & 7) == 4);
    CHECK(m.reg[CS35_BST_IPK_CTL / 4] == 0x40 && m.reg[CS35_FS_MON_0 / 4] == (71u | 115u << 12));
    CHECK(m.reg[CS35_IRQ1_MASK_1 / 4] == 0xffffffffu && (m.reg[CS35_BLOCK_ENABLES2 / 4] & 0x1000u));
    CHECK((m.reg[CS35_VPBR_CONFIG / 4] & 0x00ffff1fu) == (5u << 12 | 3u << 8 | 8u));

    /* A read-modify-write whose bits are already set costs one transfer. */
    unsigned before = m.writes;
    Cs35Step one[1] = {{Cs35Update, CS35_GLOBAL_SAMPLE_RATE, 0x1f, 3}};
    cs35_run_start(&r, one, 1);
    CHECK(play(&m, &r) == 1 && m.writes == before);

    /* Power up: serial port RX1, the unlock-guarded patch, then the enables. */
    n = cs35_power_up_program(steps);
    cs35_run_start(&r, steps, n);
    play(&m, &r);
    CHECK(m.keys == 4 && m.key[0] == 0x55 && m.key[1] == 0xaa && m.key[2] == 0xcc && m.key[3] == 0x33 && m.reg[0x2084 / 4] == 0x000f1aa0u);
    CHECK((m.reg[CS35_ASP_ENABLES1 / 4] & 0x10000u) && m.reg[CS35_GLOBAL_ENABLES / 4] == 1 && (m.reg[CS35_BLOCK_ENABLES / 4] & 1));

    /* Status: both interrupt registers read, then acknowledged. */
    m.reg[CS35_IRQ1_EINT_2 / 4] = CS35_EINT2_PLL_LOCK;
    n = cs35_status_program(steps);
    cs35_run_start(&r, steps, n);
    play(&m, &r);
    CHECK(r.read_count == 2 && r.reads[1] == CS35_EINT2_PLL_LOCK);

    /* Power down waits 2 ms before the global disable. */
    n = cs35_power_down_program(steps);
    cs35_run_start(&r, steps, n);
    play(&m, &r);
    CHECK(m.waits == 2 && m.reg[CS35_GLOBAL_ENABLES / 4] == 0 && !(m.reg[CS35_BLOCK_ENABLES / 4] & 1) && m.reg[0x2084 / 4] == 0x000f1aa3u);

    CHECK(cs35_slot(0) == 0 && cs35_slot(2) == 0 && cs35_slot(3) == 1 && cs35_slot(5) == 1);
    printf("PASS cs35l43: %u checks\n", checks);
    return 0;
}
