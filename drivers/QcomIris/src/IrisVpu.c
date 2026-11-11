/* SPDX-License-Identifier: GPL-2.0-only */
#include "QcomIrisVpu.h"

/* iris_vpu_register_defines.h */
#define VCODEC_SS_IDLE_STATUSN 0x00070u
#define AON_MVP_NOC_RESET 0x1f000u
#define AON_WRAPPER_MVP_NOC_RESET_REQ (AON_MVP_NOC_RESET + 0x000)
#define AON_WRAPPER_MVP_NOC_RESET_ACK (AON_MVP_NOC_RESET + 0x004)
#define VIDEO_NOC_RESET_REQ 3u
#define CPU_CS 0xa0000u
#define CPU_CS_A2HSOFTINTCLR (CPU_CS + 0x1c)
#define CPU_CS_H2XSOFTINTEN (CPU_CS + 0x148)
#define CPU_IC_SOFTINT (CPU_CS + 0x150)
#define CPU_CS_AHB_BRIDGE_SYNC_RESET (CPU_CS + 0x160)
#define CORE_BRIDGE_SW_RESET 1u
#define CORE_BRIDGE_HW_RESET_DISABLE 2u
#define CPU_CS_X2RPMH (CPU_CS + 0x168)
#define MSK_SIGNAL_FROM_TENSILICA 1u
#define MSK_CORE_POWER_ON 2u
#define CTRL_INIT (CPU_CS + 0x48)
#define CTRL_STATUS (CPU_CS + 0x4c)
#define CTRL_ERROR_STATUS_M 0xfeu
#define CTRL_STATUS_PC_READY 0x100u
#define CTRL_INIT_IDLE_MSG_BMSK 0x40000000u
#define QTBL_INFO (CPU_CS + 0x50)
#define QTBL_ADDR (CPU_CS + 0x54)
#define CPU_CS_SCIACMDARG3 (CPU_CS + 0x58)
#define SFR_ADDR (CPU_CS + 0x5c)
#define UC_REGION_ADDR (CPU_CS + 0x64)
#define UC_REGION_SIZE (CPU_CS + 0x68)
#define WRAPPER 0xb0000u
#define WRAPPER_INTR_STATUS (WRAPPER + 0x0c)
#define WRAPPER_INTR_STATUS_A2HWD (1u << 3)
#define WRAPPER_INTR_STATUS_A2H (1u << 2)
#define WRAPPER_INTR_MASK (WRAPPER + 0x10)
#define WRAPPER_INTR_MASK_A2HWD (1u << 3)
#define WRAPPER_INTR_MASK_A2HCPU (1u << 2)
#define WRAPPER_DEBUG_BRIDGE_LPI_CONTROL (WRAPPER + 0x54)
#define WRAPPER_DEBUG_BRIDGE_LPI_STATUS (WRAPPER + 0x58)
#define WRAPPER_IRIS_CPU_NOC_LPI_CONTROL (WRAPPER + 0x5c)
#define WRAPPER_IRIS_CPU_NOC_LPI_STATUS (WRAPPER + 0x60)
#define REQ_POWER_DOWN_PREP 1u
#define WRAPPER_CORE_POWER_STATUS (WRAPPER + 0x80)
#define WRAPPER_CORE_CLOCK_CONFIG (WRAPPER + 0x88)
#define CORE_CLK_RUN 0u
#define WRAPPER_PRESET (WRAPPER + 0x88)   /* iris_vpu_set_preset_registers writes 0xb0088 */
#define WRAPPER_TZ 0xc0000u
#define WRAPPER_TZ_CPU_STATUS (WRAPPER_TZ + 0x10)
#define WRAPPER_TZ_CTL_AXI_CLOCK_CONFIG (WRAPPER_TZ + 0x14)
#define CTL_AXI_CLK_HALT 1u
#define CTL_CLK_HALT 2u
#define WRAPPER_TZ_QNS4PDXFIFO_RESET (WRAPPER_TZ + 0x18)
#define RESET_HIGH 1u
#define AON 0xe0000u
#define AON_WRAPPER_MVP_NOC_LPI_CONTROL (AON + 0x0)
#define AON_WRAPPER_MVP_NOC_LPI_STATUS (AON + 0x4)
#define SFR_ARCH_OFFSET 1u   /* iris_hfi_gen2_data.core_arch: VIDEO_ARCH_LX */

static uint32_t rd(const struct qcom_iris_vpu_io *io, uint32_t o) {return io->read(io->context, o);}
static void wr(const struct qcom_iris_vpu_io *io, uint32_t o, uint32_t v) {io->write(io->context, o, v);}

/* readl_poll_timeout: every `step` us until the condition or `timeout` us */
static bool poll(const struct qcom_iris_vpu_io *io, uint32_t offset, uint32_t mask, uint32_t value, unsigned step, unsigned timeout)
{
    for (unsigned waited = 0;; waited += step) {
        if ((rd(io, offset) & mask) == value) return true;
        if (waited >= timeout) return false;
        io->delay_us(io->context, step);
    }
}

void qcom_iris_vpu_preset(const struct qcom_iris_vpu_io *io)
{
    wr(io, WRAPPER_PRESET, 0);
    wr(io, WRAPPER_INTR_MASK, rd(io, WRAPPER_INTR_MASK) & ~(WRAPPER_INTR_MASK_A2HWD | WRAPPER_INTR_MASK_A2HCPU));
}

