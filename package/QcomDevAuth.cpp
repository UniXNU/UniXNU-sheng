/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Qualcomm QTEE SMCInvoke + SHMBridge + UFS RPMB listener (service 0x2000) + Xiaomi devauth.mbn TA client
 * for sheng magnetic keyboard cover offline authentication (MiAuth cmd 0x2020).
 *
 * Wire protocol constants and sequence live in QcomTeeWire.h (host-tested under ASan/UBSan). */
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/thread.h>
#include <string.h>
extern "C" {
#include "QcomTeeWire.h"
int native_ufs_query_desc(uint8_t idn, uint8_t index, void *dst, uint32_t max_bytes, uint32_t *actual_bytes) __attribute__((weak_import));
int native_ufs_rpmb_scsi(const uint8_t *cdb, uint32_t cdb_len, int direction, void *buf, uint32_t bytes, uint32_t *scsi_status) __attribute__((weak_import));
int sheng_fw_open(const char *path, void **handle, uint64_t *size) __attribute__((weak_import));
int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length) __attribute__((weak_import));
void sheng_fw_close(void *handle) __attribute__((weak_import));
}

const char kDevAuthFirmware[] = "/private/var/ngpu/firmware/qcom/sm8550/sheng/devauth.mbn";

/* [0] stage (1 alloc, 2 shm, 3 root, 4 open87, 5 reg2000, 6 open122, 7 loadTa, 8 ready; negative = failed stage),
 * [1] last SMC x0 / error, [2] last QTEE result, [3] last TA status word,
 * [4] offline token calls, [5] offline token successes, [6] callbacks handled, [7] MAP_REGION calls,
 * [8] RPMB INIT calls, [9] RPMB READ calls, [10] RPMB WRITE calls, [11] last RPMB error,
 * [12] UFS RPMB 512B sectors, [13] UFS RPMB rel_wr_count, [14] shmEnabled | (shmEnableX1 << 8),
 * [15] last callback (cxt & 0xffff) | ((op & 0xffff) << 16). */
extern "C" {
volatile int32_t native_devauth_stats[16];
}

