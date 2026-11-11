/* SPDX-License-Identifier: GPL-2.0-only */
/* sources: src/IrisCore.c src/IrisSystem.c src/IrisHfi.c src/IrisVpu.c src/IrisSession.c src/IrisBuffers.c deps/linux/drivers/media/platform/qcom/iris/iris_vpu_buffer.c */
/* The core against a VPU register model and a firmware that reads the command queue and answers on the message queue: init in
 * mainline's order with the system answer, the image version and a debug string; a command with runtime PM; autosuspend early,
 * then power collapse; resume on the next command; a watchdog that fails the sessions and the recovery; a failed init; an init
 * nobody answers. */
#include "QcomIrisCore.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the VPU registers ---- */
static uint32_t regs[0xf0000 / 4];
static bool pc_ready;
static unsigned doorbells;
static uint32_t rd(void *c, uint32_t o)
{
    (void)c;
    if (o == 0xa004c) regs[o / 4] = (regs[0xa0048 / 4] ? 1u : 0u) | (pc_ready ? 0x100u : 0u);   /* CTRL_STATUS */
    if (o == 0xe0004 || o == 0xb0060) regs[o / 4] |= 1;
    if (o == 0x1f004) regs[o / 4] = regs[0x1f000 / 4] ? 3 : 0;
    if (o >= 0x70 && o < 0x80) regs[o / 4] = 0x400000;
    return regs[o / 4];
}
static void wr(void *c, uint32_t o, uint32_t v)
{
    (void)c;
    if (o == 0xa0150 && v == 1) ++doorbells;
    if (o == 0xa001c) regs[0xb000c / 4] = 0;   /* the doorbell clear empties the status */
    regs[o / 4] = v;
}
static void delay(void *c, unsigned us) {(void)c; (void)us;}
static const struct qcom_iris_vpu_io vpu = {NULL, rd, wr, delay};

/* ---- queue memory ---- */
static _Alignas(4096) uint8_t queue_memory[QCOM_IRIS_QUEUE_BYTES];
static uint32_t sfr[1024];
static const struct qcom_iris_queues queues = {queue_memory, 0x26000000};
static struct qcom_iris_queue_header *qhdr(unsigned q) {return &((struct qcom_iris_queue_table *)queue_memory)->q_hdr[q];}
static uint32_t *qdata(unsigned q) {return (uint32_t *)(queue_memory + sizeof(struct qcom_iris_queue_table) + q * QCOM_IRIS_IFACEQ_QUEUE_SIZE);}
static bool fw_read(uint8_t *out)
{
    struct qcom_iris_queue_header *h = qhdr(QcomIrisCmdQ);
    if (h->read_idx == h->write_idx) return false;
    const uint32_t size = qdata(QcomIrisCmdQ)[h->read_idx];
    memcpy(out, qdata(QcomIrisCmdQ) + h->read_idx, size);
    h->read_idx += size / 4;
    return true;
}
static void fw_write(unsigned q, const uint8_t *message, uint32_t size)
{
    struct qcom_iris_queue_header *h = qhdr(q);
    memcpy(qdata(q) + h->write_idx, message, size);
    h->write_idx += size / 4;
}

