// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the firmware partition reader: FatVolume over a volume image made by an independent tool, and the split image view of
// QcomMdt over the pieces found in it.
//   modem-fw-host VOLUME.img image/NAME JOINED.mbn [image/FRAGMENTED EXPECTED]
// reads image/NAME.b00.. from the volume, compares the joined view (whole, in odd chunks, and across piece edges) with JOINED.mbn, which
// the runner made on its own, and parses it as firmware; optionally reads a fragmented file and compares it too.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../FatVolume.h"
#include "../QcomMdt.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

static uint8_t *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(*size ? *size : 1);
    CHECK(data && fread(data, 1, *size, f) == *size);
    fclose(f);
    return data;
}

typedef struct {const uint8_t *data; size_t size; unsigned reads;} Disk;
static int disk_read(void *context, uint64_t offset, void *to, size_t length)
{
    Disk *d = context;
    d->reads++;
    if (offset > d->size || length > d->size - offset) return -1;
    memcpy(to, d->data + offset, length);
    return 0;
}

typedef struct {FatVolume *volume; FatFile file[QCOM_MDT_MAX_SEGMENTS];} Pieces;
static int piece_read(void *context, unsigned piece, uint64_t offset, void *to, size_t length)
{
    Pieces *p = context;
    return fat_read(p->volume, &p->file[piece], offset, to, length);
}
static int joined_read(void *context, uint64_t offset, void *to, size_t length)
{
    void **c = context;
    return qcom_mdt_split_read(c[0], piece_read, c[1], offset, to, length);
}

