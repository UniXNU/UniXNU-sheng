/* SPDX-License-Identifier: GPL-2.0-only */
#include "QcomIrisCore.h"
#include "QcomIrisSystem.h"
#include <string.h>
#ifdef KERNEL
#include <sys/errno.h>
#else
#include <errno.h>
#endif

static void note(struct qcom_iris_core *c, const char *text) {if (c->ops->log) c->ops->log(c->ops->context, text);}

void qcom_iris_core_setup(struct qcom_iris_core *c, const struct qcom_iris_core_ops *ops, const struct qcom_iris_vpu_io *vpu,
                          const struct qcom_iris_queues *queues, volatile uint32_t *sfr, uint32_t sfr_device_addr, unsigned num_vpp_pipe)
{
    memset(c, 0, sizeof *c);
    c->ops = ops;
    c->vpu = *vpu;
    c->queues = *queues;
    c->sfr = sfr;
    c->sfr_device_addr = sfr_device_addr;
    c->num_vpp_pipe = num_vpp_pipe;
    c->state = QcomIrisCoreDeinit;
    c->next_session_id = 1;
}

/* iris_vpu_power_on: the clock rate is fixed by the platform (one level, never lowered) */
static int power_on(struct qcom_iris_core *c)
{
    int ret = c->ops->power_on_controller(c->ops->context);
    if (ret) return ret;
    ret = c->ops->power_on_hardware(c->ops->context);
    if (ret) {
        qcom_iris_vpu_power_off_controller(&c->vpu);
        c->ops->power_off_controller(c->ops->context);
        return ret;
    }
    qcom_iris_vpu_preset(&c->vpu);
    c->intr_status = 0;
    c->powered = true;
    c->ops->enable_irq(c->ops->context);
    return 0;
}

/* iris_vpu_power_off with the vpu3 register halves */
static void power_off(struct qcom_iris_core *c)
{
    if (!c->powered) return;
    qcom_iris_vpu3_power_off_hardware(&c->vpu, c->num_vpp_pipe);
    c->ops->power_off_hardware(c->ops->context);
    qcom_iris_vpu_power_off_controller(&c->vpu);
    c->ops->power_off_controller(c->ops->context);
    c->powered = false;
    if (!qcom_iris_vpu_watchdog(c->intr_status)) c->ops->disable_irq(c->ops->context);
}

static int write_locked(struct qcom_iris_core *c, const void *message, uint32_t size)
{
    if (c->state == QcomIrisCoreError || c->state == QcomIrisCoreDeinit) return -EINVAL;
    if (qcom_iris_queue_write(&c->queues, message, size)) {note(c, "iris: queue full"); return -ENODATA;}
    qcom_iris_vpu_raise(&c->vpu);
    return 0;
}

static int boot(struct qcom_iris_core *c)
{
    uint32_t status = 0;
    const int ret = qcom_iris_vpu_boot(&c->vpu, c->queues.device_addr, qcom_iris_uc_region_size(), c->sfr_device_addr, &status);
    if (ret) {note(c, ret == -1 ? "iris: firmware rejected the UC region" : "iris: firmware did not boot"); return -ETIMEDOUT;}
    return c->ops->set_hwmode(c->ops->context);
}

int qcom_iris_core_init(struct qcom_iris_core *c)
{
    if (c->state == QcomIrisCoreInit) return 0;
    if (c->state == QcomIrisCoreError) return -EINVAL;
    c->state = QcomIrisCoreInit;
    qcom_iris_queues_init(&c->queues, c->sfr);
    int ret = power_on(c);
    if (ret) goto error;
    ret = c->ops->load_firmware(c->ops->context);
    if (ret) goto power;
    ret = boot(c);
    if (ret) goto firmware;
    uint32_t n = qcom_iris_system_init(c->command, sizeof c->command, &c->header_id, &c->packet_id);
    ret = n ? write_locked(c, c->command, n) : -EINVAL;
    if (!ret) {
        n = qcom_iris_system_image_version(c->command, sizeof c->command, &c->header_id, &c->packet_id);
        ret = n ? write_locked(c, c->command, n) : -EINVAL;
    }
    if (!ret && c->debug_level) {
        n = qcom_iris_system_debug(c->command, sizeof c->command, &c->header_id, &c->packet_id, c->debug_level);
        ret = n ? write_locked(c, c->command, n) : -EINVAL;
    }
    if (!ret) {
        n = qcom_iris_system_interframe_powercollapse(c->command, sizeof c->command, &c->header_id, &c->packet_id);
        ret = n ? write_locked(c, c->command, n) : -EINVAL;
    }
    if (ret) goto firmware;
    c->suspended = false;
    c->last_busy_ms = c->ops->now_ms(c->ops->context);
    /* iris_wait_for_system_response, the lock dropped */
    if (!c->ops->wait_init(c->ops->context, QCOM_IRIS_RESPONSE_TIMEOUT_MS)) {
        c->state = QcomIrisCoreError;
        note(c, "iris: no answer to the system init");
        return -ETIMEDOUT;
    }
    return c->state == QcomIrisCoreInit ? 0 : -EIO;
firmware:
    c->ops->unload_firmware(c->ops->context);
power:
    power_off(c);
error:
    c->state = QcomIrisCoreDeinit;
    return ret;
}

