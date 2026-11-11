/* SPDX-License-Identifier: GPL-2.0-only */
/* The VPU's translating domain on the apps SMMU: one live ArmSmmu4K table (TTBR0, 32-bit input) whose device addresses come from
 * QcomIrisIova, and the sorted list of every mapped range that each live update is given. As the iris driver's dma_map and
 * dma_alloc do through the IOMMU DMA layer, a buffer of scattered pages becomes one contiguous device range, mapped and unmapped in
 * one table transaction. Every range has the same attributes (read and write, normal write-back, inner shareable, unprivileged), so
 * two mappings of one page never conflict; the map takes the kit's exclusive variant on that ground. Serialized by the caller. */
#ifndef QCOM_IRIS_MMU_H
#define QCOM_IRIS_MMU_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ArmSmmu4KLive.h"
#include "QcomIrisIova.h"
#ifdef __cplusplus
extern "C" {
#endif
struct qcom_iris_mmu_memory {
    void *context;
    void *(*allocate)(void *context, size_t bytes);              /* NULL on failure */
    void (*release)(void *context, void *memory, size_t bytes);
};
struct qcom_iris_segment {uint64_t physical, bytes;};           /* 4 KiB aligned */
struct qcom_iris_mmu {
    struct qcom_iris_mmu_memory memory;
    ArmSmmu4KLiveArena arena;
    ArmSmmu4KLiveState state;
    ArmSmmu4KLiveVisibility visibility;
    ArmSmmu4KContextConfig context;
    ArmSmmu4KIo io;
    unsigned bank, poll_budget;
    bool programmed, poisoned;
    ArmSmmu4KRange *ranges;                                      /* every mapped range, sorted by IOVA */
    size_t count, capacity;
    ArmSmmu4KRange *changes;                                     /* one transaction's ranges */
    size_t change_capacity;
    ArmSmmu4KStatus last_status;
    struct qcom_iris_iova iova;
};
/* Takes the root table from pages, prepares and audits the empty arena over [0, QCOM_IRIS_IOVA_END) and publishes the root.
 * output_bits: the SMMU's output address size (ID2.OAS). Errno-style results. */
int qcom_iris_mmu_prepare(struct qcom_iris_mmu *, const struct qcom_iris_mmu_memory *, const ArmSmmu4KTablePages *,
                          const ArmSmmu4KLiveVisibility *, unsigned output_bits);
/* Programs and enables the disabled context bank (the caller picked it with its ASID and routes the streams afterwards). */
int qcom_iris_mmu_program(struct qcom_iris_mmu *, const ArmSmmu4KIo *, unsigned bank, unsigned asid, unsigned poll_budget);
/* Maps the segments, in order, at one fresh device range aligned to align (a power of two, at least 4 KiB). */
int qcom_iris_mmu_map(struct qcom_iris_mmu *, const struct qcom_iris_segment *, size_t count, uint32_t align, uint32_t *iova);
/* Unmaps a whole range qcom_iris_mmu_map returned (its address and the total bytes mapped). */
int qcom_iris_mmu_unmap(struct qcom_iris_mmu *, uint32_t iova, uint64_t bytes);
/* Disables the bank (the streams are unrouted first); the table pages stay the caller's until it frees them. */
int qcom_iris_mmu_disable(struct qcom_iris_mmu *);
void qcom_iris_mmu_free(struct qcom_iris_mmu *);
#ifdef __cplusplus
}
#endif
#endif
