/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The sheng board's wall clock, as register and arithmetic code a host test can run against a model of the SPMI PMIC arbiter
 * (tests/boardclock_host.c); BoardClock.cpp maps the arbiter and registers the clock.
 *
 * The PMK8550 has a real-time clock: a 32-bit seconds counter at 0x48 to 0x4b of peripheral 0x61 of SID 0, running while bit 7 of its
 * control register 0x46 is set (pmk8550.dtsi rtc@6100 "qcom,pmk8350-rtc"; rtc-pm8xxx.c pmk8350_regs: ctrl 0x6146, read 0x6148). The
 * counter is not wall time: it counts from the PMIC's last reset, and this side must not set it (the device tree does not allow
 * setting the time, and the peripheral's write registers are left alone). Wall time is the counter plus an offset, and Linux keeps
 * that offset for this board in four bytes, little endian, at 0xbc of the second SDAM (SID 0, peripheral 0x71): the "offset" nvmem
 * cell of sm8550-xiaomi-sheng.dts (rtc-offset@bc in pmk8550_sdam_2), read and written by rtc-pm8xxx.c as time = counter + offset.
 * The same SDAM holds the reboot reason byte at 0x48 that BootResetCore.h writes. An offset of 0 means none was stored.
 *
 * Both peripherals are reached through the arbiter's version 7 register layout (BootResetCore.h). A read goes through this execution
 * environment's observer channel and is allowed for every peripheral; a write goes through the peripheral's own channel and is
 * allowed only to the execution environment its ownership register names, which is the rule spmi-pmic-arb.c enforces before a write.
 * One command moves up to four bytes: the byte count less one in the command word's low bits and the bytes in the data register,
 * first byte lowest (spmi-pmic-arb.c pmic_arb_read_data, pmic_arb_write_data), so the cell changes in a single transfer. */
#ifndef SHENG_BOARD_CLOCK_H
#define SHENG_BOARD_CLOCK_H
#include <stdint.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif
#include "BootResetCore.h"

enum {
    BC_RTC_PPID = 0x061, BC_RTC_CONTROL = 0x46, BC_RTC_RUNNING = 0x80, BC_RTC_COUNTER = 0x48,
    BC_SDAM_PPID = 0x071, BC_OFFSET_CELL = 0xbc,
    BC_READ_TRIES = 4,
    BC_EARLIEST = 1672531200,      /* 2023-01-01 00:00 UTC: no wall time stored on this hardware is older than the hardware */
    BC_SET_BACK = 86400            /* a stored time may be this far after the time being set (a clock set slightly back) */
};
/* Results. The arbiter's own failures are 1 (no answer) and 2 (the transfer failed, was denied or dropped), as br_wait returns them. */
enum {
    BC_OK = 0,
    BC_UNSTABLE = 3,       /* no two reads in a row agreed */
    BC_EMPTY = 4,          /* the cell holds no offset */
    BC_RANGE = 5,          /* the time does not fit the 32-bit seconds the counter and the cell share */
    BC_STOPPED = 6,        /* the counter is not running */
    BC_NOT_OWNER = 7,      /* the cell's peripheral belongs to another execution environment */
    BC_MISMATCH = 8,       /* the cell read back differs from what was written */
    BC_FOREIGN = 9,        /* the cell holds something that is not an offset */
    BC_CURRENT = 10,       /* the cell already holds this offset */
    BC_NOT_TRIED = 0xff
};

/* A host test replaces the command write to run its model of the arbiter at that moment. */
#ifndef BC_COMMAND
#define BC_COMMAND(channel, word) ((channel)[BR_CMD / 4] = (word))
#endif

/* The APID of a peripheral (SID << 8 | peripheral) and the execution environment that owns it: the mapping this side owns when there
 * are several, as BootResetCore.h br_find chooses. Returns -1 when the arbiter maps no such peripheral. */
static inline int bc_find(const struct br_arbiter *a, uint32_t apids, uint32_t ppid, int *owner)
{
    int found = -1;
    for (uint32_t i = 0; i < apids; ++i) {
        const uint32_t map = a->core[(BR_APID_MAP + 4 * i) / 4];
        if (!map || ((map >> 8) & 0xfff) != ppid) continue;
        const int o = (int)(a->cnfg[(BR_CNFG_OWNER + 4 * i) / 4] & 7u);
        if (found < 0 || o == a->ee) {found = (int)i; *owner = o;}
    }
    return found;
}

/* One to four bytes of a peripheral through the observer channel, the first byte in the low bits. obs is the observer base. */
static inline int bc_read(volatile uint32_t *obs, const struct br_arbiter *a, int apid, uint8_t byte_addr, unsigned bytes, uint32_t *value)
{
    volatile uint32_t *ch = obs + (BR_OBS_EE_STRIDE * a->ee + BR_OBS_APID_STRIDE * (uint32_t)apid) / 4;
    BC_COMMAND(ch, br_cmd(BR_OP_EXT_READL, byte_addr, (uint8_t)(bytes - 1)));
    const int rc = br_wait(ch);
    if (rc) return rc;
    const uint32_t data = ch[BR_RDATA0 / 4];
    *value = bytes >= 4 ? data : data & ((1u << (8 * bytes)) - 1);
    return BC_OK;
}
/* The same value from two reads in a row. A second user of the same observer channel (the boot log samples these peripherals too)
 * could put its own transfer between a command and its data; two equal answers rule that out. */
