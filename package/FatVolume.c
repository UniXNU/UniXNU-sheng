/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "FatVolume.h"

static uint16_t le16(const uint8_t *p) {return (uint16_t)(p[0] | (uint16_t)p[1] << 8);}
static uint32_t le32(const uint8_t *p) {return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16;}
static int fits(uint64_t at, uint64_t length, uint64_t size) {return at <= size && length <= size - at;}

static int vread(FatVolume *v, uint64_t offset, void *to, size_t length)
{
    if (!fits(offset, length, v->size)) return FatDamaged;
    return v->read(v->context, offset, to, length) ? FatReadError : FatOk;
}

int fat_mount(FatVolume *v, uint64_t size, FatRead read, void *context)
{
    uint8_t b[512];
    if (!v || !read) return FatReadError;
    *v = (FatVolume){0};
    v->read = read; v->context = context; v->size = size;
    v->cached_sector = UINT64_MAX;
    if (size < sizeof b) return FatNotFat;
    if (vread(v, 0, b, sizeof b)) return FatReadError;
    const uint32_t bps = le16(b + 11), spc = b[13], reserved = le16(b + 14), fats = b[16], root = le16(b + 17);
    const uint32_t total = le16(b + 19) ? le16(b + 19) : le32(b + 32);
    const uint32_t fat_sectors = le16(b + 22) ? le16(b + 22) : le32(b + 36);
    if (b[510] != 0x55 || b[511] != 0xaa) return FatNotFat;
    if (bps < 512 || bps > sizeof v->sector || (bps & (bps - 1)) || !spc || (spc & (spc - 1)) || !reserved || !fats || !fat_sectors)
        return FatNotFat;
    const uint64_t root_sectors = ((uint64_t)root * 32 + bps - 1) / bps;
    const uint64_t data_sector = reserved + (uint64_t)fats * fat_sectors + root_sectors;
    if (total <= data_sector || (uint64_t)total * bps > size) return FatNotFat;
    const uint64_t clusters = (total - data_sector) / spc;
    if (clusters < 4085) return FatNotFat;  /* FAT12 */
    v->fat32 = clusters >= 65525;
    if (v->fat32 ? root != 0 : root == 0) return FatNotFat;
    /* the table must hold an entry for every cluster */
    if ((uint64_t)fat_sectors * bps / (v->fat32 ? 4 : 2) < clusters + 2 || clusters > 0x0ffffff0) return FatNotFat;
    v->sector_bytes = bps;
    v->cluster_bytes = bps * spc;
    v->clusters = (uint32_t)clusters;
    v->fat_offset = (uint64_t)reserved * bps;
    v->root_offset = (reserved + (uint64_t)fats * fat_sectors) * bps;
    v->data_offset = data_sector * bps;
    v->root_entries = root;
    v->root_cluster = v->fat32 ? le32(b + 44) & 0x0fffffff : 0;
    return FatOk;
}

static int valid(const FatVolume *v, uint32_t cluster) {return cluster >= 2 && cluster - 2 < v->clusters;}
static uint64_t cluster_offset(const FatVolume *v, uint32_t cluster) {return v->data_offset + (uint64_t)(cluster - 2) * v->cluster_bytes;}

/* The table entry of cluster (valid), through the one-sector cache. */
static int next_cluster(FatVolume *v, uint32_t cluster, uint32_t *next)
{
    const uint64_t at = v->fat_offset + (uint64_t)cluster * (v->fat32 ? 4 : 2);
    const uint64_t sector = at / v->sector_bytes;
    if (sector != v->cached_sector) {
        v->cached_sector = UINT64_MAX;
        const int rc = vread(v, sector * v->sector_bytes, v->sector, v->sector_bytes);
        if (rc) return rc;
        v->cached_sector = sector;
    }
    const uint8_t *e = v->sector + at % v->sector_bytes;
    *next = v->fat32 ? le32(e) & 0x0fffffff : le16(e);
    return FatOk;
}

static uint8_t upper(char c) {return (uint8_t)(c >= 'a' && c <= 'z' ? c - 32 : c);}

/* The 11-byte directory form of one path component; 0 when it is no 8.3 name. */
static int short_name(const char *name, size_t length, uint8_t out[11])
{
    size_t dot = length;
    for (size_t i = 0; i < length; ++i) if (name[i] == '.') {if (dot != length) return 0; dot = i;}
    const size_t ext = dot < length ? length - dot - 1 : 0;
    if (!dot || dot > 8 || ext > 3 || (dot < length && !ext)) return 0;
    for (unsigned i = 0; i < 11; ++i) out[i] = ' ';
    for (size_t i = 0; i < dot; ++i) out[i] = upper(name[i]);
    for (size_t i = 0; i < ext; ++i) out[8 + i] = upper(name[dot + 1 + i]);
    return 1;
}

/* Long names: up to 20 entries of 13 UCS-2 characters each, before the short entry they belong to (checked by its checksum). */
#define LFN_ENTRIES 20u
static const uint8_t lfn_slots[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
typedef struct {uint16_t text[LFN_ENTRIES * 13]; uint8_t valid, next, sum, count;} LongName;

static uint8_t short_sum(const uint8_t *d)
{
    uint8_t sum = 0;
    for (unsigned i = 0; i < 11; ++i) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + d[i]);
    return sum;
}