void qcom_iris_core_deinit(struct qcom_iris_core *c)
{
    /* pm_runtime_resume_and_get first: the firmware is unloaded and the core powered off from the resumed state */
    const int resumed = c->suspended ? qcom_iris_core_get(c) : (c->usage++, 0);
    if (c->state != QcomIrisCoreDeinit) {
        c->ops->unload_firmware(c->ops->context);
        if (!resumed) power_off(c);
        c->state = QcomIrisCoreDeinit;
    }
    c->usage--;
    c->suspended = false;
}

/* iris_hfi_pm_suspend */
static int suspend(struct qcom_iris_core *c)
{
    switch (qcom_iris_vpu_pc_state(&c->vpu)) {
    case QcomIrisPcReady: break;
    case QcomIrisPcBusy: goto skip;
    case QcomIrisPcPrepare: {
        const uint32_t n = qcom_iris_system_pc_prep(c->command, sizeof c->command, &c->header_id, &c->packet_id);
        if (!n || write_locked(c, c->command, n) || !qcom_iris_vpu_pc_wait(&c->vpu)) goto skip;
        break;
    }
    }
    if (c->ops->set_remote_state(c->ops->context, false)) return -EIO;
    power_off(c);
    return 0;
skip:
    c->last_busy_ms = c->ops->now_ms(c->ops->context);
    note(c, "iris: skip power collapse");
    return -EAGAIN;
}

/* iris_hfi_pm_resume */
static int resume(struct qcom_iris_core *c)
{
    int ret = power_on(c);
    if (ret) return -EBUSY;
    if (c->ops->set_remote_state(c->ops->context, true)) goto off;
    if (boot(c)) goto hw;
    const uint32_t n = qcom_iris_system_interframe_powercollapse(c->command, sizeof c->command, &c->header_id, &c->packet_id);
    if (!n || write_locked(c, c->command, n)) goto hw;
    c->last_busy_ms = c->ops->now_ms(c->ops->context);
    return 0;
hw:
    c->ops->set_remote_state(c->ops->context, false);
off:
    power_off(c);
    note(c, "iris: resume failed");
    return -EBUSY;
}

int qcom_iris_core_get(struct qcom_iris_core *c)
{
    if (c->suspended) {
        /* iris_pm_resume does the work only for an initialized core */
        if (c->state == QcomIrisCoreInit) {
            const int ret = resume(c);
            if (ret) return ret;
        }
        c->suspended = false;
    }
    c->usage++;
    return 0;
}

void qcom_iris_core_put(struct qcom_iris_core *c)
{
    if (c->usage) c->usage--;
    c->last_busy_ms = c->ops->now_ms(c->ops->context);
    if (!c->usage) c->ops->schedule_suspend(c->ops->context, QCOM_IRIS_AUTOSUSPEND_MS);
}

void qcom_iris_core_autosuspend(struct qcom_iris_core *c)
{
    if (c->usage || c->suspended) return;
    const uint64_t now = c->ops->now_ms(c->ops->context), due = c->last_busy_ms + QCOM_IRIS_AUTOSUSPEND_MS;
    if (now < due) {c->ops->schedule_suspend(c->ops->context, (unsigned)(due - now)); return;}
    /* iris_pm_suspend: only an initialized core collapses; any other state suspends with nothing to do */
    if (c->state == QcomIrisCoreInit && c->powered) {
        const int ret = suspend(c);
        if (ret == -EAGAIN) {c->ops->schedule_suspend(c->ops->context, QCOM_IRIS_AUTOSUSPEND_MS); return;}
        if (ret) return;
    }
    c->suspended = true;
}

