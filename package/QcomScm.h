/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Secure channel manager calls of the peripheral authentication service (PAS) in the SMCCC convention: the function id (standard call,
 * SMC64, SiP owner, service 2) and the argument-type word in x1. The SMC itself lives in the kernel glue; these encoders are what the
 * host tests check. */
#ifndef QCOM_SCM_H
#define QCOM_SCM_H
#include <stdint.h>

#define QCOM_SCM_SVC_PIL 0x02u
#define QCOM_SCM_PAS_INIT_IMAGE 0x01u
#define QCOM_SCM_PAS_MEM_SETUP 0x02u
#define QCOM_SCM_PAS_AUTH_AND_RESET 0x05u
#define QCOM_SCM_PAS_SHUTDOWN 0x06u
#define QCOM_SCM_PAS_IS_SUPPORTED 0x07u
#define QCOM_SCM_VAL 0u
#define QCOM_SCM_RO 1u
#define QCOM_SCM_RW 2u
#define QCOM_SCM_INTERRUPTED 1    /* x0 of a call the secure world wants resumed (x6 carries its session) */
#define QCOM_SCM_V2_EBUSY (-12)   /* busy: retry after a pause */

static inline uint32_t qcom_scm_fn(uint32_t svc, uint32_t cmd)
{
    return (1u << 30) /* SMC64 */ | (2u << 24) /* SiP */ | ((svc & 0xff) << 8) | (cmd & 0xff);
}
/* Argument count in bits 3:0, the type of argument i in bits 5+2i:4+2i. */
static inline uint32_t qcom_scm_args(unsigned count, unsigned t0, unsigned t1, unsigned t2)
{
    return (count & 0xf) | (t0 & 3) << 4 | (t1 & 3) << 6 | (t2 & 3) << 8;
}

/* IPCC (the inter-processor doorbells): a doorbell id is client << 16 | signal. */
#define QCOM_IPCC_SEND 0x0cu
#define QCOM_IPCC_RECV_ID 0x10u
#define QCOM_IPCC_RECV_ENABLE 0x14u
#define QCOM_IPCC_RECV_DISABLE 0x18u
#define QCOM_IPCC_RECV_CLEAR 0x1cu
#define QCOM_IPCC_NONE 0xffffffffu
#define QCOM_IPCC_CLIENT_AOP 0u
#define QCOM_IPCC_CLIENT_LPASS 3u
#define QCOM_IPCC_SIGNAL_GLINK_QMP 0u
#define QCOM_IPCC_SIGNAL_SMP2P 2u
static inline uint32_t qcom_ipcc_id(uint32_t client, uint32_t signal) {return client << 16 | (signal & 0xffff);}

#endif
