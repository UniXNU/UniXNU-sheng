/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * UCSI over Qualcomm PMIC GLINK, one connector: what the port manager says is on the other end of the cable. The order of commands and
 * the message layout follow Linux drivers/usb/typec/ucsi/ucsi.c and ucsi_glink.c. No I/O and no waits: the owner hands in each message
 * and tick, and sends what comes back in out. */
#ifndef QCOM_UCSI_H
#define QCOM_UCSI_H
#include <stdint.h>
#include <string.h>
#define QCOM_UCSI_OWNER 32779u
#define QCOM_UCSI_V2_BUFFER 528u
#define QCOM_UCSI_MAX_MESSAGE (12u + QCOM_UCSI_V2_BUFFER + 4u)
enum {kUcsiOpRead = 0x11, kUcsiOpWrite = 0x12, kUcsiOpNotify = 0x13};
enum {kUcsiOffVersion = 0, kUcsiOffCci = 4, kUcsiOffControl = 8, kUcsiOffMessageIn = 16};
enum : uint32_t {kUcsiCciResetComplete = 1u << 27, kUcsiCciBusy = 1u << 28, kUcsiCciAckComplete = 1u << 29, kUcsiCciError = 1u << 30,
                 kUcsiCciCommandComplete = 1u << 31};
/* ucsi.h: PPM_RESET, ACK_CC_CI, SET_NOTIFICATION_ENABLE with UCSI_ENABLE_NTFY_ALL, GET_CONNECTOR_STATUS of connector 1. */
static const uint64_t kUcsiReset = 0x01, kUcsiClearReset = 0x05, kUcsiEnableAll = 0x05ull | 0xdbe70000ull,
                      kUcsiStatus = 0x12ull | 1ull << 16, kUcsiAckCommand = 0x04ull | 1ull << 17, kUcsiAckChange = 0x04ull | 1ull << 16;
