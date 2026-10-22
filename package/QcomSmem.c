/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomSmem.h"

#define TOC_MAGIC 0x434f5424u   /* "$TOC" */
#define PART_MAGIC 0x54525024u  /* "$PRT" */
#define CANARY 0xa5a5u
#define TOC_HEADER 32u
#define TOC_ENTRY 48u
#define PART_HEADER 32u
#define ENTRY 16u

uint32_t qcom_smem_read32(const QcomSmem *smem, uint32_t offset) {return *(volatile uint32_t *)(smem->base + offset);}
void qcom_smem_write32(const QcomSmem *smem, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(smem->base + offset) = value;
#if defined(__aarch64__) && defined(KERNEL)
    __asm__ volatile("dmb oshst" ::: "memory");
#endif
}
static uint32_t rd(const QcomSmem *s, uint32_t at) {return qcom_smem_read32(s, at);}
static int fits(uint64_t at, uint64_t length, uint64_t size) {return at <= size && length <= size - at;}
static uint32_t align_up(uint32_t value, uint32_t to) {return (value + to - 1) & ~(to - 1);}

int qcom_smem_partition(const QcomSmem *smem, uint16_t host, uint16_t remote, QcomSmemPartition *out)
{
    if (!smem || !smem->base || smem->size < 0x2000 || (smem->size & 0xfff)) return QcomSmemNoTable;
    const uint32_t toc = smem->size - 0x1000;
    if (rd(smem, toc) != TOC_MAGIC || rd(smem, toc + 4) != 1) return QcomSmemNoTable;
    const uint32_t count = rd(smem, toc + 8);
    if (count > (0x1000 - TOC_HEADER) / TOC_ENTRY) return QcomSmemNoTable;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e = toc + TOC_HEADER + i * TOC_ENTRY;
        const uint32_t offset = rd(smem, e), size = rd(smem, e + 4), hosts = rd(smem, e + 12), cacheline = rd(smem, e + 16);
        const uint16_t h0 = (uint16_t)hosts, h1 = (uint16_t)(hosts >> 16);
        if (!offset || !size) continue;
        if (!((h0 == host && h1 == remote) || (h0 == remote && h1 == host))) continue;
        if (!fits(offset, size, toc) || size < PART_HEADER + 2 * ENTRY || (offset & 7)) return QcomSmemBadPartition;
        if (cacheline & (cacheline - 1)) return QcomSmemBadPartition;
        if (rd(smem, offset) != PART_MAGIC || rd(smem, offset + 4) != hosts || rd(smem, offset + 8) != size) return QcomSmemBadPartition;
        const uint32_t free_uncached = rd(smem, offset + 12), free_cached = rd(smem, offset + 16);
        if (free_uncached < PART_HEADER || free_uncached > free_cached || free_cached > size) return QcomSmemBadPartition;
        *out = (QcomSmemPartition){offset, size, cacheline ? cacheline : 8u, h0, h1};
        return QcomSmemOk;
    }
    return QcomSmemNoPartition;
}

/* Walks the uncached list; returns the offset (relative to the partition) where it ends, or the item's entry when found. */
static int walk_uncached(const QcomSmem *smem, const QcomSmemPartition *p, uint16_t item, uint32_t *at_out, int *found)
{
    const uint32_t end = rd(smem, p->offset + 12);
    uint32_t at = PART_HEADER;
    *found = 0;
    while (at < end) {
        if (!fits(at, ENTRY, end)) return QcomSmemCorrupt;
        const uint32_t w0 = rd(smem, p->offset + at), size = rd(smem, p->offset + at + 4), pads = rd(smem, p->offset + at + 8);
        if ((w0 & 0xffff) != CANARY) return QcomSmemCorrupt;
        const uint32_t padding_header = pads >> 16;
        if ((uint64_t)at + ENTRY + padding_header + size > end) return QcomSmemCorrupt;
        if ((w0 >> 16) == item) {*found = 1; *at_out = at; return QcomSmemOk;}
        at += ENTRY + padding_header + size;
    }
    if (at != end) return QcomSmemCorrupt;
    *at_out = at;
    return QcomSmemOk;
}

int qcom_smem_find(const QcomSmem *smem, const QcomSmemPartition *p, uint16_t item, uint32_t *offset, uint32_t *size)
{
    uint32_t at;
    int found, rc = walk_uncached(smem, p, item, &at, &found);
    if (rc) return rc;
    if (found) {
        const uint32_t bytes = rd(smem, p->offset + at + 4), pads = rd(smem, p->offset + at + 8);
        if ((pads & 0xffff) > bytes) return QcomSmemCorrupt;
        *offset = p->offset + at + ENTRY + (pads >> 16);
        *size = bytes - (pads & 0xffff);
        return QcomSmemOk;
    }
    /* Cached items: each entry sits at the end of its cache line slot, its data right below it. */
    const uint32_t header = align_up(ENTRY, p->cacheline), last = rd(smem, p->offset + 16);
    if (p->size < header) return QcomSmemCorrupt;
    uint32_t e = p->size - header;
    while (e > last) {
        const uint32_t w0 = rd(smem, p->offset + e), bytes = rd(smem, p->offset + e + 4), pads = rd(smem, p->offset + e + 8);
        if ((w0 & 0xffff) != CANARY || bytes > e || (pads & 0xffff) > bytes) return QcomSmemCorrupt;
        if ((w0 >> 16) == item) {*offset = p->offset + e - bytes; *size = bytes - (pads & 0xffff); return QcomSmemOk;}
        if (e < bytes + header) return QcomSmemCorrupt;
        e -= bytes + header;
    }
    return QcomSmemMissing;
}

int qcom_smem_alloc(const QcomSmem *smem, const QcomSmemPartition *p, uint16_t item, uint32_t size)
{
    uint32_t at;
    int found, rc = walk_uncached(smem, p, item, &at, &found);
    if (rc) return rc;
    if (found) return QcomSmemExists;
    uint32_t offset, bytes;
    if (qcom_smem_find(smem, p, item, &offset, &bytes) == QcomSmemOk) return QcomSmemExists;
    const uint32_t aligned = align_up(size, 8), need = ENTRY + aligned;
    if (!size || aligned < size || (uint64_t)at + need > rd(smem, p->offset + 16)) return QcomSmemNoSpace;
    qcom_smem_write32(smem, p->offset + at, CANARY | (uint32_t)item << 16);
    qcom_smem_write32(smem, p->offset + at + 4, aligned);
    qcom_smem_write32(smem, p->offset + at + 8, aligned - size);  /* padding_data, padding_header 0 */
    qcom_smem_write32(smem, p->offset + at + 12, 0);
    for (uint32_t i = 0; i < aligned; i += 4) qcom_smem_write32(smem, p->offset + at + ENTRY + i, 0);
    /* The entry is complete before the free mark moves over it. */
    qcom_smem_write32(smem, p->offset + 12, at + need);
    return QcomSmemOk;
}
