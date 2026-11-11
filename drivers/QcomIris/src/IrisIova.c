/* SPDX-License-Identifier: GPL-2.0-only */
#include "QcomIrisIova.h"

static uint64_t round_page(uint64_t bytes) {return (bytes + 4095) & ~(uint64_t)4095;}

void qcom_iris_iova_init(struct qcom_iris_iova *a, uint32_t base, uint32_t end)
{
    a->count = 0;
    a->allocated = 0;
    base = (uint32_t)round_page(base);
    end &= ~4095u;
    if (base < end) {a->free[0].start = base; a->free[0].end = end; a->count = 1;}
}

uint32_t qcom_iris_iova_alloc(struct qcom_iris_iova *a, uint64_t bytes, uint32_t align)
{
    bytes = round_page(bytes);
    if (!bytes || bytes > 0xffffffffu || align < 4096 || (align & (align - 1))) return 0;
    for (unsigned i = 0; i < a->count; ++i) {
        const uint64_t start = ((uint64_t)a->free[i].start + align - 1) & ~(uint64_t)(align - 1);
        if (start + bytes > a->free[i].end) continue;
        const uint32_t s = (uint32_t)start, e = (uint32_t)(start + bytes);
        const bool head = s > a->free[i].start, tail = e < a->free[i].end;
        if (head && tail) {
            if (a->count == QcomIrisIovaFreeMax) continue;   /* a split needs an entry */
            for (unsigned k = a->count; k > i + 1; --k) a->free[k] = a->free[k - 1];
            a->free[i + 1].start = e;
            a->free[i + 1].end = a->free[i].end;
            a->free[i].end = s;
            a->count++;
        } else if (head) {
            a->free[i].end = s;
        } else if (tail) {
            a->free[i].start = e;
        } else {
            for (unsigned k = i; k + 1 < a->count; ++k) a->free[k] = a->free[k + 1];
            a->count--;
        }
        a->allocated += bytes;
        return s;
    }
    return 0;
}

bool qcom_iris_iova_free(struct qcom_iris_iova *a, uint32_t start, uint64_t bytes)
{
    bytes = round_page(bytes);
    if (!bytes || (start & 4095) || (uint64_t)start + bytes > 0x100000000ull) return false;
    const uint32_t end = (uint32_t)(start + bytes);
    unsigned i = 0;
    while (i < a->count && a->free[i].end <= start) ++i;   /* the first free range ending after start */
    if (i < a->count && a->free[i].start < end) return false;   /* overlaps free space: not allocated */
    const bool join_prev = i > 0 && a->free[i - 1].end == start, join_next = i < a->count && a->free[i].start == end;
    if (join_prev && join_next) {
        a->free[i - 1].end = a->free[i].end;
        for (unsigned k = i; k + 1 < a->count; ++k) a->free[k] = a->free[k + 1];
        a->count--;
    } else if (join_prev) {
        a->free[i - 1].end = end;
    } else if (join_next) {
        a->free[i].start = start;
    } else {
        if (a->count == QcomIrisIovaFreeMax) return false;
        for (unsigned k = a->count; k > i; --k) a->free[k] = a->free[k - 1];
        a->free[i].start = start;
        a->free[i].end = end;
        a->count++;
    }
    a->allocated -= bytes;
    return true;
}