/* ---- the firmware ---- */
static enum {Answer, Refuse, Silent} mode;
static unsigned commands, inits, versions, ifpcs, pc_preps;
static struct qcom_iris_core core;
static void fw_run(void)
{
    uint8_t in[4096], out[4096];
    while (fw_read(in)) {
        ++commands;
        for (const struct qcom_iris_hfi_packet *p = qcom_iris_hfi_next(in, NULL); p; p = qcom_iris_hfi_next(in, p)) {
            struct qcom_iris_hfi_writer w;
            if (p->type == HFI_CMD_INIT) {
                ++inits;
                if (mode == Silent) continue;
                qcom_iris_hfi_begin(&w, out, sizeof out, 0, 0);
                qcom_iris_hfi_add(&w, HFI_CMD_INIT, mode == Answer ? HFI_FW_FLAGS_SUCCESS : 0, HFI_PAYLOAD_NONE, HFI_PORT_NONE, 0, NULL, 0);
                fw_write(QcomIrisMsgQ, out, qcom_iris_hfi_size(&w));
            } else if (p->type == HFI_PROP_IMAGE_VERSION) {
                ++versions;
                char version[128] = "video-firmware.3.1";
                qcom_iris_hfi_begin(&w, out, sizeof out, 0, 0);
                qcom_iris_hfi_add(&w, HFI_PROP_IMAGE_VERSION, 0, HFI_PAYLOAD_STRING, HFI_PORT_NONE, 0, version, sizeof version);
                fw_write(QcomIrisMsgQ, out, qcom_iris_hfi_size(&w));
                uint8_t debug[64] = {0};
                const uint32_t size = 16 + 1 + 12;
                memcpy(debug, &size, 4);
                memcpy(debug + 17, "fw: ready  ", 11);
                fw_write(QcomIrisDbgQ, debug, (size + 3) & ~3u);
            } else if (p->type == HFI_PROP_INTRA_FRAME_POWER_COLLAPSE) {
                ++ifpcs;
            } else if (p->type == HFI_CMD_POWER_COLLAPSE) {
                ++pc_preps;
            }
        }
    }
}
/* the firmware runs and interrupts; the interrupt thread runs */
static bool interrupt(uint32_t status)
{
    fw_run();
    regs[0xb000c / 4] = status;
    return qcom_iris_core_thread(&core);
}

/* ---- the platform ---- */
static char trace[512];
static void step(const char *s) {strcat(trace, s); strcat(trace, " ");}
static bool signalled, irq_enabled;
static int scheduled_suspend = -1, scheduled_recovery = -1;
static uint64_t now;
static char last_log[160];
static int ctrl_on(void *c) {(void)c; step("ctrl-on"); return 0;}
static int hw_on(void *c) {(void)c; step("hw-on"); return 0;}
static int hwmode(void *c) {(void)c; step("hwmode"); return 0;}
static void hw_off(void *c) {(void)c; step("hw-off");}
static void ctrl_off(void *c) {(void)c; step("ctrl-off");}
static int load(void *c) {(void)c; step("load"); return 0;}
static void unload(void *c) {(void)c; step("unload");}
static int remote(void *c, bool resume) {(void)c; step(resume ? "remote-on" : "remote-off"); return 0;}
static void enable_irq(void *c) {(void)c; assert(!irq_enabled); irq_enabled = true; step("irq-on");}
static void disable_irq(void *c) {(void)c; assert(irq_enabled); irq_enabled = false; step("irq-off");}
static bool wait_init(void *c, unsigned ms)
{
    (void)c;
    assert(ms == 1000);
    signalled = false;
    interrupt(1u << 2);
    return signalled;
}
static void signal_init(void *c) {(void)c; signalled = true;}
static void recovery(void *c, unsigned ms) {(void)c; scheduled_recovery = (int)ms;}
static void suspend_later(void *c, unsigned ms) {(void)c; scheduled_suspend = (int)ms;}
static uint64_t now_ms(void *c) {(void)c; return now;}
static void log_text(void *c, const char *t) {(void)c; snprintf(last_log, sizeof last_log, "%s", t);}
static const struct qcom_iris_core_ops ops = {NULL, ctrl_on, hw_on, hwmode, hw_off, ctrl_off, load, unload, remote, enable_irq, disable_irq,
                                              wait_init, signal_init, recovery, suspend_later, now_ms, log_text};

/* a session that only records its failure */
static unsigned failures;
static uint32_t failed_with;
static int s_send(void *c, const void *m, uint32_t n) {(void)c; return qcom_iris_core_send(&core, m, n);}
static void s_failed(void *c, struct qcom_iris_session *s, uint32_t e) {(void)c; (void)s; ++failures; failed_with = e;}
static const struct qcom_iris_session_ops session_ops = {.send = s_send, .failed = s_failed};
static struct qcom_iris_session session;

