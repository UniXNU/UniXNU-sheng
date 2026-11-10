/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The SM8550 temperature sensors, as data and arithmetic a host test can run (tests/thermal_host.c); ThermalProbe.cpp does the register
 * access. Everything here is read only.
 *
 * Three TSENS v2 controllers (sm8550.dtsi thermal-sensor@c271000, @c272000, @c273000, "qcom,tsens-v2", 16 sensors each). Each has a TM
 * block and an SROT block. tsens-v2.c: TM Sn_STATUS at 0xa0 + 4 * n holds the last temperature in bits 11:0 (signed, tenths of a degree
 * Celsius), the valid bit 21 and the threshold bits 16 to 20 (min, lower, upper, critical, max: 1 = violated); SROT 0x0 is the hardware
 * version (major in 31:28, minor in 27:16), SROT 0x4 the control word (bit 0 controller enabled, bits 18:3 one enable per sensor).
 * tsens.c reads a status word up to three times until the valid bit is set, and otherwise takes a value two consecutive reads agree on.
 *
 * What each sensor measures is the thermal-zones table of sm8550.dtsi. That table names sensors 4 to 7 of the third controller twice
 * (cdsp0..3 and gpuss-3..6) and gives no zone to sensors 4 to 7 of the second and 9 to 15 of the third; those are kept as unnamed. The
 * CPU numbers are the device tree's, which are XNU's: cpus 0 to 2 are frequency domain 0, cpus 3 to 6 domain 1, cpu 7 domain 2
 * (qcom,freq-domain in sm8550.dtsi). The zones' trip points there: CPU cores 90 and 95 degrees passive, 110 critical; GPU 85 passive,
 * 90 hot, 110 critical. */
#ifndef SHENG_THERMAL_PROBE_H
#define SHENG_THERMAL_PROBE_H
#include <stdint.h>
#include <stdarg.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif

enum {
    THERMAL_CONTROLLERS = 3, THERMAL_SENSORS = 16,
    THERMAL_TM_STATUS = 0xa0,                       /* + 4 * sensor */
    THERMAL_STATUS_TEMP_MASK = 0xfff, THERMAL_STATUS_VALID = 1u << 21,
    THERMAL_STATUS_FLAGS_SHIFT = 16, THERMAL_STATUS_FLAGS_MASK = 0x1f,   /* min, lower, upper, critical, max */
    THERMAL_SROT_VERSION = 0x0, THERMAL_SROT_CTRL = 0x4,
    THERMAL_CTRL_ENABLE = 1u << 0, THERMAL_CTRL_SENSOR_SHIFT = 3,
    THERMAL_WINDOW = 0x1000,
    THERMAL_READ_TRIES = 3,
    THERMAL_NONE = INT32_MIN                        /* no reading: sensor disabled, or no stable value */
};
/* TM and SROT bases of the three controllers. */
static const uint64_t thermal_tm_base[THERMAL_CONTROLLERS] = {0x0c271000, 0x0c272000, 0x0c273000};
static const uint64_t thermal_srot_base[THERMAL_CONTROLLERS] = {0x0c222000, 0x0c223000, 0x0c224000};

/* The groups a row reports: the hottest sensor of each. The first three are the CPU frequency domains in order. */
enum {
    THERMAL_GROUP_CPU_D0, THERMAL_GROUP_CPU_D1, THERMAL_GROUP_CPU_D2, THERMAL_GROUP_CPUSS, THERMAL_GROUP_GPU, THERMAL_GROUP_MODEM,
    THERMAL_GROUP_OTHER, THERMAL_GROUP_UNNAMED, THERMAL_GROUPS
};
static const char *const thermal_group_name[THERMAL_GROUPS] = {"cpu0-2", "cpu3-6", "cpu7", "cpuss", "gpu", "modem", "other", "unnamed"};