int qcom_iris_vpu_boot(const struct qcom_iris_vpu_io *io, uint32_t queue, uint32_t uc_region_size, uint32_t sfr, uint32_t *status)
{
    wr(io, UC_REGION_ADDR, queue);
    wr(io, UC_REGION_SIZE, uc_region_size);
    wr(io, QTBL_ADDR, queue);
    wr(io, QTBL_INFO, 1);   /* QTBL_ENABLE */
    if (sfr) wr(io, SFR_ADDR, sfr + SFR_ARCH_OFFSET);
    wr(io, CTRL_INIT, 1);
    wr(io, CPU_CS_SCIACMDARG3, 1);
    uint32_t s = 0;
    for (unsigned tries = 0; !s && tries < 1000; ++tries) {
        s = rd(io, CTRL_STATUS);
        if ((s & CTRL_ERROR_STATUS_M) == 0x4) {if (status) *status = s; return -1;}
        io->delay_us(io->context, 50);
    }
    if (status) *status = s;
    if (!s) return -2;
    wr(io, CPU_CS_H2XSOFTINTEN, 1);   /* HOST2XTENSA_INTR_ENABLE */
    wr(io, CPU_CS_X2RPMH, 0);
    return 0;
}

void qcom_iris_vpu_raise(const struct qcom_iris_vpu_io *io) {wr(io, CPU_IC_SOFTINT, 1);}

uint32_t qcom_iris_vpu_clear(const struct qcom_iris_vpu_io *io)
{
    const uint32_t status = rd(io, WRAPPER_INTR_STATUS);
    const uint32_t mask = WRAPPER_INTR_STATUS_A2H | WRAPPER_INTR_STATUS_A2HWD | CTRL_INIT_IDLE_MSG_BMSK;
    wr(io, CPU_CS_A2HSOFTINTCLR, 1);
    return status & mask ? status : 0;
}

bool qcom_iris_vpu_watchdog(uint32_t status) {return (status & WRAPPER_INTR_STATUS_A2HWD) != 0;}

enum qcom_iris_pc qcom_iris_vpu_pc_state(const struct qcom_iris_vpu_io *io)
{
    const uint32_t ctrl = rd(io, CTRL_STATUS);
    if (ctrl & CTRL_STATUS_PC_READY) return QcomIrisPcReady;
    const bool wfi = rd(io, WRAPPER_TZ_CPU_STATUS) & 1, idle = ctrl & (1u << 30);
    return wfi && idle ? QcomIrisPcPrepare : QcomIrisPcBusy;
}

bool qcom_iris_vpu_pc_wait(const struct qcom_iris_vpu_io *io)
{
    return poll(io, CTRL_STATUS, CTRL_STATUS_PC_READY, CTRL_STATUS_PC_READY, 250, 2500) && poll(io, WRAPPER_TZ_CPU_STATUS, 1, 1, 250, 2500);
}

void qcom_iris_vpu3_power_off_hardware(const struct qcom_iris_vpu_io *io, unsigned pipes)
{
    if (!(rd(io, WRAPPER_CORE_POWER_STATUS) & 2)) return;   /* iris_vpu3x_hw_power_collapsed */
    if (rd(io, WRAPPER_CORE_CLOCK_CONFIG)) wr(io, WRAPPER_CORE_CLOCK_CONFIG, CORE_CLK_RUN);
    for (unsigned i = 0; i < pipes; ++i)
        if (!poll(io, VCODEC_SS_IDLE_STATUSN + 4 * i, 0x400000, 0x400000, 2000, 20000)) return;
    wr(io, AON_WRAPPER_MVP_NOC_RESET_REQ, VIDEO_NOC_RESET_REQ);
    if (!poll(io, AON_WRAPPER_MVP_NOC_RESET_ACK, 3, 3, 200, 2000)) return;
    wr(io, AON_WRAPPER_MVP_NOC_RESET_REQ, 0);
    if (!poll(io, AON_WRAPPER_MVP_NOC_RESET_ACK, 3, 0, 200, 2000)) return;
    wr(io, CPU_CS_AHB_BRIDGE_SYNC_RESET, CORE_BRIDGE_SW_RESET | CORE_BRIDGE_HW_RESET_DISABLE);
    wr(io, CPU_CS_AHB_BRIDGE_SYNC_RESET, CORE_BRIDGE_HW_RESET_DISABLE);
    wr(io, CPU_CS_AHB_BRIDGE_SYNC_RESET, 0);
}

void qcom_iris_vpu_power_off_controller(const struct qcom_iris_vpu_io *io)
{
    wr(io, CPU_CS_X2RPMH, MSK_SIGNAL_FROM_TENSILICA | MSK_CORE_POWER_ON);
    wr(io, AON_WRAPPER_MVP_NOC_LPI_CONTROL, REQ_POWER_DOWN_PREP);
    if (!poll(io, AON_WRAPPER_MVP_NOC_LPI_STATUS, 1, 1, 200, 2000)) return;
    wr(io, WRAPPER_IRIS_CPU_NOC_LPI_CONTROL, REQ_POWER_DOWN_PREP);
    if (!poll(io, WRAPPER_IRIS_CPU_NOC_LPI_STATUS, 1, 1, 200, 2000)) return;
    wr(io, WRAPPER_DEBUG_BRIDGE_LPI_CONTROL, 0);
    if (!poll(io, WRAPPER_DEBUG_BRIDGE_LPI_STATUS, 0xffffffffu, 0, 200, 2000)) return;
    wr(io, WRAPPER_TZ_CTL_AXI_CLOCK_CONFIG, CTL_AXI_CLK_HALT | CTL_CLK_HALT);
    wr(io, WRAPPER_TZ_QNS4PDXFIFO_RESET, RESET_HIGH);
    wr(io, WRAPPER_TZ_QNS4PDXFIFO_RESET, 0);
    wr(io, WRAPPER_TZ_CTL_AXI_CLOCK_CONFIG, 0);
}
