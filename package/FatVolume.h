/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* A read-only FAT16/FAT32 volume over a byte read callback: enough to find a file by its long or short (8.3) name in a directory path and read
 * any range of it. Used to take the device's own firmware files from its firmware partition in place. No allocation: the one sector of
 * the allocation table in use is cached in the volume. The volume's metadata is untrusted: every cluster number and chain is bounds
 * checked and every chain walk is bounded, so a damaged volume gives an error, never a loop or a read outside the partition. */
#ifndef FAT_VOLUME_H
#define FAT_VOLUME_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Reads length bytes at offset (from the partition start) into to; returns 0 on success. */
typedef int (*FatRead)(void *context, uint64_t offset, void *to, size_t length);

enum {
    FatOk = 0,
    FatReadError = -1,
    FatNotFat = -2,     /* no boot sector of a FAT16 or FAT32 volume */
    FatNotFound = -3,
    FatNotDirectory = -4,
    FatIsDirectory = -5,
    FatDamaged = -6,    /* a cluster chain leaves the volume, loops or is shorter than the file */
    FatRange = -7,      /* the read goes past the end of the file */
};

typedef struct {
    FatRead read;
    void *context;
    uint64_t size;                                    /* partition bytes */
    uint8_t fat32;
    uint32_t sector_bytes, cluster_bytes, clusters;   /* clusters: data clusters, numbered 2 .. clusters + 1 */
    uint64_t fat_offset, root_offset, data_offset;    /* bytes from the partition start; root_offset is FAT16 only */
    uint32_t root_entries, root_cluster;              /* root_entries: FAT16 fixed root; root_cluster: FAT32 */
    uint64_t cached_sector;                           /* UINT64_MAX: nothing cached */
    uint8_t sector[4096];
} FatVolume;

typedef struct {
    uint32_t first_cluster, size;
    uint8_t directory;
} FatFile;

int fat_mount(FatVolume *v, uint64_t size, FatRead read, void *context);
/* path: names separated by '/', each matched in any case against the long name, else the 8.3 name ("image/adsp.b00"). */
int fat_open(FatVolume *v, const char *path, FatFile *file);
int fat_read(FatVolume *v, const FatFile *file, uint64_t offset, void *to, size_t length);

#ifdef __cplusplus
}
#endif
#endif
