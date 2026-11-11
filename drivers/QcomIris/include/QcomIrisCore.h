/* SPDX-License-Identifier: GPL-2.0-only */
/* The iris core: iris_core.c (init, deinit, the wait for the system's answer), iris_hfi_common.c (core init messages, the threaded
 * interrupt, runtime suspend and resume), iris_hfi_queue.c's command write, iris_hfi_gen2_response.c's dispatch (system, session,
 * debug queue, watchdog and system errors) and iris_probe.c's system error recovery. Platform work (power domains, clocks, firmware
 * loading, the interrupt line, waiting and deferred work) goes through the ops; register sequences through QcomIrisVpu.
 *
 * Locking follows mainline's core->lock with the sessions' locks folded into it: the caller holds one lock around every call here
 * and every session call, and the ops' waits drop it while they sleep (as iris_wait_for_session_response drops inst->lock). The
 * interrupt thread calls qcom_iris_core_thread with the lock held and enables the line after it returns true. */
#ifndef QCOM_IRIS_CORE_H
#define QCOM_IRIS_CORE_H
#include <stdbool.h>
#include <stdint.h>
#include "QcomIrisHfi.h"
#include "QcomIrisSession.h"
#include "QcomIrisVpu.h"
#ifdef __cplusplus
extern "C" {
#endif
#define QCOM_IRIS_RESPONSE_TIMEOUT_MS 1000u                  /* HW_RESPONSE_TIMEOUT_VALUE */
#define QCOM_IRIS_AUTOSUSPEND_MS (QCOM_IRIS_RESPONSE_TIMEOUT_MS + 500u)   /* AUTOSUSPEND_DELAY_VALUE */
#define QCOM_IRIS_RECOVERY_MS 10u                            /* the system error handler's delay */
enum {QcomIrisMaxSessions = 16};
enum qcom_iris_core_state {QcomIrisCoreDeinit = 1, QcomIrisCoreInit, QcomIrisCoreError};

struct qcom_iris_core_ops {
    void *context;
    /* iris_vpu_power_on's platform half: the controller (MVS0C, the reset, the AXI and controller clocks), the hardware (MVS0 and the
     * core clock at its rate) and the hardware power domain handed to the hardware; the off halves run after the register halves. */
    int (*power_on_controller)(void *context);
    int (*power_on_hardware)(void *context);
    int (*set_hwmode)(void *context);
    void (*power_off_hardware)(void *context);
    void (*power_off_controller)(void *context);
    /* iris_fw_load (the image into its carveout, authentication and reset, content protection), iris_fw_unload, iris_set_hw_state */
    int (*load_firmware)(void *context);
    void (*unload_firmware)(void *context);
    int (*set_remote_state)(void *context, bool resume);
    /* The interrupt line: power-on enables it, power-off disables it (the thread left it disabled after a watchdog). */
    void (*enable_irq)(void *context);
    void (*disable_irq)(void *context);
    /* Waits on core_init_done with the lock dropped; true when signalled. */
    bool (*wait_init)(void *context, unsigned milliseconds);
    void (*signal_init)(void *context);
    /* Runs qcom_iris_core_recover (with the lock held) after the delay, from a worker. */
    void (*schedule_recovery)(void *context, unsigned milliseconds);
    /* Runs qcom_iris_core_autosuspend (with the lock held) after the delay, from a worker; a later call replaces an earlier one. */
    void (*schedule_suspend)(void *context, unsigned milliseconds);
    uint64_t (*now_ms)(void *context);
    void (*log)(void *context, const char *text);           /* firmware debug strings and errors; optional */
};

struct qcom_iris_core {
    const struct qcom_iris_core_ops *ops;
    struct qcom_iris_vpu_io vpu;
    struct qcom_iris_queues queues;
    volatile uint32_t *sfr;
    uint32_t sfr_device_addr;
    unsigned num_vpp_pipe;
    enum qcom_iris_core_state state;
    bool powered, suspended;
    unsigned usage;                   /* runtime PM usage count */
    uint64_t last_busy_ms;
    uint32_t intr_status;
    uint32_t header_id, packet_id;
    uint32_t next_session_id;
    struct qcom_iris_session *sessions[QcomIrisMaxSessions];
    char version[128];
    uint32_t last_error;              /* the type of the last system error */
    unsigned recoveries;
    uint8_t message[QCOM_IRIS_CORE_PKT_SIZE];
    uint8_t command[512];
};

void qcom_iris_core_setup(struct qcom_iris_core *, const struct qcom_iris_core_ops *, const struct qcom_iris_vpu_io *,
                          const struct qcom_iris_queues *, volatile uint32_t *sfr, uint32_t sfr_device_addr, unsigned num_vpp_pipe);
/* iris_core_init with iris_wait_for_system_response: 0 once the firmware answered the init, the state otherwise. The caller holds
 * a get around it and puts after, as iris_open does, so an idle core power-collapses after the autosuspend delay. */
int qcom_iris_core_init(struct qcom_iris_core *);
void qcom_iris_core_deinit(struct qcom_iris_core *);
/* pm_runtime_resume_and_get / pm_runtime_put_autosuspend: a get resumes a suspended core. */
int qcom_iris_core_get(struct qcom_iris_core *);
void qcom_iris_core_put(struct qcom_iris_core *);
/* The autosuspend timer: suspends when unused and idle for QCOM_IRIS_AUTOSUSPEND_MS, else schedules itself again. */
void qcom_iris_core_autosuspend(struct qcom_iris_core *);
/* iris_hfi_queue_cmd_write (the session's send). */
int qcom_iris_core_send(struct qcom_iris_core *, const void *message, uint32_t size);
/* Session registry: a session id unique among the open ones, assigned before qcom_iris_session_init uses it. */
uint32_t qcom_iris_core_session_id(struct qcom_iris_core *);
int qcom_iris_core_add_session(struct qcom_iris_core *, struct qcom_iris_session *);
void qcom_iris_core_remove_session(struct qcom_iris_core *, struct qcom_iris_session *);
/* iris_hfi_isr_handler after the line was disabled: true when the caller enables the line again. */
bool qcom_iris_core_thread(struct qcom_iris_core *);
/* iris_sys_error_handler: deinit, then init again. */
void qcom_iris_core_recover(struct qcom_iris_core *);
#ifdef __cplusplus
}
#endif
#endif