namespace {

constexpr int64_t kScmInterrupted = 1;
constexpr int64_t kScmBusy = -12;

constexpr uint32_t kInBufBytes = 196608u;  /* 192 KiB: holds devauth.mbn (143 KiB) + headers + distName */
constexpr uint32_t kOutBufBytes = 4096u;   /* 4 KiB outbound / callback buffer */
constexpr uint32_t kRpmbBufBytes = QTEE_RPMB_BUF_ALLOC; /* 24 KiB */

constexpr uint32_t kRpmbMemId = QTEE_OBJ_NS_BIT | 1u;
constexpr uint32_t kRpmbCboId = QTEE_OBJ_NS_BIT | 2u;
constexpr uint32_t kRpmbMapId = QTEE_OBJ_NS_BIT | 3u;

void flushRange(const void *ptr, size_t len) {
    if (!ptr || !len) return;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr) & ~uintptr_t(63);
    const uintptr_t end = reinterpret_cast<uintptr_t>(ptr) + len;
    for (; addr < end; addr += 64) {
        __asm__ volatile("dc civac, %0" :: "r"(addr) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

struct ScmRes {
    int64_t x0;
    uint64_t x1;
    uint64_t x2;
    uint64_t x3;
};

ScmRes scmCall(uint32_t fn, uint32_t arginfo, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0) {
    ScmRes res = {-1, 0, 0, 0};
    for (unsigned busy = 0; busy < 20; ++busy) {
        uint64_t curFn = fn;
        uint64_t session = 0;
        for (unsigned resumes = 0;;) {
            register uint64_t x0 __asm__("x0") = curFn;
            register uint64_t x1 __asm__("x1") = arginfo;
            register uint64_t x2 __asm__("x2") = a0;
            register uint64_t x3 __asm__("x3") = a1;
            register uint64_t x4 __asm__("x4") = a2;
            register uint64_t x5 __asm__("x5") = a3;
            register uint64_t x6 __asm__("x6") = session;
            register uint64_t x7 __asm__("x7") = 0;
            __asm__ volatile("smc #0"
                             : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                             :: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
            res = {static_cast<int64_t>(x0), x1, x2, x3};
            if (res.x0 != kScmInterrupted || ++resumes > 200000) break;
            curFn = static_cast<uint64_t>(kScmInterrupted);
            session = x6;
        }
        if (res.x0 != kScmBusy) break;
        IOSleep(30);
    }
    return res;
}

struct DmaBuf {
    IOBufferMemoryDescriptor *desc = nullptr;
    void *va = nullptr;
    uint64_t pa = 0;
    uint32_t size = 0;
    uint64_t shmHandle = 0;
    bool hasShm = false;
};

bool allocDma(DmaBuf &buf, uint32_t size) {
    static const uint64_t kMasks[2] = {0xffffffffULL & ~UINT64_C(4095), ~UINT64_C(4095)};
    for (unsigned i = 0; i < 2; ++i) {
        IOBufferMemoryDescriptor *d = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task, (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous, size, kMasks[i]);
        if (!d) continue;
        if (d->prepare() != kIOReturnSuccess) {
            d->release();
            continue;
        }
        IOByteCount segLen = 0;
        const addr64_t pa = d->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone);
        void *va = d->getBytesNoCopy();
        if (pa && !(pa & 4095) && segLen >= size && va) {
            memset(va, 0, size);
            flushRange(va, size);
            buf.desc = d;
            buf.va = va;
            buf.pa = pa;
            buf.size = size;
            return true;
        }
        d->complete();
        d->release();
    }
    return false;
}

bool registerShm(DmaBuf &buf) {
    const uint32_t arginfo = qcom_scm_args4(4, QCOM_SCM_ARG_VAL, QCOM_SCM_ARG_VAL, QCOM_SCM_ARG_VAL, QCOM_SCM_ARG_VAL);
    const ScmRes r = scmCall(QCOM_SCM_FN_SHM_CREATE, arginfo,
                             buf.pa | QCOM_SCM_PERM_RW,
                             buf.pa | QCOM_SCM_PERM_RW,
                             static_cast<uint64_t>(buf.size) | (1ULL << QCOM_SHM_BRIDGE_VM_SHIFT),
                             QCOM_SCM_VMID_HLOS);
    if (r.x0 != 0 || r.x1 != 0) {
        native_devauth_stats[1] = static_cast<int32_t>(r.x0 ? r.x0 : r.x1);
        return false;
    }
    buf.shmHandle = r.x2;
    buf.hasShm = true;
    return true;
}

struct DevAuthState {
    volatile int32_t busy = 0;
    bool ready = false;
    bool shmProbed = false;
    bool shmEnabled = false;
    bool rpmbRegistered = false;
    DmaBuf inBuf, outBuf, rpmbBuf;
    uint8_t *rpmbScratch = nullptr;
    uint32_t clientEnv = 0;
    uint32_t regCbo = 0;
    uint32_t appLoader = 0;
    uint32_t appController = 0;
    uint32_t rpmbSectors = 0;
    uint32_t rpmbRelWr = 0;
    bool rpmbInited = false;
} gAuth;

int rpmbInitDevice() {
    if (!&native_ufs_query_desc || !&native_ufs_rpmb_scsi) return -1;
    uint8_t dev[0x40], geo[0x48], unit[0x23];
    uint32_t devLen = 0, geoLen = 0, unitLen = 0;
    memset(dev, 0, sizeof dev);
    memset(geo, 0, sizeof geo);
    memset(unit, 0, sizeof unit);
    if (native_ufs_query_desc(0x00, 0x00, dev, sizeof dev, &devLen) != 0) return -2;
    if (native_ufs_query_desc(0x07, 0x00, geo, sizeof geo, &geoLen) != 0) return -3;
    if (native_ufs_query_desc(0x02, 0xc4, unit, sizeof unit, &unitLen) != 0) return -4;
    uint32_t sectors = 0, relWr = 0;
    if (qtee_ufs_rpmb_params(dev, devLen, geo, geoLen, unit, unitLen, &sectors, &relWr) != 0) return -5;
    /* Clear any power-on Unit Attention on W-LUN 0xC4 before the first RPMB SECURITY PROTOCOL transfer. */
    uint8_t tur[6] = {0, 0, 0, 0, 0, 0};
    for (unsigned tries = 0; tries < 2; ++tries) {
        uint32_t scsi = 0;
        if (native_ufs_rpmb_scsi(tur, sizeof tur, 0, nullptr, 0, &scsi) == 0 && scsi == 0) break;
    }
    gAuth.rpmbSectors = sectors;
    gAuth.rpmbRelWr = relWr;
    gAuth.rpmbInited = true;
    native_devauth_stats[12] = static_cast<int32_t>(sectors);
    native_devauth_stats[13] = static_cast<int32_t>(relWr);
    return 0;
}

int handleRpmbInit() {
    native_devauth_stats[8] = native_devauth_stats[8] + 1;
    qtee_rpmb_init_req req;
    memcpy(&req, gAuth.rpmbBuf.va, sizeof req);
    int rc = gAuth.rpmbInited ? 0 : rpmbInitDevice();
    if (rc != 0) native_devauth_stats[11] = rc;
    if (req.version <= 1u) {
        qtee_rpmb_init_res_v1 res;
        memset(&res, 0, sizeof res);
        res.cmd_id = req.cmd_id;
        res.version = 1u;
        res.status = rc;
        res.num_sectors = rc == 0 ? gAuth.rpmbSectors : 0;
        res.rel_wr_count = rc == 0 ? gAuth.rpmbRelWr : 0;
        memcpy(gAuth.rpmbBuf.va, &res, sizeof res);
    } else {
        qtee_rpmb_init_res_v2 res;
        memset(&res, 0, sizeof res);
        res.cmd_id = req.cmd_id;
        res.version = 2u;
        res.status = rc;
        res.num_sectors = rc == 0 ? gAuth.rpmbSectors : 0;
        res.rel_wr_count = rc == 0 ? gAuth.rpmbRelWr : 0;
        res.dev_type = QTEE_RPMB_DEV_UFS;
        memcpy(gAuth.rpmbBuf.va, &res, sizeof res);
    }
    return 0;
}

int handleRpmbRead() {
    native_devauth_stats[9] = native_devauth_stats[9] + 1;
    qtee_rpmb_rw_req req;
    memcpy(&req, gAuth.rpmbBuf.va, sizeof req);
    qtee_rpmb_rw_res res;
    memset(&res, 0, sizeof res);
    res.cmd_id = req.cmd_id;
    res.res_buff_offset = sizeof(res);
    res.version = req.version;

    int rc = 0;
    if (!gAuth.rpmbInited) rc = rpmbInitDevice();
    const uint32_t rel = gAuth.rpmbRelWr ? gAuth.rpmbRelWr : 1u;
    const uint32_t blkCnt = req.num_sectors;
    const uint32_t numTrans = blkCnt ? ((blkCnt + rel - 1u) / rel) : 0;
    const uint32_t reqNeed = numTrans * QTEE_RPMB_BLK_SIZE;
    const uint32_t rspBytes = blkCnt * QTEE_RPMB_BLK_SIZE;
    if (rc == 0 && (!blkCnt || rspBytes > QTEE_RPMB_MAX_DATA_LEN || reqNeed > QTEE_RPMB_MAX_DATA_LEN ||
                    req.req_buff_offset > kRpmbBufBytes || reqNeed > kRpmbBufBytes - req.req_buff_offset ||
                    sizeof(res) + rspBytes > kRpmbBufBytes)) {
        rc = -10;
    }
    if (rc == 0) {
        memcpy(gAuth.rpmbScratch, static_cast<const uint8_t *>(gAuth.rpmbBuf.va) + req.req_buff_offset, reqNeed);
        uint8_t *dst = static_cast<uint8_t *>(gAuth.rpmbBuf.va) + sizeof(res);
        uint32_t rem = blkCnt;
        for (uint32_t i = 0; i < numTrans; ++i) {
            const uint32_t chunk = rem > rel ? rel : rem;
            const uint32_t inBytes = chunk * QTEE_RPMB_BLK_SIZE;
            uint8_t cdbOut[12], cdbIn[12];
            uint32_t scsi = 0;
            qtee_build_rpmb_cdb(cdbOut, 0xb5u, QTEE_RPMB_BLK_SIZE);
            if (native_ufs_rpmb_scsi(cdbOut, 12, 1, gAuth.rpmbScratch + i * QTEE_RPMB_BLK_SIZE, QTEE_RPMB_BLK_SIZE, &scsi) != 0 || scsi != 0) {
                rc = -11;
                break;
            }
            qtee_build_rpmb_cdb(cdbIn, 0xa2u, inBytes);
            if (native_ufs_rpmb_scsi(cdbIn, 12, 2, dst, inBytes, &scsi) != 0 || scsi != 0) {
                rc = -12;
                break;
            }
            dst += inBytes;
            rem -= chunk;
        }
    }
    res.status = rc;
    res.res_buff_len = rc == 0 ? rspBytes : 0;
    if (rc != 0) native_devauth_stats[11] = rc;
    memcpy(gAuth.rpmbBuf.va, &res, sizeof res);
    return 0;
}

int handleRpmbWrite() {
    native_devauth_stats[10] = native_devauth_stats[10] + 1;
    qtee_rpmb_rw_req req;
    memcpy(&req, gAuth.rpmbBuf.va, sizeof req);
    qtee_rpmb_rw_res res;
    memset(&res, 0, sizeof res);
    res.cmd_id = req.cmd_id;
    res.res_buff_offset = sizeof(res);
    res.version = req.version;

    int rc = 0;
    if (!gAuth.rpmbInited) rc = rpmbInitDevice();
    const uint32_t maxRel = gAuth.rpmbRelWr ? gAuth.rpmbRelWr : 1u;
    const uint32_t blkCnt = req.num_sectors;
    const uint32_t reqBytes = blkCnt * QTEE_RPMB_BLK_SIZE;
    uint32_t perTrans = (req.rel_wr_count && req.rel_wr_count <= maxRel) ? req.rel_wr_count : maxRel;
    if (rc == 0 && (!blkCnt || reqBytes > QTEE_RPMB_MAX_DATA_LEN ||
                    req.req_buff_offset > kRpmbBufBytes || reqBytes > kRpmbBufBytes - req.req_buff_offset ||
                    sizeof(res) + QTEE_RPMB_BLK_SIZE > kRpmbBufBytes)) {
        rc = -20;
    }
    if (rc == 0) {
        memcpy(gAuth.rpmbScratch, static_cast<const uint8_t *>(gAuth.rpmbBuf.va) + req.req_buff_offset, reqBytes);
        uint8_t *dst = static_cast<uint8_t *>(gAuth.rpmbBuf.va) + sizeof(res);
        uint8_t resReqFrame[QTEE_RPMB_BLK_SIZE];
        memset(resReqFrame, 0, sizeof resReqFrame);
        resReqFrame[510] = 0x00u;
        resReqFrame[511] = 0x05u; /* RPMB RESULT_READ_REQ */
        uint32_t done = 0;
        while (done < blkCnt) {
            const uint32_t chunk = (blkCnt - done) > perTrans ? perTrans : (blkCnt - done);
            const uint32_t outBytes = chunk * QTEE_RPMB_BLK_SIZE;
            uint8_t cdbOut[12], cdbResOut[12], cdbIn[12];
            uint32_t scsi = 0;
            qtee_build_rpmb_cdb(cdbOut, 0xb5u, outBytes);
            if (native_ufs_rpmb_scsi(cdbOut, 12, 1, gAuth.rpmbScratch + done * QTEE_RPMB_BLK_SIZE, outBytes, &scsi) != 0 || scsi != 0) {
                rc = -21;
                break;
            }
            qtee_build_rpmb_cdb(cdbResOut, 0xb5u, QTEE_RPMB_BLK_SIZE);
            if (native_ufs_rpmb_scsi(cdbResOut, 12, 1, resReqFrame, QTEE_RPMB_BLK_SIZE, &scsi) != 0 || scsi != 0) {
                rc = -22;
                break;
            }
            qtee_build_rpmb_cdb(cdbIn, 0xa2u, QTEE_RPMB_BLK_SIZE);
            if (native_ufs_rpmb_scsi(cdbIn, 12, 2, dst, QTEE_RPMB_BLK_SIZE, &scsi) != 0 || scsi != 0) {
                rc = -23;
                break;
            }
            done += chunk;
        }
    }
    res.status = rc;
    res.res_buff_len = rc == 0 ? QTEE_RPMB_BLK_SIZE : 0;
    if (rc != 0) native_devauth_stats[11] = rc;
    memcpy(gAuth.rpmbBuf.va, &res, sizeof res);
    return 0;
}

int handleRpmbPartition() {
    qtee_rpmb_part_req req;
    memcpy(&req, gAuth.rpmbBuf.va, sizeof req);
    qtee_rpmb_part_res res;
    res.cmd_id = req.cmd_id;
    res.status = -1;
    res.num_partitions = 0;
    res.rsp_buff_offset = sizeof(res);
    memcpy(gAuth.rpmbBuf.va, &res, sizeof res);
    return 0;
}

int handleRpmbDispatch() {
    flushRange(gAuth.rpmbBuf.va, kRpmbBufBytes);
    uint32_t cmd = 0;
    memcpy(&cmd, gAuth.rpmbBuf.va, sizeof cmd);
    int rc = -1;
    switch (cmd) {
    case QTEE_RPMB_CMD_INIT:      rc = handleRpmbInit(); break;
    case QTEE_RPMB_CMD_READ:      rc = handleRpmbRead(); break;
    case QTEE_RPMB_CMD_WRITE:     rc = handleRpmbWrite(); break;
    case QTEE_RPMB_CMD_PARTITION: rc = handleRpmbPartition(); break;
    default:
        native_devauth_stats[11] = -99;
        rc = -1;
        break;
    }
    flushRange(gAuth.rpmbBuf.va, kRpmbBufBytes);
    return rc;
}

void handleCallback() {
    native_devauth_stats[6] = native_devauth_stats[6] + 1;
    auto *cb = static_cast<qtee_msg_callback *>(gAuth.outBuf.va);
    const uint32_t cxt = cb->cxt;
    const uint32_t op = cb->op & QTEE_OP_MASK;
    const uint32_t counts = cb->counts;
    const unsigned nib = qtee_num_ib(counts);
    const unsigned nob = qtee_num_ob(counts);
    const unsigned nio = qtee_num_io(counts);
    const unsigned noo = qtee_num_oo(counts);
    native_devauth_stats[15] = static_cast<int32_t>((cxt & 0xffffu) | ((op & 0xffffu) << 16));

    /* Find the end of the callback message so any trailing async message can be cleared before returning. */
    uint32_t cbEnd = qtee_align8(static_cast<uint32_t>(sizeof(qtee_msg_callback) + (nib + nob + nio + noo) * sizeof(qtee_msg_arg)));
    for (unsigned i = 0; i < nib + nob; ++i) {
        const uint32_t off = cb->args[i].b.offset;
        const uint32_t sz = cb->args[i].b.size;
        if (off <= kOutBufBytes && sz <= kOutBufBytes - off) {
            const uint32_t end = qtee_align8(off + sz);
            if (end > cbEnd) cbEnd = end;
        }
    }

    uint32_t result = QTEE_MSG_ERROR_INVALID;
    if (op == QTEE_OP_RETAIN || op == QTEE_OP_RELEASE) {
        result = QTEE_MSG_OK;
    } else if (cxt == QTEE_OBJ_PRIMORDIAL) {
        if (op == QTEE_PRIM_OP_MAP_REGION) {
            native_devauth_stats[7] = native_devauth_stats[7] + 1;
            if (nob >= 1 && nio >= 1 && noo >= 1) {
                const uint32_t off = cb->args[nib].b.offset;
                const uint32_t sz = cb->args[nib].b.size;
                const uint32_t memObj = cb->args[nib + nob].o;
                if (memObj == kRpmbMemId && off <= kOutBufBytes && sizeof(qtee_mapping_info) <= sz &&
                    sz <= kOutBufBytes - off) {
                    qtee_mapping_info info;
                    info.paddr = gAuth.rpmbBuf.pa;
                    info.len = kRpmbBufBytes;
                    info.perms = QCOM_SCM_PERM_RW;
                    memcpy(static_cast<uint8_t *>(gAuth.outBuf.va) + off, &info, sizeof info);
                    cb->args[nib].b.size = sizeof(info);
                    cb->args[nib + nob + nio].o = kRpmbMapId;
                    result = QTEE_MSG_OK;
                }
            }
        } else if (op == QTEE_PRIM_OP_YIELD) {
            IOSleep(1);
            result = QTEE_MSG_OK;
        } else if (op == QTEE_PRIM_OP_SLEEP) {
            if (nib >= 1 && cb->args[0].b.size >= 4 && cb->args[0].b.offset <= kOutBufBytes - 4) {
                uint32_t ms = 0;
                memcpy(&ms, static_cast<const uint8_t *>(gAuth.outBuf.va) + cb->args[0].b.offset, 4);
                if (ms > 5000u) ms = 5000u;
                IOSleep(ms);
                result = QTEE_MSG_OK;
            }
        }
    } else if (cxt == kRpmbCboId) {
        if (op == QTEE_CBO_OP_REQUEST) {
            const int rc = handleRpmbDispatch();
            for (unsigned i = 0; i < nob; ++i) {
                const uint32_t off = cb->args[nib + i].b.offset;
                const uint32_t sz = cb->args[nib + i].b.size;
                if (off <= kOutBufBytes && sz <= kOutBufBytes - off && sz) {
                    memset(static_cast<uint8_t *>(gAuth.outBuf.va) + off, 0, sz);
                }
                if (sz == 4u) cb->args[nib + i].b.size = 4u;
                else cb->args[nib + i].b.size = 0u;
            }
            for (unsigned i = 0; i < noo; ++i) cb->args[nib + nob + nio + i].o = QTEE_OBJ_NULL;
            result = rc == 0 ? QTEE_MSG_OK : QTEE_MSG_ERROR;
        } else if (op == QTEE_CBO_OP_WAIT) {
            result = QTEE_MSG_OK;
        }
    }

    cb->result = result;
    if (cbEnd < kOutBufBytes) {
        memset(static_cast<uint8_t *>(gAuth.outBuf.va) + cbEnd, 0, kOutBufBytes - cbEnd);
    }
}

int qteeInvoke(uint32_t cxt, uint32_t op,
               const qtee_buf_desc *ib, unsigned nib,
               const qtee_buf_desc *ob, unsigned nob, uint32_t *obSizes,
               const uint32_t *io, unsigned nio,
               uint32_t *oo, unsigned noo,
               uint32_t *qteeResOut) {
    const uint32_t used = qtee_pack_invoke(gAuth.inBuf.va, kInBufBytes, cxt, op, ib, nib, ob, nob, io, nio, noo);
    if (!used) return -1;
    const uint32_t inSize = (used + 4095u) & ~4095u;
    memset(gAuth.outBuf.va, 0, kOutBufBytes);
    flushRange(gAuth.inBuf.va, inSize);
    flushRange(gAuth.outBuf.va, kOutBufBytes);

    const uint32_t invArgs = qcom_scm_args4(4, QCOM_SCM_ARG_RW, QCOM_SCM_ARG_VAL, QCOM_SCM_ARG_RW, QCOM_SCM_ARG_VAL);
    const uint32_t cbArgs = qcom_scm_args4(2, QCOM_SCM_ARG_RW, QCOM_SCM_ARG_VAL, 0, 0);

    ScmRes r = scmCall(QCOM_SCM_FN_QTEE_INVOKE, invArgs, gAuth.inBuf.pa, inSize, gAuth.outBuf.pa, kOutBufBytes);
    for (unsigned iter = 0; iter < 512; ++iter) {
        flushRange(gAuth.outBuf.va, kOutBufBytes);
        if (r.x0 != 0) {
            native_devauth_stats[1] = static_cast<int32_t>(r.x0);
            return -2;
        }
        if (r.x1 != QTEE_RES_INBOUND_REQ) {
            flushRange(gAuth.inBuf.va, inSize);
            if (qteeResOut) *qteeResOut = static_cast<uint32_t>(r.x2);
            if (r.x2 != QTEE_MSG_OK) {
                native_devauth_stats[2] = static_cast<int32_t>(r.x2);
                return -3;
            }
            return qtee_unpack_invoke(gAuth.inBuf.va, inSize, ob, nob, obSizes, oo, noo) == 0 ? 0 : -4;
        }
        handleCallback();
        flushRange(gAuth.outBuf.va, kOutBufBytes);
        r = scmCall(QCOM_SCM_FN_QTEE_CB_RSP, cbArgs, gAuth.outBuf.pa, kOutBufBytes);
    }
    native_devauth_stats[1] = -60;
    return -5;
}

bool failStage(int step, int32_t err) {
    native_devauth_stats[1] = err;
    native_devauth_stats[0] = -step;
    return false;
}

bool ensureReady() {
    if (gAuth.ready) return true;
    if (!&native_ufs_query_desc || !&native_ufs_rpmb_scsi || !&sheng_fw_open || !&sheng_fw_read || !&sheng_fw_close) {
        return failStage(1, -78);
    }

    /* Confirm UFS storage host is up before registering the RPMB listener with QTEE. */
    if (!gAuth.rpmbInited && rpmbInitDevice() != 0) return failStage(1, -11);

    /* Stage 1: allocate DMA buffers and RPMB scratch once. */
    native_devauth_stats[0] = 1;
    if (!gAuth.rpmbScratch) {
        gAuth.rpmbScratch = static_cast<uint8_t *>(IOMallocData(QTEE_RPMB_MAX_DATA_LEN));
        if (!gAuth.rpmbScratch) return failStage(1, -12);
    }
    if (!gAuth.inBuf.va && !allocDma(gAuth.inBuf, kInBufBytes)) return failStage(1, -13);
    if (!gAuth.outBuf.va && !allocDma(gAuth.outBuf, kOutBufBytes)) return failStage(1, -14);
    if (!gAuth.rpmbBuf.va && !allocDma(gAuth.rpmbBuf, kRpmbBufBytes)) return failStage(1, -15);

    /* Stage 2: enable SHMBridge and register DMA buffers if active. */
    native_devauth_stats[0] = 2;
    if (!gAuth.shmProbed) {
        const ScmRes en = scmCall(QCOM_SCM_FN_SHM_ENABLE, 0);
        if (en.x0 != 0) return failStage(2, static_cast<int32_t>(en.x0));
        gAuth.shmEnabled = (en.x1 != QCOM_SHM_BRIDGE_NOTSUPP);
        gAuth.shmProbed = true;
        native_devauth_stats[14] = (gAuth.shmEnabled ? 1 : 0) | (static_cast<int32_t>(en.x1 & 0xffu) << 8);
    }
    if (gAuth.shmEnabled) {
        if (!gAuth.inBuf.hasShm && !registerShm(gAuth.inBuf)) return failStage(2, native_devauth_stats[1]);
        if (!gAuth.outBuf.hasShm && !registerShm(gAuth.outBuf)) return failStage(2, native_devauth_stats[1]);
        if (!gAuth.rpmbBuf.hasShm && !registerShm(gAuth.rpmbBuf)) return failStage(2, native_devauth_stats[1]);
    }

    uint32_t qres = 0;

    /* Stage 3: ROOT::registerWithCredentials(NULL) -> clientEnv. */
    native_devauth_stats[0] = 3;
    if (!gAuth.clientEnv) {
        const uint32_t credObj = QTEE_OBJ_NULL;
        uint32_t outObj = 0;
        if (qteeInvoke(QTEE_OBJ_ROOT, QTEE_ROOT_OP_REG_WITH_CRED, nullptr, 0, nullptr, 0, nullptr,
                       &credObj, 1, &outObj, 1, &qres) != 0 || !outObj) {
            return failStage(3, qres ? static_cast<int32_t>(qres) : native_devauth_stats[1]);
        }
        gAuth.clientEnv = outObj;
    }

    /* Stage 4: clientEnv::open(CRegisterListenerCBO = 87) -> regCbo. */
    native_devauth_stats[0] = 4;
    if (!gAuth.regCbo) {
        const uint32_t uid = QTEE_UID_REG_LISTENER_CBO;
        const qtee_buf_desc ib = {&uid, nullptr, sizeof uid};
        uint32_t outObj = 0;
        if (qteeInvoke(gAuth.clientEnv, QTEE_CLIENT_ENV_OP_OPEN, &ib, 1, nullptr, 0, nullptr,
                       nullptr, 0, &outObj, 1, &qres) != 0 || !outObj) {
            return failStage(4, qres ? static_cast<int32_t>(qres) : native_devauth_stats[1]);
        }
        gAuth.regCbo = outObj;
    }

    /* Stage 5: regCbo::register(0x2000, kRpmbCboId, kRpmbMemId). */
    native_devauth_stats[0] = 5;
    if (!gAuth.rpmbRegistered) {
        const uint32_t svc = QTEE_RPMB_SERVICE_ID;
        const qtee_buf_desc ib = {&svc, nullptr, sizeof svc};
        const uint32_t io[2] = {kRpmbCboId, kRpmbMemId};
        if (qteeInvoke(gAuth.regCbo, QTEE_REG_CBO_OP_REGISTER, &ib, 1, nullptr, 0, nullptr,
                       io, 2, nullptr, 0, &qres) != 0) {
            return failStage(5, qres ? static_cast<int32_t>(qres) : native_devauth_stats[1]);
        }
        gAuth.rpmbRegistered = true;
    }

    /* Stage 6: clientEnv::open(CAppLoader = 122) -> appLoader. */
    native_devauth_stats[0] = 6;
    if (!gAuth.appLoader) {
        const uint32_t uid = QTEE_UID_QSEECOM_LOADER;
        const qtee_buf_desc ib = {&uid, nullptr, sizeof uid};
        uint32_t outObj = 0;
        if (qteeInvoke(gAuth.clientEnv, QTEE_CLIENT_ENV_OP_OPEN, &ib, 1, nullptr, 0, nullptr,
                       nullptr, 0, &outObj, 1, &qres) != 0 || !outObj) {
            return failStage(6, qres ? static_cast<int32_t>(qres) : native_devauth_stats[1]);
        }
        gAuth.appLoader = outObj;
    }

    /* Stage 7: read devauth.mbn from Data addon and invoke appLoader::loadFromBuffer. */
    native_devauth_stats[0] = 7;
    if (!gAuth.appController) {
        void *file = nullptr;
        uint64_t fwSize = 0;
        int openRc = -1;
        for (unsigned tries = 0; tries < 16; ++tries) {
            openRc = sheng_fw_open(kDevAuthFirmware, &file, &fwSize);
            if (openRc == 0) break;
            IOSleep(250);
        }
        if (openRc != 0 || !file || !fwSize || fwSize > kInBufBytes - 4096u) {
            if (file) sheng_fw_close(file);
            return failStage(7, openRc ? -openRc : -22);
        }
        uint8_t *fwBuf = static_cast<uint8_t *>(IOMallocData(static_cast<size_t>(fwSize)));
        if (!fwBuf) {
            sheng_fw_close(file);
            return failStage(7, -12);
        }
        const int readRc = sheng_fw_read(file, 0, fwBuf, static_cast<size_t>(fwSize));
        sheng_fw_close(file);
        if (readRc != 0) {
            IOFreeData(fwBuf, static_cast<size_t>(fwSize));
            return failStage(7, -readRc);
        }
        static const char kAppName[8] = "devauth";
        uint8_t distName[256];
        memset(distName, 0, sizeof distName);
        const qtee_buf_desc ib[2] = {
            {fwBuf, nullptr, static_cast<uint32_t>(fwSize)},
            {kAppName, nullptr, sizeof kAppName},
        };
        const qtee_buf_desc ob[1] = {{nullptr, distName, sizeof distName}};
        uint32_t obLen = 0, outObj = 0;
        const int invRc = qteeInvoke(gAuth.appLoader, QTEE_LOADER_OP_LOAD_BUF, ib, 2, ob, 1, &obLen,
                                     nullptr, 0, &outObj, 1, &qres);
        IOFreeData(fwBuf, static_cast<size_t>(fwSize));
        if (invRc != 0 || !outObj) {
            return failStage(7, qres ? static_cast<int32_t>(qres) : native_devauth_stats[1]);
        }
        gAuth.appController = outObj;
    }

    gAuth.ready = true;
    native_devauth_stats[0] = 8;
    return true;
}

}  // namespace

extern "C" int sheng_devauth_offline_token(const uint8_t uid[16], const uint8_t challenge[16], uint8_t token[16]) {
    if (!uid || !challenge || !token) return -22;
    while (__atomic_exchange_n(&gAuth.busy, 1, __ATOMIC_ACQUIRE) != 0) IOSleep(5);
    native_devauth_stats[4] = native_devauth_stats[4] + 1;
    int rc = -1;
    if (ensureReady()) {
        uint8_t req[QTEE_DEVAUTH_REQ_BYTES], rspIn[QTEE_DEVAUTH_REQ_BYTES];
        uint8_t reqOut[QTEE_DEVAUTH_REQ_BYTES], rspOut[QTEE_DEVAUTH_REQ_BYTES];
        qtee_build_devauth_offline_req(req, uid, challenge);
        memset(rspIn, 0, sizeof rspIn);
        memset(reqOut, 0, sizeof reqOut);
        memset(rspOut, 0, sizeof rspOut);
        const uint32_t is64 = 1u;
        const uint8_t empty = 0;
        const qtee_buf_desc ib[4] = {
            {req, nullptr, sizeof req},
            {rspIn, nullptr, sizeof rspIn},
            {&empty, nullptr, 0},
            {&is64, nullptr, sizeof is64},
        };
        const qtee_buf_desc ob[2] = {
            {nullptr, reqOut, sizeof reqOut},
            {nullptr, rspOut, sizeof rspOut},
        };
        const uint32_t io[4] = {QTEE_OBJ_NULL, QTEE_OBJ_NULL, QTEE_OBJ_NULL, QTEE_OBJ_NULL};
        uint32_t obSizes[2] = {0, 0};
        uint32_t qres = 0;
        if (qteeInvoke(gAuth.appController, QTEE_COMPAT_OP_SEND_REQ, ib, 4, ob, 2, obSizes,
                       io, 4, nullptr, 0, &qres) == 0 && obSizes[1] >= 24u) {
            uint32_t taStatus = 0;
            memcpy(&taStatus, rspOut, sizeof taStatus);
            native_devauth_stats[3] = static_cast<int32_t>(taStatus);
            if (taStatus == 0u || taStatus == QTEE_DEVAUTH_NO_FS_STATUS) {
                memcpy(token, rspOut + 8, 16);
                native_devauth_stats[5] = native_devauth_stats[5] + 1;
                rc = 0;
            } else {
                rc = -4;
            }
        } else {
            rc = -3;
        }
    }
    __atomic_store_n(&gAuth.busy, 0, __ATOMIC_RELEASE);
    return rc;
}