enum QcomUcsiState {
    kUcsiIdleOff, kUcsiFirstRead, kUcsiClearing, kUcsiResetting, kUcsiEnabling, kUcsiAckEnable, kUcsiAskStatus, kUcsiAckStatus, kUcsiAckChangeSent, kUcsiReady, kUcsiFailed
};
enum {kUcsiPartnerDfp = 1, kUcsiPartnerUfp = 2, kUcsiPartnerCableUfp = 4};
enum {kUcsiPollMs = 20, kUcsiPollsPerCommand = 250};   /* UCSI_TIMEOUT_MS is 10 s in Linux; 5 s here, as its GLINK wait */
struct QcomUcsi {
    int state;
    uint32_t buffer_bytes, version, cci, status, polls, polling, changes, errors;
    int change_pending, change_read;   /* a connector change seen; its status was read and the change waits for its acknowledge */
    int connected, partner, usb, status_valid;   /* the last GET_CONNECTOR_STATUS */
};
static inline void ucsi_put32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}
static inline uint32_t ucsi_get32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static inline uint32_t ucsi_read_request(uint8_t *out) {
    ucsi_put32(out, QCOM_UCSI_OWNER); ucsi_put32(out + 4, 1); ucsi_put32(out + 8, kUcsiOpRead);
    return 12;
}
static inline uint32_t ucsi_write_request(const struct QcomUcsi *u, uint8_t *out, uint64_t control) {
    memset(out, 0, 12 + u->buffer_bytes + 4);
    ucsi_put32(out, QCOM_UCSI_OWNER); ucsi_put32(out + 4, 1); ucsi_put32(out + 8, kUcsiOpWrite);
    ucsi_put32(out + 12 + kUcsiOffControl, (uint32_t)control);
    ucsi_put32(out + 12 + kUcsiOffControl + 4, (uint32_t)(control >> 32));
    return 12 + u->buffer_bytes + 4;
}
/* The channel came up: ask for the buffer. Returns the bytes to send. */
static inline uint32_t ucsi_start(struct QcomUcsi *u, uint8_t *out) {
    memset(u, 0, sizeof *u);
    u->state = kUcsiFirstRead;
    return ucsi_read_request(out);
}
static inline uint32_t ucsi_command(struct QcomUcsi *u, int state, uint8_t *out, uint64_t control) {
    u->state = state; u->polls = 0; u->polling = 0;
    return ucsi_write_request(u, out, control);
}
/* A complete read answer. */
static inline uint32_t ucsi_advance(struct QcomUcsi *u, uint8_t *out, const uint8_t *buffer) {
    const uint32_t cci = ucsi_get32(buffer + kUcsiOffCci);
    u->cci = cci;
    if (!u->version) u->version = ucsi_get32(buffer + kUcsiOffVersion) & 0xffffu;
    /* ucsi_notify_common: the busy indicator makes the rest of the CCI bogus; a connector number reports a change. */
    if (cci & kUcsiCciBusy) {u->polling = u->state != kUcsiReady; return 0;}
    if (((cci >> 1) & 0x7fu) && !u->change_read && u->state >= kUcsiAskStatus) u->change_pending = 1;
    switch (u->state) {
    case kUcsiFirstRead:
        /* ucsi_reset_ppm: a latched reset completion is cleared first. */
        if (cci & kUcsiCciResetComplete) return ucsi_command(u, kUcsiClearing, out, kUcsiClearReset);
        return ucsi_command(u, kUcsiResetting, out, kUcsiReset);
    case kUcsiClearing:
        if (cci & kUcsiCciCommandComplete) return ucsi_command(u, kUcsiResetting, out, kUcsiReset);
        break;
    case kUcsiResetting:
        if (cci & kUcsiCciResetComplete) return ucsi_command(u, kUcsiEnabling, out, kUcsiEnableAll);
        break;
    case kUcsiEnabling:
        if (cci & (kUcsiCciCommandComplete | kUcsiCciError)) return ucsi_command(u, kUcsiAckEnable, out, kUcsiAckCommand);
        break;
    case kUcsiAckEnable:
        if (cci & kUcsiCciAckComplete) return ucsi_command(u, kUcsiAskStatus, out, kUcsiStatus);
        break;
    case kUcsiAskStatus:
        if (cci & kUcsiCciError) {u->errors++; return ucsi_command(u, kUcsiAckStatus, out, kUcsiAckCommand);}
        if (cci & kUcsiCciCommandComplete) {
            /* UCSI_CONSTAT: connected bit 19, partner flag USB bit 21, partner type bits 31:29. */
            const uint32_t status = ucsi_get32(buffer + kUcsiOffMessageIn);
            u->status = status;
            u->connected = (status >> 19) & 1;
            u->usb = (status >> 21) & 1;
            u->partner = (int)(status >> 29);
            u->status_valid = 1;
            return ucsi_command(u, kUcsiAckStatus, out, kUcsiAckCommand);
        }
        break;
    case kUcsiAckStatus:
        if (cci & kUcsiCciAckComplete) {
            /* ucsi_handle_connector_change: the status first, then the acknowledge of the change. */
            if (u->change_read) {u->change_read = 0; u->changes++; return ucsi_command(u, kUcsiAckChangeSent, out, kUcsiAckChange);}
            if (u->change_pending) {u->change_pending = 0; u->change_read = 1; return ucsi_command(u, kUcsiAskStatus, out, kUcsiStatus);}
            u->state = kUcsiReady; u->polling = 0;
            return 0;
        }
        break;
    case kUcsiAckChangeSent:
        if (cci & kUcsiCciAckComplete) {
            if (u->change_pending) {u->change_pending = 0; u->change_read = 1; return ucsi_command(u, kUcsiAskStatus, out, kUcsiStatus);}
            u->state = kUcsiReady; u->polling = 0;
            return 0;
        }
        break;
    case kUcsiReady:
        if (u->change_pending) {u->change_pending = 0; u->change_read = 1; return ucsi_command(u, kUcsiAskStatus, out, kUcsiStatus);}
        return 0;
    default:
        return 0;
    }
    u->polling = 1;   /* the state did not move: the tick reads again */
    return 0;
}
/* A message of the USB-C owner. Returns the bytes to send. */
static inline uint32_t ucsi_receive(struct QcomUcsi *u, const uint8_t *data, uint32_t length, uint8_t *out) {
    if (length < 12 || ucsi_get32(data) != QCOM_UCSI_OWNER || u->state == kUcsiIdleOff || u->state == kUcsiFailed) return 0;
    const uint32_t opcode = ucsi_get32(data + 8);
    if (opcode == kUcsiOpNotify) return ucsi_read_request(out);   /* pmic_glink_ucsi_notify: the indication says only to read the CCI */
    if (opcode == kUcsiOpWrite) return ucsi_read_request(out);   /* written: read how the command went */
    if (opcode != kUcsiOpRead || length < 16 + 48) return 0;
    const uint32_t bytes = length - 16;
    if (bytes != 48 && bytes != QCOM_UCSI_V2_BUFFER) return 0;
    u->buffer_bytes = bytes;
    return ucsi_advance(u, out, data + 12);
}
/* Every kUcsiPollMs while polling is set. Returns the bytes to send. */
static inline uint32_t ucsi_tick(struct QcomUcsi *u, uint8_t *out) {
    if (!u->polling) return 0;
    if (++u->polls > kUcsiPollsPerCommand) {u->state = kUcsiFailed; u->polling = 0; return 0;}
    u->polling = 0;
    return ucsi_read_request(out);
}
#endif
