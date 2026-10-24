// SPDX-License-Identifier: BSD-2-Clause-Patent
// Firmware files for the remote processors, read by the kernel from the mounted Data volume (the device's own adsp.mbn and
// adsp_dtb.mbn, installed by the owner below /private/var/firmware; they are never part of the build). Plain VFS reads on the caller's
// thread: the ADSP thread is the only caller and it runs no interrupt work, so a slow read only delays that bring-up.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <IOKit/IOLib.h>

int sheng_fw_open(const char *path, void **handle, uint64_t *size);
int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length);
void sheng_fw_close(void *handle);

int sheng_fw_open(const char *path, void **handle, uint64_t *size)
{
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t vp = NULLVP;
    struct vnode_attr va;
    int error = vnode_open(path, FREAD, 0, 0, &vp, ctx);
    if (error) return error;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size);
    if ((error = vnode_getattr(vp, &va, ctx)) || !VATTR_IS_SUPPORTED(&va, va_data_size) || !va.va_data_size) {
        vnode_close(vp, FREAD, ctx);
        return error ? error : EFTYPE;
    }
    *handle = vp;
    *size = va.va_data_size;
    return 0;
}

int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length)
{
    vfs_context_t ctx = vfs_context_kernel();
    uint8_t *out = (uint8_t *)to;
    while (length) {
        const int want = length > (1u << 30) ? (1 << 30) : (int)length;
        int resid = want;
        const int error = vn_rdwr(UIO_READ, (vnode_t)handle, (caddr_t)out, want, (off_t)offset, UIO_SYSSPACE, IO_NOAUTH,
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
    if (handle) vnode_close((vnode_t)handle, FREAD, vfs_context_kernel());
}