/* The group of each sensor, [controller][sensor]. */
#define TG(x) THERMAL_GROUP_##x
static const uint8_t thermal_group_of[THERMAL_CONTROLLERS][THERMAL_SENSORS] = {
    /* aoss0, cpuss0..3, cpu3 top/bottom, cpu4, cpu5, cpu6, cpu7 top/middle/bottom */
    {TG(OTHER), TG(CPUSS), TG(CPUSS), TG(CPUSS), TG(CPUSS), TG(CPU_D1), TG(CPU_D1), TG(CPU_D1), TG(CPU_D1), TG(CPU_D1), TG(CPU_D1),
     TG(CPU_D1), TG(CPU_D1), TG(CPU_D2), TG(CPU_D2), TG(CPU_D2)},
    /* aoss1, cpu0..2, four without a zone, video, mem, modem0..3, camera0..1 */
    {TG(OTHER), TG(CPU_D0), TG(CPU_D0), TG(CPU_D0), TG(UNNAMED), TG(UNNAMED), TG(UNNAMED), TG(UNNAMED), TG(OTHER), TG(OTHER), TG(MODEM),
     TG(MODEM), TG(MODEM), TG(MODEM), TG(OTHER), TG(OTHER)},
    /* aoss2, gpuss-0..7, seven without a zone */
    {TG(OTHER), TG(GPU), TG(GPU), TG(GPU), TG(GPU), TG(GPU), TG(GPU), TG(GPU), TG(GPU), TG(UNNAMED), TG(UNNAMED), TG(UNNAMED),
     TG(UNNAMED), TG(UNNAMED), TG(UNNAMED), TG(UNNAMED)},
};
#undef TG

/* Millidegrees Celsius of a status word's temperature field. */
static inline int32_t thermal_status_mc(uint32_t status)
{
    const uint32_t field = status & THERMAL_STATUS_TEMP_MASK;
    const int32_t tenths = (field & 0x800) ? (int32_t)field - 0x1000 : (int32_t)field;
    return tenths * 100;
}

/* One sensor from up to three reads of its status word, in the order read: the first valid one, otherwise the value two consecutive
 * reads agree on (the later of the two), otherwise none. Returns 1 with *mc set, 0 for none. */
static inline int thermal_pick(const uint32_t *status, unsigned reads, int32_t *mc)
{
    for (unsigned i = 0; i < reads; ++i)
        if (status[i] & THERMAL_STATUS_VALID) {*mc = thermal_status_mc(status[i]); return 1;}
    for (unsigned i = 1; i < reads; ++i)
        if ((status[i] & THERMAL_STATUS_TEMP_MASK) == (status[i - 1] & THERMAL_STATUS_TEMP_MASK)) {*mc = thermal_status_mc(status[i]); return 1;}
    return 0;
}

/* Whether a controller's control word enables a sensor. */
static inline int thermal_sensor_enabled(uint32_t ctrl, unsigned sensor)
{
    return (ctrl & THERMAL_CTRL_ENABLE) && ((ctrl >> (THERMAL_CTRL_SENSOR_SHIFT + sensor)) & 1u);
}

/* The hottest reading of each group; a group without a reading is THERMAL_NONE. */
static inline void thermal_groups(const int32_t mc[THERMAL_CONTROLLERS][THERMAL_SENSORS], int32_t out[THERMAL_GROUPS])
{
    for (unsigned g = 0; g < THERMAL_GROUPS; ++g) out[g] = THERMAL_NONE;
    for (unsigned c = 0; c < THERMAL_CONTROLLERS; ++c)
        for (unsigned s = 0; s < THERMAL_SENSORS; ++s) {
            const unsigned g = thermal_group_of[c][s];
            if (mc[c][s] != THERMAL_NONE && (out[g] == THERMAL_NONE || mc[c][s] > out[g])) out[g] = mc[c][s];
        }
}

