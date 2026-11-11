/* SPDX-License-Identifier: GPL-2.0-only */
#include "QcomIrisMmu.h"
#include <string.h>
#ifdef KERNEL
#include <sys/errno.h>
#else
#include <errno.h>
#endif

static const ArmSmmu4KRange attributes = {0, 0, 0, ArmSmmu4KRead | ArmSmmu4KWrite, 1, 3};

static int status_errno(ArmSmmu4KStatus s)
{
    switch (s) {
    case ArmSmmu4KSuccess: return 0;
    case ArmSmmu4KCapacity: case ArmSmmu4KBudget: return -ENOMEM;
    case ArmSmmu4KTimeout: return -ETIMEDOUT;
    case ArmSmmu4KBusy: return -EBUSY;
    case ArmSmmu4KIoError: case ArmSmmu4KFault: case ArmSmmu4KStateMismatch: return -EIO;
    default: return -EINVAL;
    }
}

/* Room for want ranges in an array of capacity; doubles, keeps the contents. */
static bool reserve(struct qcom_iris_mmu *m, ArmSmmu4KRange **array, size_t *capacity, size_t used, size_t want)
{
    if (want <= *capacity) return true;
    size_t size = *capacity ? *capacity : 64;
    while (size < want) size *= 2;
    ArmSmmu4KRange *made = m->memory.allocate(m->memory.context, size * sizeof *made);
    if (!made) return false;
    if (used) memcpy(made, *array, used * sizeof *made);
    if (*array) m->memory.release(m->memory.context, *array, *capacity * sizeof **array);
    *array = made;
    *capacity = size;
    return true;
}

int qcom_iris_mmu_prepare(struct qcom_iris_mmu *m, const struct qcom_iris_mmu_memory *memory, const ArmSmmu4KTablePages *pages,
                          const ArmSmmu4KLiveVisibility *visibility, unsigned output_bits)
{
    if (!memory || !memory->allocate || !memory->release || !pages || !visibility || !visibility->publish_table_page) return -EINVAL;
    memset(m, 0, sizeof *m);
    m->memory = *memory;
    m->visibility = *visibility;
    uint64_t root = 0;
    volatile uint64_t *mapping = NULL;
    if (pages->allocate(pages->cookie, &root, &mapping)) return -ENOMEM;
    m->arena = (ArmSmmu4KLiveArena){{32, output_bits, root, QCOM_IRIS_IOVA_END / 4096}, 0, QCOM_IRIS_IOVA_END, 0, pages, NULL, 0};
    ArmSmmu4KStatus s = ArmSmmu4KLivePrepare(&m->arena, &m->state);
    ArmSmmu4KLiveResult audit;
    if (s == ArmSmmu4KSuccess) s = ArmSmmu4KLiveAudit(&m->arena, &m->state, NULL, 0, &audit);
    if (s == ArmSmmu4KSuccess && visibility->publish_table_page(visibility->cookie, mapping, 4096)) s = ArmSmmu4KIoError;
    m->last_status = s;
    if (s != ArmSmmu4KSuccess) {m->poisoned = true; return status_errno(s);}
    /* Table walks are not cache coherent on this SMMU (the board's other apps SMMU domains walk outer non-cacheable). */
    m->context = (ArmSmmu4KContextConfig){.lower = &m->arena.config, .lower_tables = &m->state.tables, .asid = 0,
                                          .coherent_walk = 0, .stall_on_fault = 0, .continue_after_fault = 1};
    qcom_iris_iova_init(&m->iova, QCOM_IRIS_IOVA_BASE, QCOM_IRIS_IOVA_END);
    return 0;
}

int qcom_iris_mmu_program(struct qcom_iris_mmu *m, const ArmSmmu4KIo *io, unsigned bank, unsigned asid, unsigned poll_budget)
{
    if (!io || m->programmed || m->poisoned || !m->state.prepared || asid > 0xffff || !poll_budget) return -EINVAL;
    m->context.asid = asid;
    ArmSmmu4KProgramResult r;
    const ArmSmmu4KStatus s = ArmSmmu4KProgramDisabledContext(io, bank, &m->context, poll_budget, &r);
    m->last_status = s;
    if (s != ArmSmmu4KSuccess) {m->poisoned = true; return status_errno(s);}
    m->io = *io;
    m->bank = bank;
    m->poll_budget = poll_budget;
    m->programmed = true;
    return 0;
}

static size_t lower_bound(const struct qcom_iris_mmu *m, uint64_t iova)
{
    size_t low = 0, high = m->count;
    while (low < high) {
        const size_t mid = low + (high - low) / 2;
        if (m->ranges[mid].iova < iova) low = mid + 1;
        else high = mid;
    }
    return low;
}

