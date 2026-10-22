/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* RPMh for a remote processor's proxy votes: find a resource in the AOP command database (cmd-db) and turn a level into the index the
 * ARC resource takes, and describe the register writes of one active-only request in a TCS of the application processor's RSC, for
 * both register layouts (the RSC's major version 3 moved the TCS registers). Nothing here touches hardware: the owner applies the
 * writes and waits for the TCS done interrupt. */
#ifndef QCOM_RPMH_H
#define QCOM_RPMH_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define QCOM_CMD_DB_MAGIC 0x0c0330dbu
#define QCOM_RPMH_MAX_LEVELS 16u

typedef struct {
    uint32_t address;
    uint16_t levels[QCOM_RPMH_MAX_LEVELS];
    unsigned level_count;
} QcomRpmhResource;

enum {QcomRpmhOk = 0, QcomRpmhNoDb = -1, QcomRpmhMissing = -2, QcomRpmhCorrupt = -3, QcomRpmhNoLevel = -4, QcomRpmhArgument = -5};

/* Looks name (up to 8 characters) up in a copy of the command database; levels are the ARC's voltage levels (aux data) when it has
 * them. */
int qcom_cmd_db_find(const uint8_t *db, size_t bytes, const char *name, QcomRpmhResource *out);
/* The ARC level index for a request of at least level (the highest index when level is above all of them; 0 turns the vote off). */
int qcom_rpmh_level_index(const QcomRpmhResource *r, uint32_t level, uint32_t *index);

typedef struct {uint32_t offset, value;} QcomRpmhWrite;

/* The register layout of an RSC DRV; offsets are relative to the TCS block (DRV base + tcs-offset). */
typedef struct {
    uint32_t tcs_stride, cmd_stride, irq_status, irq_clear, wait_for_cmpl, control, status, cmd_enable, cmd_msgid, cmd_addr, cmd_data;
} QcomRscLayout;

/* The layout for a DRV whose id register reads drv_id (major version in bits 23:16). */
QcomRscLayout qcom_rsc_layout(uint32_t drv_id);
/* The writes of an active request of count (<= 4) data commands in TCS tcs, in order, ending with the trigger. Returns the number of
 * writes, 0 on bad arguments. */
unsigned qcom_rpmh_active_request(const QcomRscLayout *l, unsigned tcs, const uint32_t *addresses, const uint32_t *values, unsigned count,
                                  QcomRpmhWrite *writes, unsigned capacity);
/* The writes that return the TCS to idle after its done interrupt. */
unsigned qcom_rpmh_finish(const QcomRscLayout *l, unsigned tcs, QcomRpmhWrite *writes, unsigned capacity);

#ifdef __cplusplus
}
#endif
#endif
