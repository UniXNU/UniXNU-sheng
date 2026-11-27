/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Pure wire codec for Qualcomm QTEE SMCInvoke, SHMBridge, MinkIPC QSEEComCompat,
 * UFS RPMB listener (service 0x2000) and Xiaomi devauth offline token request (0x2020).
 * No kernel dependencies; host-tested under ASan/UBSan. */
#ifndef QCOM_TEE_WIRE_H
#define QCOM_TEE_WIRE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* SMCCC function IDs (SMC64, standard call). */
#define QCOM_SMCCC_SMC64_STD(owner, svc, cmd) \
    ((1u << 30) | (((uint32_t)(owner) & 0x3fu) << 24) | (((uint32_t)(svc) & 0xffu) << 8) | ((uint32_t)(cmd) & 0xffu))

#define QCOM_SCM_OWNER_SIP        2u
#define QCOM_SCM_OWNER_TRUSTED_OS 50u

#define QCOM_SCM_SVC_MP           0x0cu
#define QCOM_SCM_MP_SHM_ENABLE    0x1cu
#define QCOM_SCM_MP_SHM_DELETE    0x1du
#define QCOM_SCM_MP_SHM_CREATE    0x1eu

#define QCOM_SCM_SVC_SMCINVOKE    0x06u
#define QCOM_SCM_SMCINVOKE_CB_RSP 0x01u
#define QCOM_SCM_SMCINVOKE_INVOKE 0x02u

#define QCOM_SCM_FN_SHM_ENABLE    QCOM_SMCCC_SMC64_STD(QCOM_SCM_OWNER_SIP, QCOM_SCM_SVC_MP, QCOM_SCM_MP_SHM_ENABLE)
#define QCOM_SCM_FN_SHM_DELETE    QCOM_SMCCC_SMC64_STD(QCOM_SCM_OWNER_SIP, QCOM_SCM_SVC_MP, QCOM_SCM_MP_SHM_DELETE)
#define QCOM_SCM_FN_SHM_CREATE    QCOM_SMCCC_SMC64_STD(QCOM_SCM_OWNER_SIP, QCOM_SCM_SVC_MP, QCOM_SCM_MP_SHM_CREATE)
#define QCOM_SCM_FN_QTEE_INVOKE   QCOM_SMCCC_SMC64_STD(QCOM_SCM_OWNER_TRUSTED_OS, QCOM_SCM_SVC_SMCINVOKE, QCOM_SCM_SMCINVOKE_INVOKE)
#define QCOM_SCM_FN_QTEE_CB_RSP   QCOM_SMCCC_SMC64_STD(QCOM_SCM_OWNER_TRUSTED_OS, QCOM_SCM_SVC_SMCINVOKE, QCOM_SCM_SMCINVOKE_CB_RSP)

#define QCOM_SCM_ARG_VAL          0u
#define QCOM_SCM_ARG_RO           1u
#define QCOM_SCM_ARG_RW           2u
static inline uint32_t qcom_scm_args4(unsigned count, unsigned t0, unsigned t1, unsigned t2, unsigned t3)
{
    return (count & 0xfu) | ((t0 & 3u) << 4) | ((t1 & 3u) << 6) | ((t2 & 3u) << 8) | ((t3 & 3u) << 10);
}

#define QCOM_SCM_PERM_RW          0x6u
#define QCOM_SCM_VMID_HLOS        0x3u
#define QCOM_SHM_BRIDGE_VM_SHIFT  9u
#define QCOM_SHM_BRIDGE_NOTSUPP   4u

/* QTEE object IDs and transport operations. */
#define QTEE_OBJ_NULL             0u
#define QTEE_OBJ_ROOT             1u
#define QTEE_OBJ_NS_BIT           0x80000000u
#define QTEE_OBJ_PRIMORDIAL       QTEE_OBJ_NS_BIT

#define QTEE_OP_MASK              0xffffu
#define QTEE_OP_RETAIN            0xfffeu
#define QTEE_OP_RELEASE           0xffffu

#define QTEE_ROOT_OP_REG_WITH_CRED 5u
#define QTEE_CLIENT_ENV_OP_OPEN    0u
#define QTEE_UID_REG_LISTENER_CBO  87u
#define QTEE_UID_QSEECOM_LOADER    122u