int qcom_iris_core_send(struct qcom_iris_core *c, const void *message, uint32_t size)
{
    int ret = qcom_iris_core_get(c);
    if (ret) return ret;
    ret = write_locked(c, message, size);
    qcom_iris_core_put(c);
    return ret;
}

uint32_t qcom_iris_core_session_id(struct qcom_iris_core *c)
{
    for (;;) {
        const uint32_t id = c->next_session_id++;
        if (!c->next_session_id) c->next_session_id = 1;
        bool used = !id;
        for (unsigned i = 0; i < QcomIrisMaxSessions && !used; ++i) used = c->sessions[i] && c->sessions[i]->session_id == id;
        if (!used) return id;
    }
}

int qcom_iris_core_add_session(struct qcom_iris_core *c, struct qcom_iris_session *s)
{
    for (unsigned i = 0; i < QcomIrisMaxSessions; ++i)
        if (!c->sessions[i]) {c->sessions[i] = s; return 0;}
    return -EBUSY;   /* iris_check_session_supported: max_session_count */
}

void qcom_iris_core_remove_session(struct qcom_iris_core *c, struct qcom_iris_session *s)
{
    for (unsigned i = 0; i < QcomIrisMaxSessions; ++i)
        if (c->sessions[i] == s) c->sessions[i] = NULL;
}

/* iris_hfi_gen2_handle_system_error: every session fails, recovery runs after the handler's delay */
static void system_error(struct qcom_iris_core *c, uint32_t type)
{
    c->last_error = type;
    c->state = QcomIrisCoreError;
    note(c, "iris: system error");
    for (unsigned i = 0; i < QcomIrisMaxSessions; ++i)
        if (c->sessions[i]) qcom_iris_session_fail(c->sessions[i], type);
    c->ops->schedule_recovery(c->ops->context, QCOM_IRIS_RECOVERY_MS);
}

static void handle(struct qcom_iris_core *c, uint32_t size)
{
    if (!qcom_iris_hfi_valid(c->message, size)) {system_error(c, 0); return;}
    const struct qcom_iris_hfi_header *h = (const struct qcom_iris_hfi_header *)c->message;
    if (h->session_id) {
        for (unsigned i = 0; i < QcomIrisMaxSessions; ++i)
            if (c->sessions[i] && c->sessions[i]->session_id == h->session_id) {qcom_iris_session_handle(c->sessions[i], c->message); return;}
        return;   /* iris_get_instance found none */
    }
    struct qcom_iris_system_event e;
    qcom_iris_system_handle(c->message, &e);
    if (e.version[0]) {memcpy(c->version, e.version, sizeof c->version); note(c, c->version);}
    if (e.error) {system_error(c, e.error_type); return;}
    /* iris_hfi_gen2_handle_system_init: a failed init leaves the core in error, the waiter is woken either way */
    if (e.init_failed) {c->state = QcomIrisCoreError; c->ops->signal_init(c->ops->context);}
    else if (e.init_done) c->ops->signal_init(c->ops->context);
}

/* iris_hfi_gen2_flush_debug_queue: each message's text after its header and one byte */
static void flush_debug(struct qcom_iris_core *c)
{
    int n;
    while ((n = qcom_iris_queue_read(&c->queues, QcomIrisDbgQ, c->message, sizeof c->message)) > 0) {
        const struct hfi_debug_header *h = (const struct hfi_debug_header *)c->message;
        if (h->size < sizeof *h || h->size >= sizeof c->message) continue;
        c->message[h->size] = 0;
        note(c, (const char *)c->message + sizeof *h + 1);
    }
}

bool qcom_iris_core_thread(struct qcom_iris_core *c)
{
    c->last_busy_ms = c->ops->now_ms(c->ops->context);
    if (!c->powered) return true;
    c->intr_status = qcom_iris_vpu_clear(&c->vpu);
    if (qcom_iris_vpu_watchdog(c->intr_status)) {
        note(c, "iris: cpu watchdog error received");
        system_error(c, HFI_SYS_ERROR_WD_TIMEOUT);
        return false;
    }
    int n;
    /* iris_hfi_queue_msg_read until it fails: empty, or a packet it dropped */
    while ((n = qcom_iris_queue_read(&c->queues, QcomIrisMsgQ, c->message, sizeof c->message)) > 0) handle(c, (uint32_t)n);
    flush_debug(c);
    return true;
}

void qcom_iris_core_recover(struct qcom_iris_core *c)
{
    c->recoveries++;
    qcom_iris_core_deinit(c);
    qcom_iris_core_init(c);
}
