/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../QcomTeeWire.h"
#include "../Wn8030.h"

int main(void)
{
    /* SMCCC function IDs and argument encodings */
    assert(QCOM_SCM_FN_SHM_ENABLE == 0x42000c1cu);
    assert(QCOM_SCM_FN_SHM_DELETE == 0x42000c1du);
    assert(QCOM_SCM_FN_SHM_CREATE == 0x42000c1eu);
    assert(QCOM_SCM_FN_QTEE_INVOKE == 0x72000602u);
    assert(QCOM_SCM_FN_QTEE_CB_RSP == 0x72000601u);
    assert(qcom_scm_args4(4, QCOM_SCM_ARG_RW, QCOM_SCM_ARG_VAL, QCOM_SCM_ARG_RW, QCOM_SCM_ARG_VAL) == 0x224u);
    assert(qcom_scm_args4(2, QCOM_SCM_ARG_RW, QCOM_SCM_ARG_VAL, 0, 0) == 0x22u);
    assert(qcom_scm_args4(4, QCOM_SCM_ARG_VAL, QCOM_SCM_ARG_VAL, QCOM_SCM_ARG_VAL, QCOM_SCM_ARG_VAL) == 0x4u);

    /* Packed struct sizes */
    assert(sizeof(struct qtee_mapping_info) == 20);
    assert(sizeof(struct qtee_rpmb_init_req) == 8);
    assert(sizeof(struct qtee_rpmb_init_res_v1) == 20);
    assert(sizeof(struct qtee_rpmb_init_res_v2) == 40);
    assert(sizeof(struct qtee_rpmb_rw_req) == 24);
    assert(sizeof(struct qtee_rpmb_rw_res) == 20);
    assert(sizeof(struct qtee_rpmb_part_req) == 12);
    assert(sizeof(struct qtee_rpmb_part_res) == 16);

    /* QTEE invoke pack + unpack round-trip */
    uint8_t wire[512];
    uint32_t uid_arg = QTEE_UID_QSEECOM_LOADER;
    uint8_t ob_out[32] = {0};
    struct qtee_buf_desc ib[1] = {{.in_ptr = &uid_arg, .size = sizeof uid_arg}};
    struct qtee_buf_desc ob[1] = {{.out_ptr = ob_out, .size = sizeof ob_out}};
    uint32_t io[1] = {0x80000001u};
    uint32_t used = qtee_pack_invoke(wire, sizeof wire, 7u, 1u, ib, 1, ob, 1, io, 1, 1);
    assert(used == 48u + 8u + 32u); /* hdr 12 + 4*8=44 -> align8=48, ib 4->8, ob 32->32 */
    struct qtee_msg_invoke *m = (struct qtee_msg_invoke *)wire;
    assert(m->cxt == 7u && m->op == 1u && m->counts == qtee_pack_counts(1, 1, 1, 1));
    assert(m->args[0].b.offset == 48u && m->args[0].b.size == 4u);
    assert(m->args[1].b.offset == 56u && m->args[1].b.size == 32u);
    assert(m->args[2].o == 0x80000001u);
    /* Simulate QTEE response */
    memcpy(wire + 56u, "OK-DIST", 7);
    m->args[1].b.size = 7u;
    m->args[3].o = 42u;
    uint32_t ob_sz = 0, oo = 0;
    assert(qtee_unpack_invoke(wire, sizeof wire, ob, 1, &ob_sz, &oo, 1) == 0);
    assert(ob_sz == 7u && memcmp(ob_out, "OK-DIST", 7) == 0 && oo == 42u);

    /* UFS RPMB params parsing (UFS 3.x / 4.0) */
    uint8_t dev[0x40] = {0}, geo[0x48] = {0}, unit[0x23] = {0};
    dev[16] = 0x04; dev[17] = 0x00; /* UFS 4.0 */
    geo[23] = 32; /* bRPMB_ReadWriteSize */
    unit[19] = 128; /* 16 MiB region 0 -> 128 * 256 = 32768 512B sectors */
    uint32_t secs = 0, rel = 0;
    assert(qtee_ufs_rpmb_params(dev, sizeof dev, geo, sizeof geo, unit, sizeof unit, &secs, &rel) == 0);
    assert(secs == 32768u && rel == 32u);

    /* RPMB CDB builder */
    uint8_t cdb[12];
    qtee_build_rpmb_cdb(cdb, 0xb5u, 512u);
    assert(cdb[0] == 0xb5u && cdb[1] == 0xecu && cdb[2] == 0x00u && cdb[3] == 0x01u);
    assert(cdb[6] == 0 && cdb[7] == 0 && cdb[8] == 2 && cdb[9] == 0);

    /* devauth 0x2020 request */
    uint8_t req[QTEE_DEVAUTH_REQ_BYTES];
    const uint8_t uid[16] = {0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef,0xfe,0xdc,0xba,0x98,0x76,0x54,0x32,0x10};
    const uint8_t chal[16] = {0xaa,0xbb,0xcc,0xdd,1,2,3,4,5,6,7,8,9,10,11,12};
    qtee_build_devauth_offline_req(req, uid, chal);
    uint64_t r0 = 0; uint32_t r4 = 0;
    memcpy(&r0, req, 8); memcpy(&r4, req + 4, 4);
    assert((r0 & 0xffffffffu) == 1u && r4 == 0x2020u);
    assert(memcmp(req + 12, "0123456789ABCDEFFEDCBA987654321000000002", 40) == 0);
    assert(memcmp(req + 52, chal, 16) == 0);

    /* WN8030 MiAuth frames match map220v/sm8550-mainline byte-for-byte */
    uint8_t cmd[1 + WN8030_COMMAND_BYTES];
    assert(wn8030_auth_init(cmd) == 67u);
    assert(cmd[0] == 0x5cu && cmd[1] == 0x32u && cmd[2] == 0x00u && cmd[3] == 0x4fu && cmd[4] == 0x31u);
    assert(cmd[5] == 0x80u && cmd[6] == 0x38u && cmd[7] == 0x31u && cmd[8] == 0x06u);
    assert(memcmp(cmd + 9, "MIAUTH", 6) == 0);
    assert(cmd[15] == wn8030_sum8(cmd + 3, 12u));

    const uint8_t km[4] = {0, 0, 0, 2};
    assert(wn8030_auth_s3t1(cmd, km, chal) == 67u);
    assert(cmd[3] == 0x4fu && cmd[7] == 0x32u && cmd[8] == 0x14u);
    assert(memcmp(cmd + 9, km, 4) == 0 && memcmp(cmd + 13, chal, 16) == 0);
    assert(cmd[29] == wn8030_sum8(cmd + 3, 26u));

    assert(wn8030_auth_s5t1(cmd, uid) == 67u);
    assert(cmd[3] == 0x4fu && cmd[7] == 0x33u && cmd[8] == 0x10u);
    assert(memcmp(cmd + 9, uid, 16) == 0);
    assert(cmd[25] == wn8030_sum8(cmd + 3, 22u));

    /* WN8030 MiAuth inbound parsing */
    uint8_t f[WN8030_FRAME_BYTES] = {0};
    Wn8030Frame out;
    f[0] = 0x57; f[2] = 0x39; f[3] = 0x22; f[4] = 0x31; f[5] = 0x38; f[6] = 0x80; f[7] = 0x24; f[8] = 0x01; f[9] = 0x64;
    assert(wn8030_parse(f, &out) == Wn8030AuthRequest && out.auth_state == 0x64u);
    f[3] = 0x23; f[7] = 0x31; memcpy(f + 11, uid, 16);
    assert(wn8030_parse(f, &out) == Wn8030AuthInit && memcmp(out.auth_uid, uid, 16) == 0);
    f[3] = 0x24; f[7] = 0x32; memcpy(f + 25, chal, 16);
    assert(wn8030_parse(f, &out) == Wn8030AuthChallenge && memcmp(out.auth_challenge, chal, 16) == 0);

    puts("PASS qcom_tee_wire_host");
    return 0;
}
