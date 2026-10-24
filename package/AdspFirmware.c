// SPDX-License-Identifier: BSD-2-Clause-Patent
// Firmware files for the remote processors (they are never part of the build), from one of two places:
// - "modem:NAME": the device's own split image NAME.b00, NAME.b01, ... in /image of its firmware partition (modem_a, else modem_b), a
//   FAT volume read in place through the UFS driver (read only; nothing is installed and the boot image does not grow);
// - any other path: a file on a mounted volume (such as adsp.mbn installed by the owner below /private/var/firmware), read through VFS.
// Plain reads on the caller's thread: the ADSP thread is the only caller and it runs no interrupt work, so a slow read only delays that
// bring-up.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <IOKit/IOLib.h>
#include "FatVolume.h"
#include "QcomMdt.h"

int sheng_fw_open(const char *path, void **handle, uint64_t *size);
int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length);
void sheng_fw_close(void *handle);

extern int native_ufs_part_find(const char *name, uint32_t *lun, uint64_t *first, uint64_t *blocks) __attribute__((weak_import));
extern int native_ufs_part_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst) __attribute__((weak_import));

// ---- the firmware partition ----
struct partition {
    uint32_t lun;
    uint64_t first;
    uint64_t cached;  // block in block (UINT64_MAX: none)
    uint8_t block[4096];
};
static struct partition *gPart;
static FatVolume *gVolume;

// Bytes of the partition: whole blocks straight into to, the ends of a range through the one-block cache.
static int partitionRead(void *context, uint64_t offset, void *to, size_t length)
{
    struct partition *p = context;
    uint8_t *out = to;
    while (length) {
        const uint64_t lba = offset / 4096;
        const size_t within = (size_t)(offset % 4096);
        if (!within && length >= 4096) {
            const uint32_t n = length / 4096 > 256 ? 256 : (uint32_t)(length / 4096);
            if (!native_ufs_part_read(p->lun, p->first + lba, n, out)) return EIO;
            out += (size_t)n * 4096; offset += (uint64_t)n * 4096; length -= (size_t)n * 4096;
            continue;
        }
        if (p->cached != lba) {
            p->cached = UINT64_MAX;
            if (!native_ufs_part_read(p->lun, p->first + lba, 1, p->block)) return EIO;
            p->cached = lba;
        }
        const size_t take = 4096 - within < length ? 4096 - within : length;
        memcpy(out, p->block + within, take);
        out += take; offset += take; length -= take;
    }
    return 0;
}

static int mountPartition(void)
{
    static const char *const names[] = {"modem_a", "modem_b"};
    if (gVolume) return 0;
    if (!native_ufs_part_find || !native_ufs_part_read) return ENXIO;
    struct partition *p = IOMallocType(struct partition);
    FatVolume *v = IOMallocType(FatVolume);
    int rc = ENOENT;
    for (unsigned i = 0; i < 2 && rc; ++i) {
        uint64_t blocks = 0;
        // ENXIO while the UFS driver is not ready yet: the caller tries again later
        if (!native_ufs_part_find(names[i], &p->lun, &p->first, &blocks)) {rc = ENXIO; continue;}
        p->cached = UINT64_MAX;
        rc = fat_mount(v, blocks * 4096, partitionRead, p) ? EFTYPE : 0;
    }
    if (rc) {IOFreeType(p, struct partition); IOFreeType(v, FatVolume); return rc;}
    gPart = p;
    gVolume = v;
    return 0;
}

struct modem_image {
    QcomMdtSplit split;
    FatFile piece[QCOM_MDT_MAX_SEGMENTS];
};

static int pieceRead(void *context, unsigned piece, uint64_t offset, void *to, size_t length)
{
    struct modem_image *m = context;
    return fat_read(gVolume, &m->piece[piece], offset, to, length);
}

static int openModem(const char *name, struct modem_image **image, uint64_t *size)
{
    char path[48];
    int rc = mountPartition();
    if (rc) return rc;
    struct modem_image *m = IOMallocType(struct modem_image);
    snprintf(path, sizeof path, "image/%s.b00", name);
    FatFile headers;
    rc = fat_open(gVolume, path, &headers);
    if (rc == FatNotFound) rc = ENOENT;
    else if (rc) rc = EIO;
    else if (headers.size < 52 || headers.size > 64 * 1024) rc = EFTYPE;
    uint8_t *bytes = rc ? NULL : IOMallocData(headers.size);
    if (!rc && fat_read(gVolume, &headers, 0, bytes, headers.size)) rc = EIO;
    if (!rc && qcom_mdt_split(&m->split, bytes, headers.size)) rc = EFTYPE;
    if (bytes) IOFreeData(bytes, headers.size);
    for (unsigned i = 0; !rc && i < m->split.count; ++i) {
        if (!m->split.piece[i].size) continue;
        snprintf(path, sizeof path, "image/%s.b%02u", name, i);
        if (fat_open(gVolume, path, &m->piece[i])) rc = ENOENT;
        else if (m->piece[i].size < m->split.piece[i].size) rc = EFTYPE;
    }
    if (rc) {IOFreeType(m, struct modem_image); return rc;}
    *image = m;
    *size = m->split.size;
    return 0;
}

// ---- both kinds ----
struct sheng_fw {
    vnode_t vnode;
    struct modem_image *modem;
};

int sheng_fw_open(const char *path, void **handle, uint64_t *size)
{
    struct sheng_fw *fw = IOMallocType(struct sheng_fw);
    int error;
    if (!strncmp(path, "modem:", 6)) {
        error = openModem(path + 6, &fw->modem, size);
    } else {
        vfs_context_t ctx = vfs_context_kernel();
        struct vnode_attr va;
        error = vnode_open(path, FREAD, 0, 0, &fw->vnode, ctx);
        if (!error) {
            VATTR_INIT(&va);
            VATTR_WANTED(&va, va_data_size);
            if ((error = vnode_getattr(fw->vnode, &va, ctx)) || !VATTR_IS_SUPPORTED(&va, va_data_size) || !va.va_data_size) {
                vnode_close(fw->vnode, FREAD, ctx);
                fw->vnode = NULLVP;
                if (!error) error = EFTYPE;
            } else {
                *size = va.va_data_size;
            }
        }
    }
    if (error) {IOFreeType(fw, struct sheng_fw); return error;}
    *handle = fw;
    return 0;
}

int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length)
{
    struct sheng_fw *fw = handle;
    if (fw->modem) return qcom_mdt_split_read(&fw->modem->split, pieceRead, fw->modem, offset, to, length) ? EIO : 0;
    vfs_context_t ctx = vfs_context_kernel();
    uint8_t *out = (uint8_t *)to;
    while (length) {
        const int want = length > (1u << 30) ? (1 << 30) : (int)length;
        int resid = want;
        const int error = vn_rdwr(UIO_READ, fw->vnode, (caddr_t)out, want, (off_t)offset, UIO_SYSSPACE, IO_NOAUTH,
                                  vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        if (error) return error;
        if (resid == want) return EIO;  /* end of file before the bytes asked for */
        out += want - resid;
        offset += (uint64_t)(want - resid);
        length -= (size_t)(want - resid);
    }
    return 0;
}

void sheng_fw_close(void *handle)
{
    struct sheng_fw *fw = handle;
    if (!fw) return;
    if (fw->modem) IOFreeType(fw->modem, struct modem_image);
    if (fw->vnode) vnode_close(fw->vnode, FREAD, vfs_context_kernel());
    IOFreeType(fw, struct sheng_fw);
}