int main(void)
{
    qcom_iris_core_setup(&core, &ops, &vpu, &queues, sfr, 0x26400000, 4);
    assert(!qcom_iris_core_init(&core));
    assert(!strcmp(trace, "ctrl-on hw-on irq-on load hwmode "));
    assert(regs[0xa0054 / 4] == 0x26000000 && regs[0xa005c / 4] == 0x26400001 && regs[0xa0048 / 4] == 1);
    assert(core.state == QcomIrisCoreInit && commands == 3 && inits == 1 && versions == 1 && ifpcs == 1 && doorbells == 3);
    assert(!strncmp(core.version, "video-firmware.3.1", 18) && !strcmp(last_log, "fw: ready  "));
    assert(!qcom_iris_core_init(&core) && inits == 1);   /* already initialized */

    /* a session command: get and put, the autosuspend scheduled */
    const uint32_t id = qcom_iris_core_session_id(&core);
    qcom_iris_session_init(&session, &session_ops, &core.header_id, &core.packet_id, id, false, QcomIrisCodecH264, 4);
    assert(id == 1 && !qcom_iris_core_add_session(&core, &session) && qcom_iris_core_session_id(&core) == 2);
    now = 100;
    assert(!qcom_iris_session_open(&session) && !core.usage && scheduled_suspend == (int)QCOM_IRIS_AUTOSUSPEND_MS);
    fw_run();
    /* the timer runs early: rescheduled for the rest; then on time: power collapse */
    now = 1000;
    qcom_iris_core_autosuspend(&core);
    assert(scheduled_suspend == 600 && !core.suspended);
    now = 1600;
    trace[0] = 0;
    pc_ready = true;   /* the firmware reports itself ready for power collapse */
    qcom_iris_core_autosuspend(&core);
    assert(core.suspended && !core.powered && !irq_enabled && !strcmp(trace, "remote-off hw-off ctrl-off irq-off "));

    /* the next command resumes: power, remote state, boot, hardware mode, inter-frame power collapse, then the command */
    trace[0] = 0;
    pc_ready = false;
    const unsigned before = commands;
    uint8_t message[64];
    struct qcom_iris_hfi_writer w;
    qcom_iris_hfi_begin(&w, message, sizeof message, id, 99);
    qcom_iris_hfi_add(&w, HFI_CMD_RESUME, 0, HFI_PAYLOAD_NONE, HFI_PORT_RAW, 0, NULL, 0);
    assert(!qcom_iris_core_send(&core, message, qcom_iris_hfi_size(&w)));
    assert(!strncmp(trace, "ctrl-on hw-on irq-on remote-on hwmode ", 38) && core.powered && !core.suspended);
    fw_run();
    assert(ifpcs == 2 && commands >= before + 2);

    /* a message for no open session is dropped; the watchdog fails the session, leaves the line off and schedules recovery */
    qcom_iris_hfi_begin(&w, message, sizeof message, 77, 0);
    qcom_iris_hfi_add(&w, HFI_CMD_STOP, HFI_FW_FLAGS_SUCCESS, HFI_PAYLOAD_NONE, HFI_PORT_RAW, 0, NULL, 0);
    fw_write(QcomIrisMsgQ, message, qcom_iris_hfi_size(&w));
    assert(interrupt(1u << 2) && failures == 0);
    irq_enabled = false;   /* the primary handler disabled the line */
    assert(!interrupt(1u << 3) && core.state == QcomIrisCoreError && failures == 1 && failed_with == HFI_SYS_ERROR_WD_TIMEOUT);
    assert(scheduled_recovery == (int)QCOM_IRIS_RECOVERY_MS && qcom_iris_core_send(&core, message, 64) == -EINVAL);
    trace[0] = 0;
    regs[0xa0048 / 4] = 0;
    qcom_iris_core_recover(&core);
    assert(!strncmp(trace, "unload hw-off ctrl-off ctrl-on hw-on irq-on load hwmode ", 56) && core.state == QcomIrisCoreInit && inits == 2);
    qcom_iris_core_remove_session(&core, &session);

    /* the firmware refuses the init; then nobody answers */
    qcom_iris_core_deinit(&core);
    assert(core.state == QcomIrisCoreDeinit && !core.powered);
    mode = Refuse;
    assert(qcom_iris_core_init(&core) == -EIO && core.state == QcomIrisCoreError);
    assert(qcom_iris_core_init(&core) == -EINVAL);
    qcom_iris_core_deinit(&core);
    mode = Silent;
    assert(qcom_iris_core_init(&core) == -ETIMEDOUT && core.state == QcomIrisCoreError);
    qcom_iris_core_deinit(&core);
    printf("core: init, runtime suspend and resume, dispatch, watchdog recovery, refused and silent init: ok\n");
    return 0;
}
