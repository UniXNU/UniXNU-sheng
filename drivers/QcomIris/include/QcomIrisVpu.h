/* SPDX-License-Identifier: GPL-2.0-only */
/* The iris VPU 3 wrapper registers (iris_vpu_common.c, iris_vpu3x.c, iris_vpu_register_defines.h): preset and interrupt mask at
 * power-on, the UC region and firmware boot, the host-to-firmware doorbell, interrupt clear and watchdog, and the register halves of
 * the hardware and controller power-off. Clocks and power domains are the caller's (Sm8550VideoCc). Register access through
 * callbacks, so the host tests run the sequences against a model. */
#ifndef QCOM_IRIS_VPU_H
#define QCOM_IRIS_VPU_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define QCOM_IRIS_BASE 0x0aa00000ULL
#define QCOM_IRIS_SIZE 0x000f0000ULL
#define QCOM_IRIS_SPI 174u
#define QCOM_IRIS_PAS_ID 9u

struct qcom_iris_vpu_io {
    void *context;
    uint32_t (*read)(void *context, uint32_t offset);
    void (*write)(void *context, uint32_t offset, uint32_t value);   /* completes the store */
    void (*delay_us)(void *context, unsigned us);
};

/* iris_vpu_set_preset_registers and iris_vpu_interrupt_init */
void qcom_iris_vpu_preset(const struct qcom_iris_vpu_io *);
/* iris_vpu_boot_firmware with iris_vpu_setup_ucregion_memory_map: 0, -1 for an invalid UC region (CTRL_STATUS error 4), -2 when the
 * firmware never reports. status: the last CTRL_STATUS read. */
int qcom_iris_vpu_boot(const struct qcom_iris_vpu_io *, uint32_t queue_device_addr, uint32_t uc_region_size, uint32_t sfr_device_addr,
                       uint32_t *status);
/* iris_vpu_raise_interrupt */
void qcom_iris_vpu_raise(const struct qcom_iris_vpu_io *);
/* iris_vpu_clear_interrupt: the wrapper status (only bits of interest kept) and the doorbell cleared. */
uint32_t qcom_iris_vpu_clear(const struct qcom_iris_vpu_io *);
/* iris_vpu_watchdog: the status says the firmware's watchdog fired. */
bool qcom_iris_vpu_watchdog(uint32_t status);
/* iris_vpu_prepare_pc's checks: whether power collapse may be prepared now (firmware idle and in WFI), already ready, or neither. */
enum qcom_iris_pc {QcomIrisPcReady, QcomIrisPcPrepare, QcomIrisPcBusy};
enum qcom_iris_pc qcom_iris_vpu_pc_state(const struct qcom_iris_vpu_io *);
/* after the power-collapse command: PC_READY and WFI polled */
bool qcom_iris_vpu_pc_wait(const struct qcom_iris_vpu_io *);
/* iris_vpu3_power_off_hardware's register part; the caller then turns the core clock and MVS0 off (iris_vpu_power_off_hw). */
void qcom_iris_vpu3_power_off_hardware(const struct qcom_iris_vpu_io *, unsigned num_vpp_pipe);
/* iris_vpu_power_off_controller's register part; the caller then turns the controller clocks and MVS0C off. */
void qcom_iris_vpu_power_off_controller(const struct qcom_iris_vpu_io *);
#ifdef __cplusplus
}
#endif
#endif