static void long_part(LongName *l, const uint8_t *d)
{
    const unsigned seq = d[0] & 0x1f;
    if (d[0] & 0x40) {  /* the last part comes first */
        l->valid = seq >= 1 && seq <= LFN_ENTRIES;
        l->count = (uint8_t)seq;
        l->sum = d[13];
    } else if (!l->valid || seq != l->next - 1u || d[13] != l->sum) {
        l->valid = 0;
    }
    if (!l->valid) return;
    l->next = (uint8_t)seq;
    for (unsigned k = 0; k < 13; ++k) l->text[(seq - 1) * 13 + k] = le16(d + lfn_slots[k]);
}

static int long_matches(const LongName *l, const uint8_t *d, const char *name, size_t length)
{
    if (!l->valid || l->next != 1 || short_sum(d) != l->sum) return 0;
    size_t n = 0;
    while (n < (size_t)l->count * 13 && l->text[n]) ++n;
    if (n != length) return 0;
    for (size_t i = 0; i < n; ++i) if (l->text[i] > 0x7f || upper((char)l->text[i]) != upper(name[i])) return 0;
    return 1;
}

/* Looks for a name in a directory (the FAT16 fixed root for cluster 0, else a cluster chain): its long name, or its short name. */
static int find(FatVolume *v, uint32_t cluster, const char *name, size_t length, FatFile *file)
{
    uint8_t chunk[512], want[11];
    LongName l;
    l.valid = 0;
    const int has_short = short_name(name, length, want);
    const int fixed = cluster == 0;
    if (fixed && v->fat32) cluster = v->root_cluster;
    uint64_t at = fixed && !v->fat32 ? v->root_offset : 0;
    uint64_t left = fixed && !v->fat32 ? (uint64_t)v->root_entries * 32 : 0;
    for (uint32_t walked = 0;;) {
        if (!(fixed && !v->fat32)) {
            if (!valid(v, cluster) || walked++ > v->clusters) return FatDamaged;
            at = cluster_offset(v, cluster);
            left = v->cluster_bytes;
        }
        while (left) {
            const size_t n = left < sizeof chunk ? (size_t)left : sizeof chunk;
            const int rc = vread(v, at, chunk, n);
            if (rc) return rc;
            for (size_t e = 0; e + 32 <= n; e += 32) {
                const uint8_t *d = chunk + e;
                if (!d[0]) return FatNotFound;
                if (d[0] == 0xe5) {l.valid = 0; continue;}
                if ((d[11] & 0x3f) == 0x0f) {long_part(&l, d); continue;}
                int same = long_matches(&l, d, name, length);
                l.valid = 0;
                if (d[11] & 0x08) continue;  /* the volume label */
                for (unsigned i = 0; i < 11 && has_short && !same; ++i) {
                    if (upper((char)d[i]) != want[i]) break;
                    if (i == 10) same = 1;
                }
                if (!same) continue;
                file->first_cluster = (uint32_t)le16(d + 26) | (v->fat32 ? (uint32_t)le16(d + 20) << 16 : 0);
                file->size = le32(d + 28);
                file->directory = (d[11] & 0x10) != 0;
                return FatOk;
            }
            at += n;
            left -= n;
        }
        if (fixed && !v->fat32) return FatNotFound;
        uint32_t next;
        const int rc = next_cluster(v, cluster, &next);
        if (rc) return rc;
        if (!valid(v, next)) return FatNotFound;  /* the end of the directory */
        cluster = next;
    }
}

int fat_open(FatVolume *v, const char *path, FatFile *file)
{
    FatFile at = {0, 0, 1};  /* the root */
    if (!v || !path || !file) return FatNotFound;
    while (*path) {
        while (*path == '/') ++path;
        size_t n = 0;
        while (path[n] && path[n] != '/') ++n;
        if (!n) break;
        if (!at.directory) return FatNotDirectory;
        const int rc = find(v, at.first_cluster, path, n, &at);
        if (rc) return rc;
        path += n;
    }
    *file = at;
    return FatOk;
}

int fat_read(FatVolume *v, const FatFile *file, uint64_t offset, void *to, size_t length)
{
    uint8_t *out = to;
    if (!v || !file || !to) return FatReadError;
    if (file->directory) return FatIsDirectory;
    if (!fits(offset, length, file->size)) return FatRange;
    if (!length) return FatOk;
    uint32_t cluster = file->first_cluster, walked = 0;
    for (uint64_t skip = offset / v->cluster_bytes; skip; --skip) {
        if (!valid(v, cluster) || ++walked > v->clusters) return FatDamaged;
        const int rc = next_cluster(v, cluster, &cluster);
        if (rc) return rc;
    }
    uint64_t within = offset % v->cluster_bytes;
    while (length) {
        if (!valid(v, cluster)) return FatDamaged;
        /* a run of consecutive clusters is one read */
        uint32_t last = cluster, next = 0;
        uint64_t span = v->cluster_bytes - within;
        for (;;) {
            if (++walked > v->clusters) return FatDamaged;
            const int rc = next_cluster(v, last, &next);
            if (rc) return rc;
            if (span >= length || next != last + 1 || !valid(v, next)) break;
            last = next;
            span += v->cluster_bytes;
        }
        const size_t take = span < length ? (size_t)span : length;
        const int rc = vread(v, cluster_offset(v, cluster) + within, out, take);
        if (rc) return rc;
        out += take;
        length -= take;
        within = 0;
        cluster = next;
    }
    return FatOk;
}
