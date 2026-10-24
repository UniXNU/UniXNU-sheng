/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "QcomRpmh.h"

static uint16_t le16(const uint8_t *p) {return (uint16_t)(p[0] | (uint16_t)p[1] << 8);}
static uint32_t le32(const uint8_t *p) {return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16;}
static int fits(size_t at, size_t length, size_t size) {return at <= size && length <= size - at;}

/* Header: u32 version, u32 magic, 8 slave records of 16 bytes (u16 id, header offset, data offset, entry count, version, reserved[3]),
 * u32 checksum, u32 reserved, then the data area the offsets count from (byte 144). Entries are 24 bytes: an 8-byte id, two u32
 * priorities, u32 address, u16 aux length, u16 aux offset. */
int qcom_cmd_db_find(const uint8_t *db, size_t bytes, const char *name, QcomRpmhResource *out)
{
    if (!db || !name || !out) return QcomRpmhArgument;
    if (bytes < 144 || le32(db + 4) != QCOM_CMD_DB_MAGIC) return QcomRpmhNoDb;
    char id[8] = {0};
    for (unsigned i = 0; i < 8 && name[i]; ++i) id[i] = name[i];
    for (unsigned s = 0; s < 8; ++s) {
        const uint8_t *slave = db + 8 + 16 * s;
        if (!le16(slave)) break;
        const size_t heads = 144u + le16(slave + 2), data = 144u + le16(slave + 4), count = le16(slave + 6);
        if (!fits(heads, count * 24, bytes)) return QcomRpmhCorrupt;
        for (size_t e = 0; e < count; ++e) {
            const uint8_t *entry = db + heads + 24 * e;
            unsigned same = 1;
            for (unsigned i = 0; i < 8 && same; ++i) same = entry[i] == (uint8_t)id[i];
            if (!same) continue;
            QcomRpmhResource r = {0};
            r.address = le32(entry + 16);
            const size_t length = le16(entry + 20), at = data + le16(entry + 22);
            if (!r.address || !fits(at, length, bytes)) return QcomRpmhCorrupt;
            for (size_t l = 0; l + 1 < length && r.level_count < QCOM_RPMH_MAX_LEVELS; l += 2) {
                const uint16_t level = le16(db + at + l);
                if (r.level_count && !level) break;  /* the list is zero padded */
                r.levels[r.level_count++] = level;
            }
            *out = r;
            return QcomRpmhOk;
        }
    }
    return QcomRpmhMissing;
}

int qcom_rpmh_level_index(const QcomRpmhResource *r, uint32_t level, uint32_t *index)
{
    if (!r->level_count) return QcomRpmhNoLevel;
    if (!level) {*index = 0; return QcomRpmhOk;}
    for (unsigned i = 0; i < r->level_count; ++i)
        if (r->levels[i] >= level) {*index = i; return QcomRpmhOk;}
    *index = r->level_count - 1;
    return QcomRpmhOk;
}

QcomRscLayout qcom_rsc_layout(uint32_t drv_id)
{
    if (((drv_id >> 16) & 0xff) >= 3) return (QcomRscLayout){672, 24, 0x04, 0x08, 0x20, 0x24, 0x28, 0x2c, 0x34, 0x38, 0x3c};
    return (QcomRscLayout){672, 20, 0x04, 0x08, 0x10, 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38};
}

#define MSGID_WRITE_RESPONSE (8u | 1u << 8 | 1u << 16)  /* 4-byte payload code, response requested, write */
#define CONTROL_ACTIVE_MODE (1u << 16)
#define CONTROL_TRIGGER (1u << 24)

unsigned qcom_rpmh_active_request(const QcomRscLayout *l, unsigned tcs, const uint32_t *addresses, const uint32_t *values, unsigned count,
                                  QcomRpmhWrite *w, unsigned capacity)
{
    if (!count || count > 4 || capacity < 3 * count + 6) return 0;
    const uint32_t base = l->tcs_stride * tcs;
    unsigned n = 0;
    /* The control register is cleared before the TCS is reprogrammed, then set to active mode and only after that triggered. */
    w[n++] = (QcomRpmhWrite){base + l->control, 0};
    w[n++] = (QcomRpmhWrite){l->irq_clear, 1u << tcs};
    for (unsigned c = 0; c < count; ++c) {
        const uint32_t cmd = base + l->cmd_stride * c;
        w[n++] = (QcomRpmhWrite){cmd + l->cmd_msgid, MSGID_WRITE_RESPONSE};
        w[n++] = (QcomRpmhWrite){cmd + l->cmd_addr, addresses[c]};
        w[n++] = (QcomRpmhWrite){cmd + l->cmd_data, values[c]};
    }
    const uint32_t mask = (1u << count) - 1;
    w[n++] = (QcomRpmhWrite){base + l->wait_for_cmpl, mask};
    w[n++] = (QcomRpmhWrite){base + l->cmd_enable, mask};
    w[n++] = (QcomRpmhWrite){base + l->control, CONTROL_ACTIVE_MODE};
    w[n++] = (QcomRpmhWrite){base + l->control, CONTROL_ACTIVE_MODE | CONTROL_TRIGGER};
    return n;
}

unsigned qcom_rpmh_finish(const QcomRscLayout *l, unsigned tcs, QcomRpmhWrite *w, unsigned capacity)
{
    if (capacity < 3) return 0;
    const uint32_t base = l->tcs_stride * tcs;
    w[0] = (QcomRpmhWrite){l->irq_clear, 1u << tcs};
    w[1] = (QcomRpmhWrite){base + l->control, 0};
    w[2] = (QcomRpmhWrite){base + l->cmd_enable, 0};
    return 3;
}