static ArmSmmu4KStatus update(struct qcom_iris_mmu *m, size_t count, unsigned map)
{
    const ArmSmmu4KLiveRegion lower = {&m->arena, &m->state, m->ranges, m->count};
    ArmSmmu4KLiveResult r;
    return map ? ArmSmmu4KLiveMapContextBatchExclusive(&m->io, m->bank, &m->context, &lower, NULL, 0, &m->visibility, m->changes, count,
                                                       m->poll_budget, &r)
               : ArmSmmu4KLiveUpdateContextBatch(&m->io, m->bank, &m->context, &lower, NULL, 0, &m->visibility, m->changes, count, 0,
                                                 m->poll_budget, &r);
}

int qcom_iris_mmu_map(struct qcom_iris_mmu *m, const struct qcom_iris_segment *segments, size_t count, uint32_t align, uint32_t *iova)
{
    if (!m->programmed || m->poisoned) return -EIO;
    if (!segments || !count || !iova) return -EINVAL;
    uint64_t total = 0;
    size_t runs = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!segments[i].bytes || ((segments[i].physical | segments[i].bytes) & 4095) || segments[i].bytes > QCOM_IRIS_IOVA_END) return -EINVAL;
        /* physically adjacent segments become one range */
        if (!i || segments[i - 1].physical + segments[i - 1].bytes != segments[i].physical) ++runs;
        total += segments[i].bytes;
        if (total > QCOM_IRIS_IOVA_END) return -EINVAL;
    }
    if (!reserve(m, &m->changes, &m->change_capacity, 0, runs) || !reserve(m, &m->ranges, &m->capacity, m->count, m->count + runs))
        return -ENOMEM;
    const uint32_t base = qcom_iris_iova_alloc(&m->iova, total, align);
    if (!base) return -ENOMEM;
    size_t n = 0;
    uint64_t at = base;
    for (size_t i = 0; i < count; ++i) {
        if (n && m->changes[n - 1].physical + m->changes[n - 1].bytes == segments[i].physical) {
            m->changes[n - 1].bytes += segments[i].bytes;
        } else {
            m->changes[n] = attributes;
            m->changes[n].iova = at;
            m->changes[n].physical = segments[i].physical;
            m->changes[n].bytes = segments[i].bytes;
            ++n;
        }
        at += segments[i].bytes;
    }
    const ArmSmmu4KStatus s = update(m, n, 1);
    m->last_status = s;
    if (s != ArmSmmu4KSuccess) {
        /* A refusal before any store (a table page that could not be had) leaves the domain consistent; anything else poisoned it,
         * and the device range stays allocated with whatever the table holds. */
        if (m->state.poisoned) m->poisoned = true;
        else qcom_iris_iova_free(&m->iova, base, total);
        return status_errno(s);
    }
    /* The new ranges are contiguous in IOVA and lie in one gap of the sorted list. */
    const size_t at_index = lower_bound(m, base);
    memmove(m->ranges + at_index + n, m->ranges + at_index, (m->count - at_index) * sizeof *m->ranges);
    memcpy(m->ranges + at_index, m->changes, n * sizeof *m->ranges);
    m->count += n;
    *iova = base;
    return 0;
}

int qcom_iris_mmu_unmap(struct qcom_iris_mmu *m, uint32_t iova, uint64_t bytes)
{
    if (!m->programmed || m->poisoned) return -EIO;
    const size_t first = lower_bound(m, iova);
    size_t last = first;
    uint64_t covered = 0;
    while (last < m->count && m->ranges[last].iova == (uint64_t)iova + covered && covered < bytes) covered += m->ranges[last++].bytes;
    if (last == first || covered != bytes) return -EINVAL;
    const size_t n = last - first;
    if (!reserve(m, &m->changes, &m->change_capacity, 0, n)) return -ENOMEM;
    memcpy(m->changes, m->ranges + first, n * sizeof *m->changes);
    const ArmSmmu4KStatus s = update(m, n, 0);
    m->last_status = s;
    if (s != ArmSmmu4KSuccess) {m->poisoned = true; return status_errno(s);}
    memmove(m->ranges + first, m->ranges + last, (m->count - last) * sizeof *m->ranges);
    m->count -= n;
    qcom_iris_iova_free(&m->iova, iova, bytes);
    return 0;
}

int qcom_iris_mmu_disable(struct qcom_iris_mmu *m)
{
    if (!m->programmed) return -EINVAL;
    ArmSmmu4KProgramResult r;
    const ArmSmmu4KStatus s = ArmSmmu4KDisableOwnedContext(&m->io, m->bank, &m->context, m->poll_budget, &r);
    m->last_status = s;
    if (s != ArmSmmu4KSuccess) {m->poisoned = true; return status_errno(s);}
    m->programmed = false;
    return 0;
}

void qcom_iris_mmu_free(struct qcom_iris_mmu *m)
{
    if (m->ranges) m->memory.release(m->memory.context, m->ranges, m->capacity * sizeof *m->ranges);
    if (m->changes) m->memory.release(m->memory.context, m->changes, m->change_capacity * sizeof *m->changes);
    m->ranges = m->changes = NULL;
    m->count = m->capacity = m->change_capacity = 0;
}
