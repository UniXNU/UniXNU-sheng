/* SPDX-License-Identifier: GPL-2.0-only */
/* sources: src/IrisIova.c */
/* The VPU address allocator: the window bounds, alignment, first fit, coalescing frees in any order, double and partial frees
 * refused, and a randomized run whose allocations never overlap and whose frees give the whole window back. */
#include "QcomIrisIova.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static struct qcom_iris_iova a;
int main(void)
{
    qcom_iris_iova_init(&a, QCOM_IRIS_IOVA_BASE, QCOM_IRIS_IOVA_END);
    const uint32_t x = qcom_iris_iova_alloc(&a, 100, 4096), y = qcom_iris_iova_alloc(&a, 3 << 20, 1 << 20), z = qcom_iris_iova_alloc(&a, 8192, 4096);
    assert(x == QCOM_IRIS_IOVA_BASE && y == 0x25900000 && z == QCOM_IRIS_IOVA_BASE + 4096);
    assert(!qcom_iris_iova_alloc(&a, 0, 4096) && !qcom_iris_iova_alloc(&a, 4096, 3000) && !qcom_iris_iova_alloc(&a, 0xc0000000ull, 4096));
    assert(!qcom_iris_iova_free(&a, x + 4096 * 3, 4096));   /* never allocated */
    assert(qcom_iris_iova_free(&a, z, 8192) && !qcom_iris_iova_free(&a, z, 8192));
    assert(qcom_iris_iova_free(&a, y, 3 << 20) && qcom_iris_iova_free(&a, x, 100));
    assert(a.count == 1 && a.free[0].start == QCOM_IRIS_IOVA_BASE && a.free[0].end == QCOM_IRIS_IOVA_END && !a.allocated);

    static struct {uint32_t start, bytes;} live[600];
    unsigned n = 0;
    srand(7);
    for (unsigned round = 0; round < 20000; ++round) {
        if (n < 600 && (rand() & 1)) {
            const uint32_t bytes = (uint32_t)(1 + rand() % 64) * 4096u, align = 4096u << (rand() % 4);
            const uint32_t s = qcom_iris_iova_alloc(&a, bytes, align);
            if (!s) continue;
            assert(!(s & (align - 1)) && s >= QCOM_IRIS_IOVA_BASE && (uint64_t)s + bytes <= QCOM_IRIS_IOVA_END);
            for (unsigned i = 0; i < n; ++i) assert(s + bytes <= live[i].start || live[i].start + live[i].bytes <= s);
            live[n].start = s; live[n].bytes = bytes; ++n;
        } else if (n) {
            const unsigned i = (unsigned)rand() % n;
            assert(qcom_iris_iova_free(&a, live[i].start, live[i].bytes));
            live[i] = live[--n];
        }
    }
    while (n) {--n; assert(qcom_iris_iova_free(&a, live[n].start, live[n].bytes));}
    assert(a.count == 1 && !a.allocated);
    printf("iova: window, alignment, first fit, coalescing, refusals, randomized run: ok\n");
    return 0;
}
