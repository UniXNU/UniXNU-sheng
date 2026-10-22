/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Shared memory point-to-point state (SMP2P): each side owns one SMEM item holding named 32-bit entries the other side watches; a change
 * is signalled with a doorbell. This is the item layout and the bookkeeping for both directions: our outbound item (entries we set bits
 * in) and the remote's inbound item (entries whose bit changes become events). Doorbells and SMEM placement belong to the caller. */
#ifndef QCOM_SMP2P_H
#define QCOM_SMP2P_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define QCOM_SMP2P_MAGIC 0x504d5324u  /* "$SMP" */
#define QCOM_SMP2P_VERSION 1u
#define QCOM_SMP2P_FEATURE_SSR_ACK 1u
#define QCOM_SMP2P_FLAG_RESTART_DONE 1u
#define QCOM_SMP2P_FLAG_RESTART_ACK 2u
#define QCOM_SMP2P_MAX_ENTRIES 16u
#define QCOM_SMP2P_ITEM_BYTES (20u + 20u * QCOM_SMP2P_MAX_ENTRIES)

/* The two items' storage: 32-bit aligned, possibly device memory. */
typedef struct {
    volatile uint32_t *out, *in;  /* in may be null until the remote has made it */
    uint16_t local_pid, remote_pid;
    uint8_t negotiated, ssr_ack_enabled, ssr_acked;
    uint32_t last[QCOM_SMP2P_MAX_ENTRIES];  /* inbound values last seen, by inbound entry index */
    uint32_t seen_entries;
} QcomSmp2p;

/* Writes a fresh outbound item (zeroes, header, then the version word last so the remote never sees a half-made item). */
void qcom_smp2p_init_out(QcomSmp2p *s, volatile uint32_t *out, uint16_t local_pid, uint16_t remote_pid);
/* Adds a named outbound entry and returns its index, or -1 when full. */
int qcom_smp2p_add_entry(QcomSmp2p *s, const char *name);
/* Sets the bits in mask of outbound entry index to value; returns 1 when the value changed (ring the doorbell). */
int qcom_smp2p_update(QcomSmp2p *s, int index, uint32_t mask, uint32_t value);
uint32_t qcom_smp2p_out_value(const QcomSmp2p *s, int index);

/* Validates an inbound item (magic, version, the pids seen from the other side); 0 when usable. */
int qcom_smp2p_check_in(const volatile uint32_t *in, uint16_t local_pid, uint16_t remote_pid);
/* Index of the named inbound entry, -1 when the remote has not added it. */
int qcom_smp2p_find_in(const QcomSmp2p *s, const char *name);
uint32_t qcom_smp2p_in_value(const QcomSmp2p *s, int index);

/* Called on every doorbell from the remote. Agrees on the features once, answers a restart with the restart ack flag, and reports the
 * bits of the named inbound entry that changed since the last call (changed) and its current value (value). Returns 1 when our item
 * changed so the remote has to be rung. */
int qcom_smp2p_service(QcomSmp2p *s, const char *entry, uint32_t *value, uint32_t *changed);

#ifdef __cplusplus
}
#endif
#endif