/* The frequency limit against the temperature, per CPU frequency domain. A sample is busy when the domain's busiest CPU was at least
 * 90 % busy over the sampling interval, and limited when the domain then ran under 80 % of the frequency voted for it. The running
 * frequency also drops while a whole cluster idles, and it is read at one instant; a cluster with a CPU that busy is rarely all idle
 * at that instant (the reasoning of the governor's ramp count, CpuFreqProbe.cpp), which a half-busy one over these longer intervals
 * would be. Limited samples at a low temperature say the limit is not thermal; limited samples only near the trip points, and free
 * ones below them, say it is. Temperatures are the domain's hottest core sensor. */
struct thermal_limit {
    uint32_t busy, limited;                 /* samples */
    int32_t limited_min, limited_max;       /* millidegrees over the limited samples; THERMAL_NONE while there is none */
    int64_t limited_sum;
    int32_t free_max;                       /* over the busy samples that were not limited */
    int64_t free_sum;
    uint32_t lowest_khz;                    /* lowest running frequency in a limited sample */
};
static inline void thermal_limit_init(struct thermal_limit *l)
{
    l->busy = l->limited = 0;
    l->limited_min = l->limited_max = l->free_max = THERMAL_NONE;
    l->limited_sum = l->free_sum = 0;
    l->lowest_khz = 0;
}
/* busy_pm: the busiest CPU's busy share in per mille. Returns 1 when the sample counted as limited. */
static inline int thermal_limit_add(struct thermal_limit *l, unsigned busy_pm, uint32_t running_khz, uint32_t voted_khz, int32_t mc)
{
    if (busy_pm < 900 || !voted_khz || !running_khz || mc == THERMAL_NONE) return 0;
    l->busy++;
    if ((uint64_t)running_khz * 5 >= (uint64_t)voted_khz * 4) {
        if (l->free_max == THERMAL_NONE || mc > l->free_max) l->free_max = mc;
        l->free_sum += mc;
        return 0;
    }
    l->limited++;
    if (l->limited_min == THERMAL_NONE || mc < l->limited_min) l->limited_min = mc;
    if (l->limited_max == THERMAL_NONE || mc > l->limited_max) l->limited_max = mc;
    l->limited_sum += mc;
    if (!l->lowest_khz || running_khz < l->lowest_khz) l->lowest_khz = running_khz;
    return 1;
}
static inline int32_t thermal_limit_limited_mean(const struct thermal_limit *l)
{
    return l->limited ? (int32_t)(l->limited_sum / (int64_t)l->limited) : THERMAL_NONE;
}
static inline int32_t thermal_limit_free_mean(const struct thermal_limit *l)
{
    return l->busy > l->limited ? (int32_t)(l->free_sum / (int64_t)(l->busy - l->limited)) : THERMAL_NONE;
}

/* A frequency LUT row of an EPSS domain in kHz (qcom-cpufreq-hw.c: bits 31:30 the source, 7:0 the multiple of the 19.2 MHz reference;
 * source 0 is GPLL0 / 2). */
static inline uint32_t thermal_lut_khz(uint32_t row)
{
    return (row >> 30) ? 19200u * (row & 0xff) : 300000u;
}

