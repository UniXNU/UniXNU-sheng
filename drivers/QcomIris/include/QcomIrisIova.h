/* SPDX-License-Identifier: GPL-2.0-only */
/* The VPU's device address space: first-fit allocation of 4 KiB-aligned ranges inside the window the firmware may address. The iris
 * platform data limits DMA to dma_mask 0xe0000000 - 1, and the content-protection layout (tz_cp_config_vpu3) reserves [0, 0x25800000)
 * for the firmware's secure and non-pixel regions, so allocations come from [0x25800000, 0xe0000000). Not thread safe. */
#ifndef QCOM_IRIS_IOVA_H
#define QCOM_IRIS_IOVA_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define QCOM_IRIS_IOVA_BASE 0x25800000u
#define QCOM_IRIS_IOVA_END 0xe0000000u
enum {QcomIrisIovaFreeMax = 1024};
struct qcom_iris_iova {
    struct {uint32_t start, end;} free[QcomIrisIovaFreeMax];   /* sorted, disjoint, never adjacent */
    unsigned count;
    uint64_t allocated;
};
void qcom_iris_iova_init(struct qcom_iris_iova *, uint32_t base, uint32_t end);
/* bytes rounded up to 4 KiB; align a power of two of at least 4 KiB. Returns 0 when nothing fits. */
uint32_t qcom_iris_iova_alloc(struct qcom_iris_iova *, uint64_t bytes, uint32_t align);
/* Returns false for a range not wholly allocated, or when the free list is full (the range then stays allocated). */
bool qcom_iris_iova_free(struct qcom_iris_iova *, uint32_t start, uint64_t bytes);
#ifdef __cplusplus
}
#endif
#endif