#define QTEE_REG_CBO_OP_REGISTER   0u
#define QTEE_LOADER_OP_LOAD_BUF    1u
#define QTEE_COMPAT_OP_SEND_REQ    0u

#define QTEE_PRIM_OP_MAP_REGION    0u
#define QTEE_PRIM_OP_YIELD         1u
#define QTEE_PRIM_OP_SLEEP         2u

#define QTEE_CBO_OP_REQUEST        0u
#define QTEE_CBO_OP_WAIT           1u

#define QTEE_RES_INBOUND_REQ       3u

#define QTEE_MSG_OK                0u
#define QTEE_MSG_ERROR             1u
#define QTEE_MSG_ERROR_INVALID     2u
#define QTEE_MSG_ERROR_UNAVAIL     0xffffffa3u /* -93 */

#define QTEE_ASYNC_VER_MAJOR_1     1u

/* Wire message headers (little-endian). */
union qtee_msg_arg {
    struct { uint32_t offset; uint32_t size; } b;
    uint32_t o;
};

struct qtee_msg_invoke {
    uint32_t cxt;
    uint32_t op;
    uint32_t counts;
    union qtee_msg_arg args[];
};

struct qtee_msg_callback {
    uint32_t result;
    uint32_t cxt;
    uint32_t op;
    uint32_t counts;
    union qtee_msg_arg args[];
};

struct qtee_mapping_info {
    uint64_t paddr;
    uint64_t len;
    uint32_t perms;
} __attribute__((packed));

struct qtee_async_hdr {
    uint32_t version;
    uint32_t op;
};

struct qtee_async_release {
    struct qtee_async_hdr hdr;
    uint32_t count;
    uint32_t ids[];
};

static inline uint32_t qtee_align8(uint32_t n) { return (n + 7u) & ~7u; }
static inline uint32_t qtee_pack_counts(unsigned ib, unsigned ob, unsigned io, unsigned oo)
{
    return (ib & 0xfu) | ((ob & 0xfu) << 4) | ((io & 0xfu) << 8) | ((oo & 0xfu) << 12);
}
static inline unsigned qtee_num_ib(uint32_t c) { return c & 0xfu; }
static inline unsigned qtee_num_ob(uint32_t c) { return (c >> 4) & 0xfu; }
static inline unsigned qtee_num_io(uint32_t c) { return (c >> 8) & 0xfu; }
static inline unsigned qtee_num_oo(uint32_t c) { return (c >> 12) & 0xfu; }
static inline unsigned qtee_total_args(uint32_t c)
{
    return qtee_num_ib(c) + qtee_num_ob(c) + qtee_num_io(c) + qtee_num_oo(c);
}

struct qtee_buf_desc {
    const void *in_ptr;
    void *out_ptr;
    uint32_t size;
};

/* Pack an inbound direct invocation message into `buf` (size `cap`). Returns used size or 0 on overflow. */
static inline uint32_t qtee_pack_invoke(void *buf, uint32_t cap, uint32_t cxt, uint32_t op,
                                        const struct qtee_buf_desc *ib, unsigned nib,
                                        const struct qtee_buf_desc *ob, unsigned nob,
                                        const uint32_t *io, unsigned nio, unsigned noo)
{
    const unsigned total = nib + nob + nio + noo;
    uint32_t off = qtee_align8((uint32_t)(sizeof(struct qtee_msg_invoke) + total * sizeof(union qtee_msg_arg)));
    if (off > cap) return 0;
    uint32_t need = off;
    for (unsigned i = 0; i < nib; ++i) {
        const uint32_t a = qtee_align8(ib[i].size);
        if (a > cap - need) return 0;
        need += a;
    }
    for (unsigned i = 0; i < nob; ++i) {
        const uint32_t a = qtee_align8(ob[i].size);
        if (a > cap - need) return 0;
        need += a;
    }
    memset(buf, 0, need);
    struct qtee_msg_invoke *m = (struct qtee_msg_invoke *)buf;
    m->cxt = cxt;
    m->op = op;
    m->counts = qtee_pack_counts(nib, nob, nio, noo);
    unsigned idx = 0;
    for (unsigned i = 0; i < nib; ++i, ++idx) {
        m->args[idx].b.offset = off;
        m->args[idx].b.size = ib[i].size;
        if (ib[i].size && ib[i].in_ptr) memcpy((uint8_t *)buf + off, ib[i].in_ptr, ib[i].size);
        off += qtee_align8(ib[i].size);
    }
    for (unsigned i = 0; i < nob; ++i, ++idx) {
        m->args[idx].b.offset = off;
        m->args[idx].b.size = ob[i].size;
        off += qtee_align8(ob[i].size);
    }
    for (unsigned i = 0; i < nio; ++i, ++idx) {
        m->args[idx].o = io[i];
    }
    return need;
}

