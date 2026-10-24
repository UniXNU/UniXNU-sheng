/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomSmp2p.h"

/* Word offsets: 0 magic, 1 version (bits 7:0) and features (31:8), 2 local pid (15:0) and remote pid (31:16), 3 total entries (15:0)
 * and valid entries (31:16), 4 flags, then 5 words per entry: a 16-byte name and the value. */
#define W_VERSION 1
#define W_PIDS 2
#define W_COUNTS 3
#define W_FLAGS 4
#define W_ENTRY(i) (5 + 5 * (i))

static void barrier(void)
{
#if defined(__aarch64__) && defined(KERNEL)
    __asm__ volatile("dmb oshst" ::: "memory");
#endif
}
static int same_name(const volatile uint32_t *entry, const char *name)
{
    for (unsigned i = 0; i < 16; ++i) {
        const char c = (char)(entry[i / 4] >> (8 * (i % 4)));
        if (c != name[i]) return 0;
        if (!c) return 1;
    }
    return name[16] == 0;
}

void qcom_smp2p_init_out(QcomSmp2p *s, volatile uint32_t *out, uint16_t local_pid, uint16_t remote_pid)
{
    *s = (QcomSmp2p){0};
    s->out = out;
    s->local_pid = local_pid;
    s->remote_pid = remote_pid;
    for (unsigned i = 0; i < QCOM_SMP2P_ITEM_BYTES / 4; ++i) out[i] = 0;
    barrier();
    out[0] = QCOM_SMP2P_MAGIC;
    out[W_PIDS] = local_pid | (uint32_t)remote_pid << 16;
    out[W_COUNTS] = QCOM_SMP2P_MAX_ENTRIES;
    barrier();
    out[W_VERSION] = QCOM_SMP2P_VERSION | QCOM_SMP2P_FEATURE_SSR_ACK << 8;
    barrier();
}

int qcom_smp2p_add_entry(QcomSmp2p *s, const char *name)
{
    const uint32_t valid = s->out[W_COUNTS] >> 16;
    if (valid >= QCOM_SMP2P_MAX_ENTRIES) return -1;
    uint32_t words[4] = {0};
    for (unsigned i = 0; i < 16 && name[i]; ++i) words[i / 4] |= (uint32_t)(uint8_t)name[i] << (8 * (i % 4));
    for (unsigned i = 0; i < 4; ++i) s->out[W_ENTRY(valid) + i] = words[i];
    s->out[W_ENTRY(valid) + 4] = 0;
    barrier();
    s->out[W_COUNTS] = QCOM_SMP2P_MAX_ENTRIES | (valid + 1) << 16;
    barrier();
    return (int)valid;
}

int qcom_smp2p_update(QcomSmp2p *s, int index, uint32_t mask, uint32_t value)
{
    volatile uint32_t *v = &s->out[W_ENTRY(index) + 4];
    const uint32_t old = *v, next = (old & ~mask) | (value & mask);
    if (next == old) return 0;
    *v = next;
    barrier();
    return 1;
}

uint32_t qcom_smp2p_out_value(const QcomSmp2p *s, int index) {return s->out[W_ENTRY(index) + 4];}

int qcom_smp2p_check_in(const volatile uint32_t *in, uint16_t local_pid, uint16_t remote_pid)
{
    if (!in || in[0] != QCOM_SMP2P_MAGIC) return -1;
    if ((in[W_VERSION] & 0xff) != QCOM_SMP2P_VERSION) return -2;
    /* The remote writes its own pid as local. */
    if (in[W_PIDS] != (remote_pid | (uint32_t)local_pid << 16)) return -3;
    const uint32_t counts = in[W_COUNTS];
    if ((counts & 0xffff) > QCOM_SMP2P_MAX_ENTRIES || (counts >> 16) > (counts & 0xffff)) return -4;
    return 0;
}

int qcom_smp2p_find_in(const QcomSmp2p *s, const char *name)
{
    if (!s->in) return -1;
    const uint32_t valid = s->in[W_COUNTS] >> 16;
    for (uint32_t i = 0; i < valid && i < QCOM_SMP2P_MAX_ENTRIES; ++i)
        if (same_name(&s->in[W_ENTRY(i)], name)) return (int)i;
    return -1;
}

uint32_t qcom_smp2p_in_value(const QcomSmp2p *s, int index) {return s->in[W_ENTRY(index) + 4];}

int qcom_smp2p_service(QcomSmp2p *s, const char *entry, uint32_t *value, uint32_t *changed)
{
    int ring = 0;
    *value = 0;
    *changed = 0;
    if (qcom_smp2p_check_in(s->in, s->local_pid, s->remote_pid)) return 0;
    if (!s->negotiated) {
        const uint32_t ours = s->out[W_VERSION], theirs = s->in[W_VERSION];
        if ((ours & 0xff) == (theirs & 0xff)) {
            const uint32_t features = (ours >> 8) & (theirs >> 8);
            if (features != ours >> 8) {s->out[W_VERSION] = (ours & 0xff) | features << 8; barrier(); ring = 1;}
            s->ssr_ack_enabled = (features & QCOM_SMP2P_FEATURE_SSR_ACK) != 0;
            s->negotiated = 1;
        }
    }
    if (s->ssr_ack_enabled) {
        const uint8_t done = (s->in[W_FLAGS] & QCOM_SMP2P_FLAG_RESTART_DONE) != 0;
        if (done != s->ssr_acked) {
            s->ssr_acked = done;
            const uint32_t flags = s->out[W_FLAGS];
            s->out[W_FLAGS] = done ? flags | QCOM_SMP2P_FLAG_RESTART_ACK : flags & ~QCOM_SMP2P_FLAG_RESTART_ACK;
            barrier();
            ring = 1;
        }
    }
    const int index = qcom_smp2p_find_in(s, entry);
    if (index >= 0) {
        const uint32_t now = qcom_smp2p_in_value(s, index);
        *value = now;
        *changed = now ^ s->last[index];
        s->last[index] = now;
    }
    return ring;
}
