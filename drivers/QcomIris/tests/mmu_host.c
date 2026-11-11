/* SPDX-License-Identifier: GPL-2.0-only */
/* sources: src/IrisMmu.c src/IrisIova.c kit:ArmSmmu4K.c kit:ArmSmmu4KContext.c kit:ArmSmmu4KProgram.c kit:ArmSmmu4KLive.c */
/* The VPU domain against a register model of one SMMUv2 context bank and a host page pool: prepare, program, a scattered buffer
 * mapped as one device range (adjacent pages merged), a second aligned buffer, the walk of both, an unmap that clears its leaves and
 * gives its range back, refused unmaps, a table page the system refuses (nothing poisoned) and the disable. */
#include "QcomIrisMmu.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POOL 32
#define POOL_BASE 0x10000000ull
static _Alignas(4096) uint64_t pool[POOL * 512];
static size_t used, refuse_at = SIZE_MAX;
static int page_allocate(void *cookie, uint64_t *physical, volatile uint64_t **mapping)
{
    (void)cookie;
    if (used == POOL || used == refuse_at) return 1;
    memset(pool + used * 512, 0, 4096);
    *physical = POOL_BASE + used * 4096;
    *mapping = pool + used * 512;
    ++used;
    return 0;
}
static volatile uint64_t *page_find(void *cookie, uint64_t physical)
{
    (void)cookie;
    if (physical < POOL_BASE || (physical & 4095) || (physical - POOL_BASE) / 4096 >= used) return NULL;
    return pool + (physical - POOL_BASE) / 8;
}
static int page_overlaps(void *cookie, uint64_t physical, uint64_t bytes)
{(void)cookie; return physical < POOL_BASE + used * 4096 && POOL_BASE < physical + bytes;}
static const ArmSmmu4KTablePages pages = {NULL, page_allocate, page_find, page_overlaps};
static unsigned published;
static int publish(void *cookie, volatile uint64_t *page, size_t bytes) {(void)cookie; (void)page; assert(bytes == 4096); ++published; return 0;}
static const ArmSmmu4KLiveVisibility visibility = {NULL, publish};

#define MMIO 0x100000   /* the SM8550 apps SMMU window (sm8550.dtsi reg size) */
static uint8_t regs[MMIO];
static const size_t cb = 0x80000;   /* bank 0: 128 register pages of 4 KiB before the banks */
static int r32(void *c, size_t at, uint32_t *v) {(void)c; memcpy(v, regs + at, 4); return 0;}
static int r64(void *c, size_t at, uint64_t *v) {(void)c; memcpy(v, regs + at, 8); return 0;}
static int w32(void *c, size_t at, uint32_t v)
{
    (void)c;
    if (at == cb + 0x58) {uint32_t old; memcpy(&old, regs + at, 4); v = old & ~v;}   /* FSR: write one to clear */
    memcpy(regs + at, &v, 4);
    return 0;
}
static int w64(void *c, size_t at, uint64_t v) {(void)c; memcpy(regs + at, &v, 8); return 0;}
static int barrier(void *c) {(void)c; return 0;}
static const ArmSmmu4KIo io = {NULL, MMIO, r32, r64, w32, w64, barrier};

static void *host_allocate(void *c, size_t bytes) {(void)c; return malloc(bytes);}
static void host_release(void *c, void *p, size_t bytes) {(void)c; (void)bytes; free(p);}
static const struct qcom_iris_mmu_memory memory = {NULL, host_allocate, host_release};