/* Unpack output buffers and output objects from a completed inbound invocation buffer. */
static inline int qtee_unpack_invoke(const void *buf, uint32_t cap,
                                     const struct qtee_buf_desc *ob, unsigned nob, uint32_t *ob_sizes,
                                     uint32_t *oo, unsigned noo)
{
    if (cap < sizeof(struct qtee_msg_invoke)) return -1;
    const struct qtee_msg_invoke *m = (const struct qtee_msg_invoke *)buf;
    const unsigned nib = qtee_num_ib(m->counts);
    const unsigned m_nob = qtee_num_ob(m->counts);
    const unsigned nio = qtee_num_io(m->counts);
    const unsigned m_noo = qtee_num_oo(m->counts);
    if (m_nob < nob || m_noo < noo) return -1;
    for (unsigned i = 0; i < nob; ++i) {
        const uint32_t off = m->args[nib + i].b.offset;
        const uint32_t sz = m->args[nib + i].b.size;
        if (off > cap || sz > cap - off) return -1;
        if (sz > ob[i].size) return -1;
        if (sz && ob[i].out_ptr) memcpy(ob[i].out_ptr, (const uint8_t *)buf + off, sz);
        if (ob_sizes) ob_sizes[i] = sz;
    }
    for (unsigned i = 0; i < noo; ++i) {
        oo[i] = m->args[nib + m_nob + nio + i].o;
    }
    return 0;
}

/* RPMB listener (service 0x2000) wire protocol. */
#define QTEE_RPMB_SERVICE_ID      0x2000u
#define QTEE_RPMB_MAX_DATA_LEN    20000u
#define QTEE_RPMB_MAX_BUF_LEN     (QTEE_RPMB_MAX_DATA_LEN + 40u)
#define QTEE_RPMB_BUF_ALLOC       24576u /* 6 pages, page-aligned */
#define QTEE_RPMB_BLK_SIZE        512u
#define QTEE_RPMB_DEV_UFS         9u     /* device_id_type::UFS_RPMB */

#define QTEE_RPMB_CMD_INIT        0x101u
#define QTEE_RPMB_CMD_READ        0x102u
#define QTEE_RPMB_CMD_WRITE       0x103u
#define QTEE_RPMB_CMD_PARTITION   0x104u

struct qtee_rpmb_init_req {
    uint32_t cmd_id;
    uint32_t version;
} __attribute__((packed));

struct qtee_rpmb_init_res_v1 {
    uint32_t cmd_id;
    uint32_t version;
    int32_t status;
    uint32_t num_sectors;
    uint32_t rel_wr_count;
} __attribute__((packed));

struct qtee_rpmb_init_res_v2 {
    uint32_t cmd_id;
    uint32_t version;
    int32_t status;
    uint32_t num_sectors;
    uint32_t rel_wr_count;
    uint32_t dev_type;
    uint32_t reserved[4];
} __attribute__((packed));

struct qtee_rpmb_rw_req {
    uint32_t cmd_id;
    uint32_t num_sectors;
    uint32_t req_buff_len;
    uint32_t req_buff_offset;
    uint32_t version;
    uint32_t rel_wr_count;
} __attribute__((packed));

struct qtee_rpmb_rw_res {
    uint32_t cmd_id;
    int32_t status;
    uint32_t res_buff_len;
    uint32_t res_buff_offset;
    uint32_t version;
} __attribute__((packed));

