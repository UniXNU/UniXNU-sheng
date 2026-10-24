/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Qualcomm signed firmware images (.mbn, an ELF32 or ELF64 whose program headers carry the segment types in p_flags) as the peripheral
 * authentication service takes them: the metadata handed to the secure world (the ELF and program headers followed by the hash
 * segment), the extent of the loadable segments and where each one goes in the carveout. Pure parsing over a read callback: no
 * allocation, no hardware, so the same code runs in the kernel and in the host tests. */
#ifndef QCOM_MDT_H
#define QCOM_MDT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define QCOM_MDT_MAX_SEGMENTS 64u
#define QCOM_MDT_TYPE_MASK (7u << 24)
#define QCOM_MDT_TYPE_HASH (2u << 24)
#define QCOM_MDT_RELOCATABLE (1u << 27)

/* Reads length bytes at offset of the image file into to; returns 0 on success. */
typedef int (*QcomMdtRead)(void *context, uint64_t offset, void *to, size_t length);

typedef struct {
    uint64_t file_offset, file_size, memory_size, address;  /* address: p_paddr */
    uint32_t flags;
    uint8_t load;  /* a loadable segment (PT_LOAD, not the hash, memory size not zero) */
} QcomMdtSegment;

typedef struct {
    uint8_t elf64;
    uint32_t count;
    QcomMdtSegment segment[QCOM_MDT_MAX_SEGMENTS];
    int hash;                        /* index of the hash segment */
    uint64_t header_size;            /* p_filesz of the first program header: ELF header plus program headers */
    uint64_t min_address, max_address;  /* of the loadable segments */
    uint8_t relocatable;
    uint64_t file_size;
} QcomMdtImage;

enum {
    QcomMdtOk = 0,
    QcomMdtReadError = -1,
    QcomMdtNotElf = -2,
    QcomMdtBadHeaders = -3,
    QcomMdtNoHash = -4,
    QcomMdtTooMany = -5,
    QcomMdtNoSegments = -6,
    QcomMdtOutside = -7,
    QcomMdtTooSmall = -8,
};

/* Parses the ELF and program headers of an image of file_size bytes. */
int qcom_mdt_parse(QcomMdtImage *image, uint64_t file_size, QcomMdtRead read, void *context);

/* Bytes of the metadata blob for the authentication service: the header part and the hash segment. */
uint64_t qcom_mdt_metadata_size(const QcomMdtImage *image);

/* Writes the metadata blob (qcom_mdt_metadata_size bytes) to to. The hash segment must lie inside the file: images split into .bNN
 * files are joined into one .mbn on the host before they are installed. */
int qcom_mdt_metadata(const QcomMdtImage *image, QcomMdtRead read, void *context, uint8_t *to, uint64_t capacity);

/* Bytes the loadable segments span (the end rounded up to 4 KiB, minus the lowest address): the size to hand to the memory setup call
 * of a relocatable image. */
uint64_t qcom_mdt_span(const QcomMdtImage *image);

/* Offset of a loadable segment inside a carveout of region_size bytes at region_base: relocatable images are placed by their offset from
 * the lowest segment, fixed ones by their address. Returns QcomMdtOutside if the segment does not fit. */
int qcom_mdt_place(const QcomMdtImage *image, unsigned index, uint64_t region_base, uint64_t region_size, uint64_t *offset);

/* A split image (NAME.b00, NAME.b01, ... as found on the device's firmware partition) read as the joined image: the bytes of program
 * header i's segment come from piece i (NAME.bNN) at the segment's file offset, piece 0 being the ELF and program headers; whatever
 * lies between the pieces reads as zero. The pieces are the caller's files. */
typedef int (*QcomMdtPieceRead)(void *context, unsigned piece, uint64_t offset, void *to, size_t length);
typedef struct {
    uint32_t count;
    struct {uint64_t offset, size;} piece[QCOM_MDT_MAX_SEGMENTS];  /* size 0: no piece file for this header */
    uint64_t size;                                                  /* bytes of the joined image */
} QcomMdtSplit;

/* Lays out the pieces from the headers (the bytes of NAME.b00, or the start of NAME.mdt). */
int qcom_mdt_split(QcomMdtSplit *split, const uint8_t *headers, size_t length);
/* Reads a range of the joined image. */
int qcom_mdt_split_read(const QcomMdtSplit *split, QcomMdtPieceRead read, void *context, uint64_t offset, void *to, size_t length);

#ifdef __cplusplus
}
#endif
#endif