static inline int bc_read_stable(volatile uint32_t *obs, const struct br_arbiter *a, int apid, uint8_t byte_addr, unsigned bytes, uint32_t *value)
{
    uint32_t previous = 0, next = 0;
    int rc = bc_read(obs, a, apid, byte_addr, bytes, &previous);
    for (unsigned i = 1; !rc && i < BC_READ_TRIES; ++i) {
        rc = bc_read(obs, a, apid, byte_addr, bytes, &next);
        if (rc) break;
        if (next == previous) {*value = next; return BC_OK;}
        previous = next;
    }
    return rc ? rc : BC_UNSTABLE;
}
/* The seconds counter. The PMIC does not latch the four bytes, so a read that straddles a carry is wrong in the upper bytes; like
 * rtc-pm8xxx.c this reads again, and takes the second of two reads that are equal or one second apart. */
static inline int bc_counter(volatile uint32_t *obs, const struct br_arbiter *a, int apid, uint32_t *seconds)
{
    uint32_t control = 0, previous = 0, next = 0;
    int rc = bc_read_stable(obs, a, apid, BC_RTC_CONTROL, 1, &control);
    if (rc) return rc;
    if (!(control & BC_RTC_RUNNING)) return BC_STOPPED;
    rc = bc_read(obs, a, apid, BC_RTC_COUNTER, 4, &previous);
    for (unsigned i = 1; !rc && i < BC_READ_TRIES; ++i) {
        rc = bc_read(obs, a, apid, BC_RTC_COUNTER, 4, &next);
        if (rc) break;
        if (next - previous <= 1) {*seconds = next; return BC_OK;}
        previous = next;
    }
    return rc ? rc : BC_UNSTABLE;
}

/* Wall time (seconds since 1970) from the counter and a stored offset. */
static inline int bc_time(uint32_t counter, uint32_t offset, uint64_t *seconds)
{
    if (!offset) return BC_EMPTY;
    const uint64_t sum = (uint64_t)counter + offset;
    if (sum > UINT32_MAX) return BC_RANGE;
    *seconds = sum;
    return BC_OK;
}
/* The offset that makes the counter read as seconds. 0 is not an offset (it means an empty cell). */
static inline int bc_offset(uint64_t seconds, uint32_t counter, uint32_t *offset)
{
    if (seconds > UINT32_MAX || seconds <= counter) return BC_RANGE;
    *offset = (uint32_t)(seconds - counter);
    return BC_OK;
}

/* What the cell holds, given the counter: nothing (BC_EMPTY), an offset (BC_OK), or something else (BC_FOREIGN). An offset makes
 * the counter read as a time between BC_EARLIEST and latest; a counter that restarted since the offset was stored reads earlier
 * than the true time, never later. latest is the time being set plus BC_SET_BACK when the system sets the clock, and UINT32_MAX
 * when nothing better is known (at boot). The store below replaces only an empty cell or an offset, so bytes that a firmware
 * keeps there for another purpose stay as they are. */
static inline int bc_cell(uint32_t counter, uint32_t cell, uint64_t latest)
{
    uint64_t seconds = 0;
    const int rc = bc_time(counter, cell, &seconds);
    if (rc == BC_EMPTY) return BC_EMPTY;
    if (rc || seconds < BC_EARLIEST || seconds > latest) return BC_FOREIGN;
    return BC_OK;
}

/* Writes the offset cell in one four-byte transfer and reads it back. channel is the write channel of the SDAM's APID (the arbiter's
 * channel base + BR_CHNL_APID_STRIDE * apid); owner is what bc_find reported for it. Nothing is written unless this side owns the
 * peripheral. *readback is the cell afterwards when it could be read. */
static inline int bc_store(volatile uint32_t *obs, volatile uint32_t *channel, const struct br_arbiter *a, int apid, int owner, uint32_t offset,
                           uint32_t *readback)
{
    if (owner != a->ee) return BC_NOT_OWNER;
    if (!offset) return BC_EMPTY;
    channel[BR_WDATA0 / 4] = offset;
    BC_COMMAND(channel, br_cmd(BR_OP_EXT_WRITEL, BC_OFFSET_CELL, 3));
    int rc = br_wait(channel);
    if (rc) return rc;
    rc = bc_read_stable(obs, a, apid, BC_OFFSET_CELL, 4, readback);
    if (rc) return rc;
    return *readback == offset ? BC_OK : BC_MISMATCH;
}

/* The boot log's row; v is BoardClock.cpp's native_board_clock. Returns the row's length.
 * [board clock] mode=<argument> st=<state bits> rtc=<APID>/<owner> sdam=<APID>/<owner> boot rc=<result> counter cell time
 * | set n=<times> secs counter offset err=<new offset - cell before, seconds> | store rc=<result> cell=<after> n=<stores> */
static inline unsigned bc_row(char *row, unsigned size, const uint32_t v[16])
{
    const int written = snprintf(row, size, "[board clock] mode=%u st=%x rtc=%u/%u sdam=%u/%u boot rc=%u counter=%u cell=%u time=%u | set n=%u secs=%u "
                                 "counter=%u offset=%u err=%d | store rc=%u cell=%u n=%u", v[1], v[0], v[2] & 0xffff, v[2] >> 16, v[3] & 0xffff,
                                 v[3] >> 16, v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], (int32_t)v[15], v[12], v[13], v[14]);
    return written < 0 ? 0 : (unsigned)written >= size ? size - 1 : (unsigned)written;
}
#endif
