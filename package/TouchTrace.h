// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SHENG_TOUCH_TRACE_H
#define SHENG_TOUCH_TRACE_H
#include <stdint.h>

// TPR1 records use microseconds on the kernel uptime clock. Displacements
// are relative to the contact's first sample (0.1 panel px, Y down).
enum { TouchTraceCapacity = 16384 };
enum { TouchBegin = 1, TouchEnd = 2, TouchHeld = 4, TouchVisible = 8, TouchReport = 16 };
struct touch_trace_row {
    uint64_t irq_us, read_us, report_us;
    int32_t hid_result;
    uint16_t counter;
    uint8_t id, flags;
    int16_t raw_dx, raw_dy, out_dx, out_dy;
    uint16_t peer_distance; // nearest other observed centroid, Chebyshev distance; UINT16_MAX if none
    uint8_t count, origin_edges, miss_reason, merges, hid_begins, hid_edge_begins;
};
#endif