/* The boot log's rows. Each function writes one row into row (size bytes, always terminated) and returns its length. */
static inline unsigned thermal_put(char *row, unsigned n, unsigned size, const char *format, ...) __attribute__((format(printf, 4, 5)));
static inline unsigned thermal_put(char *row, unsigned n, unsigned size, const char *format, ...)
{
    if (n + 1 >= size) return n;
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(row + n, size - n, format, arguments);
    va_end(arguments);
    if (written < 0) return n;
    return (unsigned)written >= size - n ? size - 1 : n + (unsigned)written;
}
/* "45.3" for 45300 millidegrees, "-" for no reading. */
static inline unsigned thermal_put_degrees(char *row, unsigned n, unsigned size, int32_t mc)
{
    if (mc == THERMAL_NONE) return thermal_put(row, n, size, "-");
    const int32_t tenths = mc / 100, magnitude = tenths < 0 ? -tenths : tenths;
    return thermal_put(row, n, size, "%s%d.%d", tenths < 0 ? "-" : "", magnitude / 10, magnitude % 10);
}
/* [thermal] n=<samples> then per group "<name> <now>/<hottest since boot>", degrees Celsius. */
static inline unsigned thermal_row_groups(char *row, unsigned size, uint32_t samples, const int32_t now[THERMAL_GROUPS],
                                          const int32_t peak[THERMAL_GROUPS])
{
    unsigned n = thermal_put(row, 0, size, "[thermal] n=%u", samples);
    for (unsigned g = 0; g < THERMAL_GROUPS; ++g) {
        n = thermal_put(row, n, size, " %s ", thermal_group_name[g]);
        n = thermal_put_degrees(row, n, size, now[g]);
        n = thermal_put(row, n, size, "/");
        n = thermal_put_degrees(row, n, size, peak[g]);
    }
    return n;
}
/* [thermal lmh] per frequency domain "d<n> <running>/<voted> MHz en<enable> ctl<dcvs control> irq<limit interrupt pending>
 * <0x300>/<0x304>/<0x30c>"; lmh is ThermalProbe.cpp's native_thermal_lmh. */
static inline unsigned thermal_row_lmh(char *row, unsigned size, const uint32_t lmh[3][8])
{
    unsigned n = thermal_put(row, 0, size, "[thermal lmh]");
    for (unsigned d = 0; d < 3; ++d)
        n = thermal_put(row, n, size, " d%u %u/%u MHz en%u ctl%x irq%u %x/%x/%x%s", d, lmh[d][0] / 1000, lmh[d][1] / 1000, lmh[d][2], lmh[d][3],
                        lmh[d][7], lmh[d][4], lmh[d][5], lmh[d][6], d < 2 ? " |" : "");
    return n;
}
/* [thermal limit] per frequency domain "d<n> <busy samples>/<limited> at <min>/<mean>/<max> free <mean>/<max> low <MHz>": the
 * temperature of the domain's hottest core over the limited samples, over the busy samples that were not limited, and the lowest
 * running frequency in a limited sample; limit is ThermalProbe.cpp's native_thermal_limit. */
static inline unsigned thermal_row_limit(char *row, unsigned size, const int32_t limit[3][8])
{
    unsigned n = thermal_put(row, 0, size, "[thermal limit]");
    for (unsigned d = 0; d < 3; ++d) {
        n = thermal_put(row, n, size, " d%u %d/%d at ", d, limit[d][0], limit[d][1]);
        n = thermal_put_degrees(row, n, size, limit[d][2]);
        n = thermal_put(row, n, size, "/");
        n = thermal_put_degrees(row, n, size, limit[d][3]);
        n = thermal_put(row, n, size, "/");
        n = thermal_put_degrees(row, n, size, limit[d][4]);
        n = thermal_put(row, n, size, " free ");
        n = thermal_put_degrees(row, n, size, limit[d][5]);
        n = thermal_put(row, n, size, "/");
        n = thermal_put_degrees(row, n, size, limit[d][6]);
        n = thermal_put(row, n, size, " low %d MHz%s", limit[d][7] / 1000, d < 2 ? " |" : "");
    }
    return n;
}
/* [thermal raw] t<controller> v=<SROT version> ctl=<SROT control> over=<threshold bits>: the sixteen sensors in tenths of a degree. */
static inline unsigned thermal_row_raw(char *row, unsigned size, unsigned controller, uint32_t version, uint32_t ctrl, uint32_t over,
                                       const int32_t mc[THERMAL_SENSORS])
{
    unsigned n = thermal_put(row, 0, size, "[thermal raw] t%u v=%x ctl=%x over=%x:", controller, version, ctrl, over);
    for (unsigned s = 0; s < THERMAL_SENSORS; ++s)
        n = mc[s] == THERMAL_NONE ? thermal_put(row, n, size, " -") : thermal_put(row, n, size, " %d", mc[s] / 100);
    return n;
}
#endif