/* An independent walk: 32-bit input starts at level 1 (four entries), then levels 2 and 3. */
static uint64_t walk(const struct qcom_iris_mmu *m, uint64_t va)
{
    uint64_t table = m->state.tables.root_physical;
    for (unsigned level = 1; level <= 3; ++level) {
        const unsigned shift = 39 - 9 * level;
        const volatile uint64_t *t = page_find(NULL, table);
        assert(t);
        const uint64_t d = t[(va >> shift) & 511];
        if (!(d & 1)) return 0;
        if (level == 3) {assert((d & 3) == 3); return d;}
        assert((d & 3) == 3);
        table = d & 0x0000fffffffff000ull;
    }
    return 0;
}
static struct qcom_iris_mmu m;
int main(void)
{
    uint32_t v = 1u << 30; memcpy(regs + 0x20, &v, 4);      /* ID0: stage 1 */
    v = 0x60000050; memcpy(regs + 0x24, &v, 4);             /* ID1 as on SM8550: 4 KiB register pages, 128 of them, 80 banks */
    v = 0x1555; memcpy(regs + 0x28, &v, 4);                 /* ID2: 48-bit sizes */
    assert(!qcom_iris_mmu_prepare(&m, &memory, &pages, &visibility, 48) && used == 1 && published == 1);
    assert(!qcom_iris_mmu_program(&m, &io, 0, 0x21, 64) && m.programmed);
    uint32_t sctlr; memcpy(&sctlr, regs + cb, 4);
    assert(sctlr & 1);

    const struct qcom_iris_segment a[3] = {{0x80000000, 4096}, {0x80001000, 8192}, {0x90000000, 4096}};
    uint32_t ia = 0, ib = 0;
    assert(!qcom_iris_mmu_map(&m, a, 3, 4096, &ia) && ia == QCOM_IRIS_IOVA_BASE && m.count == 2);
    for (unsigned p = 0; p < 3; ++p) assert((walk(&m, ia + p * 4096) & 0x0000fffffffff000ull) == 0x80000000ull + p * 4096);
    assert((walk(&m, ia + 3 * 4096) & 0x0000fffffffff000ull) == 0x90000000ull && !walk(&m, ia + 4 * 4096));
    const struct qcom_iris_segment b[1] = {{0x88000000, 3 << 20}};
    assert(!qcom_iris_mmu_map(&m, b, 1, 1 << 20, &ib) && ib == 0x25900000 && m.count == 3);
    assert((walk(&m, ib + (2 << 20) + 4096) & 0x0000fffffffff000ull) == 0x88000000ull + (2 << 20) + 4096);
    assert(m.ranges[0].iova == ia && m.ranges[2].iova == ib);

    assert(qcom_iris_mmu_unmap(&m, ia, 8192) == -EINVAL && qcom_iris_mmu_unmap(&m, ia + 4096, 12288) == -EINVAL);
    assert(!qcom_iris_mmu_unmap(&m, ia, 16384) && m.count == 1 && !walk(&m, ia) && !walk(&m, ia + 3 * 4096));
    assert(qcom_iris_mmu_unmap(&m, ia, 16384) == -EINVAL && !m.poisoned);

    /* A map that needs a new leaf table the system cannot give: refused before any store, nothing poisoned. */
    refuse_at = used;
    const struct qcom_iris_segment c[1] = {{0xa0000000, 4096}};
    uint32_t ic = 0;
    const uint64_t allocated = m.iova.allocated;
    assert(qcom_iris_mmu_map(&m, c, 1, 1u << 30, &ic) == -ENOMEM && !m.poisoned && m.iova.allocated == allocated);
    refuse_at = SIZE_MAX;
    assert(!qcom_iris_mmu_map(&m, a, 3, 4096, &ia) && ia == QCOM_IRIS_IOVA_BASE);
    assert(!qcom_iris_mmu_unmap(&m, ia, 16384) && !qcom_iris_mmu_unmap(&m, ib, 3 << 20) && !m.count && !m.iova.allocated);
    assert(!qcom_iris_mmu_disable(&m));
    memcpy(&sctlr, regs + cb, 4);
    assert(!(sctlr & 1));
    qcom_iris_mmu_free(&m);
    /* The architectural window is 2 x 128 pages: a map of only the implemented banks (0xd0000) is refused, and that domain stays unusable. */
    static struct qcom_iris_mmu small_domain;
    ArmSmmu4KIo small = io;
    small.register_bytes = 0xd0000;
    assert(!qcom_iris_mmu_prepare(&small_domain, &memory, &pages, &visibility, 48));
    assert(qcom_iris_mmu_program(&small_domain, &small, 0, 0x21, 64) == -22 && small_domain.last_status == ArmSmmu4KUnsupported);
    assert(qcom_iris_mmu_program(&small_domain, &io, 0, 0x21, 64) == -22);
    printf("mmu: prepare, program, scattered and aligned maps, walk, unmap, refusals, table page refused, disable: ok\n");
    return 0;
}
