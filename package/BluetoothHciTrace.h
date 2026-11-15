/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SHENG_BLUETOOTH_HCI_TRACE_H
#define SHENG_BLUETOOTH_HCI_TRACE_H
#include <stdint.h>
#include <stddef.h>
#ifdef KERNEL
#include <sys/systm.h>
#else
#include <stdio.h>
#include <string.h>
#endif

enum { BtHciTraceEntries = 256, BtHciTraceBytes = 258, BtHciTraceReserve = 16384 };
struct bt_hci_record {
    uint64_t sequence, time_ns;
    uint16_t length;
    uint8_t type, tx, bytes[BtHciTraceBytes];
};
struct bt_hci_trace {
    uint32_t lock;
    uint64_t head, dropped;
    struct bt_hci_record records[BtHciTraceEntries];
};

/* A diagnostic reader or a panic must never stall the UART executor. */
static inline int bt_hci_lock(struct bt_hci_trace *trace) {
    return !__atomic_exchange_n(&trace->lock, 1u, __ATOMIC_ACQUIRE);
}
static inline void bt_hci_unlock(struct bt_hci_trace *trace) {
    __atomic_store_n(&trace->lock, 0u, __ATOMIC_RELEASE);
}
static inline void bt_hci_record(struct bt_hci_trace *trace, uint64_t time_ns,
                                 int tx, uint8_t type, const uint8_t *bytes, size_t length) {
    if (!trace) return;
    if (!bytes || length > BtHciTraceBytes || !bt_hci_lock(trace)) {
        __atomic_fetch_add(&trace->dropped, 1ull, __ATOMIC_RELAXED);
        return;
    }
    if (trace->head >= UINT64_MAX - 1) {
        __atomic_fetch_add(&trace->dropped, 1ull, __ATOMIC_RELAXED);
        bt_hci_unlock(trace);
        return;
    }
    uint64_t sequence = ++trace->head;
    struct bt_hci_record *record = &trace->records[(sequence - 1) % BtHciTraceEntries];
    record->sequence = sequence;
    record->time_ns = time_ns;
    record->tx = !!tx;
    record->type = type;
    record->length = (uint16_t)length;
    memcpy(record->bytes, bytes, length);
    bt_hci_unlock(trace);
}

/* Advance a tentative cursor only for complete rows. The caller commits it
 * after the slot, header and storage sync succeed. UINT64_MAX selects the
 * newest sixteen records for a panic snapshot. */
static inline size_t bt_hci_dump(struct bt_hci_trace *trace, char *out, size_t capacity,
                                 uint64_t *cursor) {
    if (!trace || capacity < 192 || !bt_hci_lock(trace)) return 0;
    uint64_t head = trace->head;
    uint64_t oldest = head > BtHciTraceEntries ? head - BtHciTraceEntries + 1 : 1;
    uint64_t next = *cursor == UINT64_MAX ? (head > 16 ? head - 15 : 1) : *cursor + 1;
    uint64_t missed = next < oldest ? oldest - next : 0;
    if (next < oldest) next = oldest;
    uint64_t dropped = __atomic_load_n(&trace->dropped, __ATOMIC_RELAXED);
    bt_hci_unlock(trace);
    int n = snprintf(out, capacity,
        "[bluetooth hci] enabled=1 head=%llu next=%llu missed=%llu dropped=%llu\n",
        (unsigned long long)head, (unsigned long long)next,
        (unsigned long long)missed, (unsigned long long)dropped);
    if (n < 0 || (size_t)n >= capacity) return 0;
    size_t used = (size_t)n;
    if (missed) *cursor = next - 1;
    for (; next <= head; ++next) {
        struct bt_hci_record record;
        if (!bt_hci_lock(trace)) break;
        record = trace->records[(next - 1) % BtHciTraceEntries];
        bt_hci_unlock(trace);
        if (record.sequence != next || record.length > BtHciTraceBytes) break;
        char line[640];
        n = snprintf(line, sizeof line,
            "[bluetooth hci] seq=%llu ns=%llu %s h4=%02x bytes=%u data=",
            (unsigned long long)record.sequence, (unsigned long long)record.time_ns,
            record.tx ? "TX_UART" : "RX_CONTROLLER", record.type, record.length);
        if (n < 0 || (size_t)n >= sizeof line) break;
        size_t length = (size_t)n;
        static const char hex[] = "0123456789abcdef";
        for (unsigned i = 0; i < record.length; ++i) {
            line[length++] = hex[record.bytes[i] >> 4];
            line[length++] = hex[record.bytes[i] & 15];
        }
        line[length++] = '\n';
        if (length >= capacity - used) break;
        memcpy(out + used, line, length);
        used += length;
        *cursor = next;
    }
    out[used] = 0;
    return used;
}
#endif