struct qtee_rpmb_part_req {
    uint32_t cmd_id;
    uint32_t version;
    uint32_t dev_id;
} __attribute__((packed));

struct qtee_rpmb_part_res {
    uint32_t cmd_id;
    int32_t status;
    uint32_t num_partitions;
    uint32_t rsp_buff_offset;
} __attribute__((packed));

/* Parse UFS Device (idn=0), Geometry (idn=7) and RPMB Unit (idn=2, index=0xC4) descriptors. */
static inline int qtee_ufs_rpmb_params(const uint8_t *dev_desc, uint32_t dev_len,
                                       const uint8_t *geo_desc, uint32_t geo_len,
                                       const uint8_t *unit_desc, uint32_t unit_len,
                                       uint32_t *sectors_512, uint32_t *rel_wr_count)
{
    if (dev_len < 18 || geo_len < 24 || unit_len < 20) return -1;
    const uint16_t spec_ver = (uint16_t)((uint16_t)dev_desc[16] << 8 | dev_desc[17]);
    const uint32_t rel = geo_desc[23];
    if (!rel) return -1;
    uint32_t secs;
    if (spec_ver < 0x0300u) {
        const uint32_t blks = ((uint32_t)unit_desc[15] << 24) | ((uint32_t)unit_desc[16] << 16) |
                              ((uint32_t)unit_desc[17] << 8) | unit_desc[18];
        secs = blks / 2u;
    } else {
        secs = (uint32_t)unit_desc[19] * 256u;
    }
    if (!secs) return -1;
    *sectors_512 = secs;
    *rel_wr_count = rel;
    return 0;
}

static inline void qtee_build_rpmb_cdb(uint8_t cdb[12], uint8_t opcode, uint32_t xfer_bytes)
{
    memset(cdb, 0, 12);
    cdb[0] = opcode; /* 0xB5 SECURITY PROTOCOL OUT, 0xA2 SECURITY PROTOCOL IN */
    cdb[1] = 0xecu;  /* Security Protocol: JEDEC UFS */
    cdb[2] = 0x00u;
    cdb[3] = 0x01u;  /* Security Protocol Specific: RPMB (0x0001) */
    cdb[6] = (uint8_t)(xfer_bytes >> 24);
    cdb[7] = (uint8_t)(xfer_bytes >> 16);
    cdb[8] = (uint8_t)(xfer_bytes >> 8);
    cdb[9] = (uint8_t)(xfer_bytes & 0xffu);
}

/* Xiaomi devauth.mbn cmd 0x2020 (devauth_OfflineToken_get) 128-byte request builder. */
#define QTEE_DEVAUTH_REQ_BYTES    128u
#define QTEE_DEVAUTH_CMD_OFFLINE  0x2020u
#define QTEE_DEVAUTH_NO_FS_STATUS 0xaa00000du

static inline void qtee_build_devauth_offline_req(uint8_t out[QTEE_DEVAUTH_REQ_BYTES],
                                                  const uint8_t uid[16],
                                                  const uint8_t challenge[16])
{
    static const char hex[16] = {'0','1','2','3','4','5','6','7','8','9','A','B','C','D','E','F'};
    static const char keymeta[8] = {'0','0','0','0','0','0','0','2'};
    memset(out, 0, QTEE_DEVAUTH_REQ_BYTES);
    /* *(uint64_t *)(out + 0) = 1; *(uint32_t *)(out + 4) = 0x2020; */
    out[0] = 0x01u;
    out[4] = (uint8_t)(QTEE_DEVAUTH_CMD_OFFLINE & 0xffu);
    out[5] = (uint8_t)((QTEE_DEVAUTH_CMD_OFFLINE >> 8) & 0xffu);
    for (unsigned i = 0; i < 16; ++i) {
        out[12 + i * 2] = (uint8_t)hex[uid[i] >> 4];
        out[12 + i * 2 + 1] = (uint8_t)hex[uid[i] & 0xfu];
    }
    memcpy(out + 12 + 32, keymeta, 8);
    memcpy(out + 12 + 40, challenge, 16);
}

#endif /* QCOM_TEE_WIRE_H */