int main(int argc, char **argv)
{
    CHECK(argc == 4 || argc == 6);
    size_t disk_size, joined_size;
    Disk disk = {0};
    disk.data = slurp(argv[1], &disk_size);
    disk.size = disk_size;
    const uint8_t *joined = slurp(argv[3], &joined_size);
    static FatVolume v;
    CHECK(fat_mount(&v, disk.size, disk_read, &disk) == FatOk);

    /* lookups: any case, missing names, a file used as a directory, a directory read as a file */
    FatFile f, dir;
    char path[64];
    snprintf(path, sizeof path, "%s.b00", argv[2]);
    CHECK(fat_open(&v, path, &f) == FatOk && !f.directory && f.size >= 52);
    for (char *c = path; *c; ++c) if (*c >= 'a' && *c <= 'z') *c -= 32;
    FatFile g;
    CHECK(fat_open(&v, path, &g) == FatOk && g.first_cluster == f.first_cluster && g.size == f.size);
    CHECK(fat_open(&v, "image/nosuch.b00", &g) == FatNotFound);
    CHECK(fat_open(&v, "image/a.long.name", &g) == FatNotFound);
    CHECK(fat_open(&v, "image/toolongname.b00", &g) == FatNotFound);
    snprintf(path, sizeof path, "%s.b00/x", argv[2]);
    CHECK(fat_open(&v, path, &g) == FatNotDirectory);
    CHECK(fat_open(&v, "image", &dir) == FatOk && dir.directory);
    uint8_t byte;
    CHECK(fat_read(&v, &dir, 0, &byte, 1) == FatIsDirectory);
    CHECK(fat_read(&v, &f, f.size, &byte, 1) == FatRange);
    CHECK(fat_read(&v, &f, f.size, &byte, 0) == FatOk);

    /* the split image */
    uint8_t *headers = malloc(f.size);
    CHECK(fat_read(&v, &f, 0, headers, f.size) == FatOk);
    QcomMdtSplit split;
    CHECK(qcom_mdt_split(&split, headers, f.size) == QcomMdtOk);
    CHECK(split.size == joined_size);
    static Pieces pieces;
    pieces.volume = &v;
    for (unsigned i = 0; i < split.count; ++i) {
        if (!split.piece[i].size) continue;
        snprintf(path, sizeof path, "%s.b%02u", argv[2], i);
        CHECK(fat_open(&v, path, &pieces.file[i]) == FatOk);
        CHECK(pieces.file[i].size >= split.piece[i].size);
    }
    uint8_t *all = malloc(joined_size);
    CHECK(qcom_mdt_split_read(&split, piece_read, &pieces, 0, all, joined_size) == QcomMdtOk);
    CHECK(!memcmp(all, joined, joined_size));
    /* odd chunk sizes cross cluster, piece and gap edges */
    const size_t steps[] = {1, 7, 4095, 4097, 16383, 65537};
    for (unsigned s = 0; s < sizeof steps / sizeof steps[0]; ++s) {
        if (joined_size / steps[s] > 200000) continue;  /* each read walks its chain from the start: keep the big images quick */
        memset(all, 0xa5, joined_size);
        for (size_t at = 0; at < joined_size; at += steps[s]) {
            const size_t n = joined_size - at < steps[s] ? joined_size - at : steps[s];
            CHECK(qcom_mdt_split_read(&split, piece_read, &pieces, at, all + at, n) == QcomMdtOk);
        }
        CHECK(!memcmp(all, joined, joined_size));
    }
    for (unsigned i = 1; i < split.count; ++i) {
        if (!split.piece[i].size) continue;
        const uint64_t edge = split.piece[i].offset;
        const size_t n = joined_size - edge < 64 ? joined_size - edge : 64;
        CHECK(edge >= 64);
        CHECK(qcom_mdt_split_read(&split, piece_read, &pieces, edge - 64, all, 64 + n) == QcomMdtOk);
        CHECK(!memcmp(all, joined + edge - 64, 64 + n));
    }
    CHECK(qcom_mdt_split_read(&split, piece_read, &pieces, joined_size, all, 1) == QcomMdtOutside);

    /* the joined view parses as firmware and gives the same metadata as the joined file */
    QcomMdtImage image;
    void *ctx[2] = {&split, &pieces};
    CHECK(qcom_mdt_parse(&image, split.size, joined_read, ctx) == QcomMdtOk);
    const uint64_t meta_size = qcom_mdt_metadata_size(&image);
    uint8_t *meta = malloc(meta_size);
    CHECK(qcom_mdt_metadata(&image, joined_read, ctx, meta, meta_size) == QcomMdtOk);
    CHECK(!memcmp(meta, joined, image.header_size));
    CHECK(!memcmp(meta + image.header_size, joined + image.segment[image.hash].file_offset, meta_size - image.header_size));

    if (argc == 6) {
        size_t expect_size;
        const uint8_t *expect = slurp(argv[5], &expect_size);
        FatFile frag;
        CHECK(fat_open(&v, argv[4], &frag) == FatOk && frag.size == expect_size);
        uint8_t *got = malloc(expect_size);
        CHECK(fat_read(&v, &frag, 0, got, expect_size) == FatOk && !memcmp(got, expect, expect_size));
        CHECK(fat_read(&v, &frag, 12345, got, expect_size - 12345) == FatOk && !memcmp(got, expect + 12345, expect_size - 12345));
    }

    /* damage: a table entry pointing outside the volume stops the read with an error */
    uint8_t *broken = malloc(disk.size);
    memcpy(broken, disk.data, disk.size);
    Disk bad = {broken, disk.size, 0};
    static FatVolume w;
    CHECK(fat_mount(&w, bad.size, disk_read, &bad) == FatOk);
    FatFile big = {0, 0, 0};
    for (unsigned i = 0; i < split.count; ++i) if (pieces.file[i].size > big.size) big = pieces.file[i];
    if (big.size > w.cluster_bytes) {  /* the piece spans a cluster edge */
        const uint64_t entry = w.fat_offset + (uint64_t)big.first_cluster * (w.fat32 ? 4 : 2);
        memset(broken + entry, 0xff, w.fat32 ? 4 : 2);
        broken[entry] = 0x01;  /* cluster 1 (FAT16) or 0x0fff..01 (FAT32): not a data cluster */
        uint8_t *scratch = malloc(big.size);
        CHECK(fat_read(&w, &big, 0, scratch, big.size) == FatDamaged);
        /* a chain that points back at itself still ends (wrong bytes are caught by the firmware's signature check, not here) */
        w.cached_sector = UINT64_MAX;
        if (w.fat32) {broken[entry] = (uint8_t)big.first_cluster; broken[entry + 1] = (uint8_t)(big.first_cluster >> 8);
                      broken[entry + 2] = (uint8_t)(big.first_cluster >> 16); broken[entry + 3] = (uint8_t)(big.first_cluster >> 24);}
        else {broken[entry] = (uint8_t)big.first_cluster; broken[entry + 1] = (uint8_t)(big.first_cluster >> 8);}
        (void)fat_read(&w, &big, 0, scratch, big.size);
    }
    /* not a volume */
    memset(broken, 0, 512);
    CHECK(fat_mount(&w, bad.size, disk_read, &bad) == FatNotFat);

    printf("PASS modem firmware reader (%s): %u checks, %u disk reads\n", v.fat32 ? "FAT32" : "FAT16", checks, disk.reads);
    return 0;
}
