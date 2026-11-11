/* SPDX-License-Identifier: GPL-2.0-only */
/* sources: src/IrisVpu.c */
/* The VPU wrapper sequences against a register model: preset and interrupt unmask, the UC region and boot handshake (success, an
 * invalid region, a firmware that never answers), the doorbell, interrupt clear and watchdog, and the power-off register halves. */
#include "QcomIrisVpu.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t regs[0xf0000 / 4];
static int boot_reply;   /* the CTRL_STATUS the firmware reports after CTRL_INIT */
static unsigned delays;
static uint32_t rd(void *c, uint32_t o)
{
    (void)c;
    assert(o < sizeof regs && !(o & 3));
    if (o == 0xa004c && regs[0xa0048 / 4]) regs[o / 4] = (uint32_t)boot_reply;
    if (o == 0xe0004 || o == 0xb0060) regs[o / 4] |= 1;   /* LPI handshakes done */
    if (o == 0x1f004) regs[o / 4] = regs[0x1f000 / 4] ? 3 : 0;   /* NoC reset acknowledges follow the request */
    if (o >= 0x70 && o < 0x80) regs[o / 4] = 0x400000;          /* pipes idle */
    return regs[o / 4];
}
static void wr(void *c, uint32_t o, uint32_t v) {(void)c; assert(o < sizeof regs && !(o & 3)); regs[o / 4] = v;}
static void delay(void *c, unsigned us) {(void)c; delays += us;}

int main(void)
{
    struct qcom_iris_vpu_io io = {NULL, rd, wr, delay};
    regs[0xb0010 / 4] = 0xff;
    qcom_iris_vpu_preset(&io);
    assert(regs[0xb0010 / 4] == (0xffu & ~0xcu) && regs[0xb0088 / 4] == 0);

    uint32_t status = 0;
    boot_reply = 1;
    assert(!qcom_iris_vpu_boot(&io, 0x30000000, 0x400000, 0x30400000, &status) && status == 1);
    assert(regs[0xa0064 / 4] == 0x30000000 && regs[0xa0068 / 4] == 0x400000 && regs[0xa0054 / 4] == 0x30000000 && regs[0xa0050 / 4] == 1);
    assert(regs[0xa005c / 4] == 0x30400001 && regs[0xa0058 / 4] == 1 && regs[0xa0148 / 4] == 1 && regs[0xa0168 / 4] == 0);
    memset(regs, 0, sizeof regs);
    boot_reply = 0x4;
    assert(qcom_iris_vpu_boot(&io, 0x30000000, 0x400000, 0, &status) == -1);
    memset(regs, 0, sizeof regs);
    boot_reply = 0;
    delays = 0;
    assert(qcom_iris_vpu_boot(&io, 0x30000000, 0x400000, 0, &status) == -2 && delays == 50000);

    qcom_iris_vpu_raise(&io);
    assert(regs[0xa0150 / 4] == 1);
    regs[0xb000c / 4] = 1u << 2;
    assert(qcom_iris_vpu_clear(&io) == 1u << 2 && regs[0xa001c / 4] == 1 && !qcom_iris_vpu_watchdog(1u << 2));
    regs[0xb000c / 4] = 1u << 3;
    assert(qcom_iris_vpu_watchdog(qcom_iris_vpu_clear(&io)));
    regs[0xb000c / 4] = 1u << 9;   /* none of the bits of interest */
    assert(qcom_iris_vpu_clear(&io) == 0);

    regs[0xa004c / 4] = 1u << 30; regs[0xa0048 / 4] = 0; regs[0xc0010 / 4] = 1;
    assert(qcom_iris_vpu_pc_state(&io) == QcomIrisPcPrepare);
    regs[0xa004c / 4] = 0x100;
    assert(qcom_iris_vpu_pc_state(&io) == QcomIrisPcReady);

    regs[0xb0080 / 4] = 2;   /* hardware powered */
    regs[0xb0088 / 4] = 5;
    qcom_iris_vpu3_power_off_hardware(&io, 4);
    assert(regs[0xb0088 / 4] == 0 && regs[0x1f000 / 4] == 0 && regs[0xa0160 / 4] == 0);
    qcom_iris_vpu_power_off_controller(&io);
    assert(regs[0xa0168 / 4] == 3 && regs[0xb005c / 4] == 1 && regs[0xc0014 / 4] == 0 && regs[0xc0018 / 4] == 0);
    printf("vpu: preset, boot, doorbell, interrupt clear, watchdog, power collapse checks, power-off: ok\n");
    return 0;
}
