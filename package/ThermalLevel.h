/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The thermal pressure level macOS gets from the board's temperatures: the wire format of the service thermalmonitord reads it from,
 * the mapping from ThermalProbe.h's group temperatures to a level, and the boot log row, as data and arithmetic a host test can run
 * (tests/thermallevel_host.c). ThermalLevel.cpp is the service.
 *
 * Where the level comes from (thermalmonitord of macOS 27.3 26C5035r, arm64e slice, addresses of that slice):
 *   - -[SensorDispatcherHelper init] (0x10000c0b0) takes IOServiceMatching("AppleSMCSensorDispatcher"), opens it with type 0 and asks
 *     selector 0 (one scalar out: the number of keys) and selector 1 (60 bytes out: fifteen 32-bit key codes). A key code is the
 *     four characters with the first one in the top byte (0x10000d06c turns a code into the string it files the key under).
 *   - a run loop timer of 1.0 s (created at 0x100011a1c, function 0x100011ed0) calls readKeysFromSensorDispatcher (0x10000c234):
 *     selector 2, 60 bytes out, fifteen 32-bit values in the order of the codes. There is no notification; the daemon polls.
 *   - the same timer then reaches -[... updateAllThermalLoad:] (0x10000ca48): getValueFromSMCForKey:@"mTPL" (0x10000c2f4: the value
 *     at the index of the key's code, -1 when the list has no such key) and, when the value differs from the last one (0 at start),
 *     from -1 and from -12700, updateThermalNotification: (0x10000f828), which is notify_set_state(token, value) and notify_post of
 *     kOSThermalNotificationPressureLevelName. The value is the OSThermalPressureLevel itself. Foundation's reader (cache address
 *     0x180bcbf98, table 0x181d451b0) turns 1 and 2 into NSProcessInfoThermalStateFair, 3 into Serious, 4 into Critical, anything
 *     else into Nominal.
 *   - it then reads Te3z Ts5z Ta1z Tp2z Tp3z Tp4z Tp5z Tp7z Tp8z Tp9z tGAM as floats for maxima it only reports; a key that is not
 *     in the list reads as -1.0. Selector 3 (88 bytes in) has no caller in this build.
 * Apple's service (com.apple.driver.AppleSMC in kernelcache.release.mac17j): AppleSMCSensorDispatcherUserClient::_methods
 * (0xfffffe000825acd8) has seven entries: 0 count (1 scalar out), 1 codes (60 out), 2 values (60 out), 3 write (88 in), 4 to 6 one
 * scalar out each (magnet, power pause, NFC state keys). Its initWithTask (0xfffffe0009afdc64) returns false unless
 * copyClientEntitlement(task, "com.apple.private.smcsensor.user-access") is kOSBooleanTrue. It reads every key of its device tree
 * node's "sensor" property (a Mac: "mTPLTe3zTs5zTa1zTp2zTp3zTp4zTp5zTp7zTp8zTp9ztGAM") from the SMC once a timer period and stores
 * -12700 for a key the SMC did not answer (readTemperatureKeysFromSMC, 0xfffffe0009af1448). */
#ifndef SHENG_THERMAL_LEVEL_H
#define SHENG_THERMAL_LEVEL_H
#include "ThermalProbe.h"

/* The service's selectors and sizes. */
enum {
    THERMAL_LEVEL_SEL_COUNT = 0, THERMAL_LEVEL_SEL_KEYS = 1, THERMAL_LEVEL_SEL_VALUES = 2, THERMAL_LEVEL_SEL_WRITE = 3,
    THERMAL_LEVEL_SELECTORS = 4,                /* answered here; Apple's 4 to 6 read SMC keys this board does not have */
    THERMAL_LEVEL_SLOTS = 15,
    THERMAL_LEVEL_ARRAY_BYTES = 4 * THERMAL_LEVEL_SLOTS,
    THERMAL_LEVEL_WRITE_BYTES = 88,             /* one count byte, then at 8 up to five pairs of a key code and a value, 8 bytes each */
    THERMAL_LEVEL_WRITE_PAIRS = 5,
    THERMAL_LEVEL_KEYS = 1                      /* the keys this service has */
};
#define THERMAL_LEVEL_KEY_MTPL 0x6d54504cu      /* 'mTPL' */
#define THERMAL_LEVEL_ENTITLEMENT "com.apple.private.smcsensor.user-access"

/* OSThermalPressureLevel of macOS (libkern/OSThermalNotification.h). */
enum {
    THERMAL_LEVEL_NOMINAL = 0, THERMAL_LEVEL_MODERATE = 1, THERMAL_LEVEL_HEAVY = 2, THERMAL_LEVEL_TRAPPING = 3, THERMAL_LEVEL_SLEEPING = 4
};
static const char *const thermal_level_name[5] = {"nominal", "moderate", "heavy", "trapping", "sleeping"};

/* The mapping. The inputs are the hottest core sensor of each CPU frequency domain and the hottest GPU sensor; the thresholds are the
 * trip points of those sensors' thermal zones in sm8550.dtsi, each with the zone's own hysteresis:
 *   CPU cores  90 (2) passive   95 (2) passive   110 (1) critical
 *   GPU        85 (1) passive   90 (1) hot       110 (1) critical      degrees Celsius (hysteresis)
 * An input's level is the number of its trip points crossed: the first gives moderate, the second heavy, the critical one trapping.
 * The two lower trips are where Linux starts and then tightens its own cooling on this chip, which is what moderate and heavy ask
 * of macOS; the critical trip is where Linux shuts down, and trapping is the strongest level that asks processes to shed load.
 * Sleeping is never reported: no trip point stands for it and the board does not sleep. A trip is crossed at its temperature and,
 * once crossed, stays crossed until the reading is below the temperature less the hysteresis (the thermal core's rule). The level
 * is the highest input level. An input without a reading counts as nominal and forgets what it had crossed, so an unreadable sensor
 * can neither raise the level nor hold it up. */
enum {THERMAL_LEVEL_INPUTS = 4, THERMAL_LEVEL_TRIPS = 3, THERMAL_LEVEL_STALE_ROUNDS = 3};
struct thermal_level_trip {int32_t mc, hysteresis_mc;};
static const uint8_t thermal_level_input_group[THERMAL_LEVEL_INPUTS] = {
    THERMAL_GROUP_CPU_D0, THERMAL_GROUP_CPU_D1, THERMAL_GROUP_CPU_D2, THERMAL_GROUP_GPU
};
static const struct thermal_level_trip thermal_level_cpu_trips[THERMAL_LEVEL_TRIPS] = {{90000, 2000}, {95000, 2000}, {110000, 1000}};
static const struct thermal_level_trip thermal_level_gpu_trips[THERMAL_LEVEL_TRIPS] = {{85000, 1000}, {90000, 1000}, {110000, 1000}};
static inline const struct thermal_level_trip *thermal_level_trips_of(unsigned input)
{
    return thermal_level_input_group[input] == THERMAL_GROUP_GPU ? thermal_level_gpu_trips : thermal_level_cpu_trips;
}

/* One input: the trip points crossed, given how many were crossed before. */
static inline unsigned thermal_level_input(unsigned crossed, int32_t mc, const struct thermal_level_trip *trips)
{
    unsigned level = 0;
    if (mc == THERMAL_NONE) return 0;
    for (unsigned t = 0; t < THERMAL_LEVEL_TRIPS; ++t) {
        const int32_t threshold = crossed > t ? trips[t].mc - trips[t].hysteresis_mc : trips[t].mc;
        if (mc < threshold) break;
        level = t + 1;
    }
    return level;
}

struct thermal_level_state {
    uint8_t crossed[THERMAL_LEVEL_INPUTS];      /* trip points each input has crossed */
    int32_t mc[THERMAL_LEVEL_INPUTS];           /* the readings the level was made from; THERMAL_NONE: not used */
    uint8_t level, highest;                     /* now, and the highest since start */
    uint32_t rounds, changes;                   /* levels computed, and how often the level changed */
    uint32_t last_samples, same_samples, stale; /* the probe's sample count, rounds it has not moved, rounds computed without readings */
};
static inline void thermal_level_init(struct thermal_level_state *s)
{
    for (unsigned i = 0; i < THERMAL_LEVEL_INPUTS; ++i) {s->crossed[i] = 0; s->mc[i] = THERMAL_NONE;}
    s->level = s->highest = 0;
    s->rounds = s->changes = s->last_samples = s->same_samples = s->stale = 0;
}

/* A new level from the probe's group temperatures (ThermalProbe.cpp native_thermal_now). sampled: the probe has sampled at least
 * once; samples: its sample count. The caller asks the probe for a sample before each call, so a count that has not moved for
 * THERMAL_LEVEL_STALE_ROUNDS calls in a row means the readings are old, and they are then not used. Returns the level. */
static inline unsigned thermal_level_step(struct thermal_level_state *s, const int32_t groups[THERMAL_GROUPS], int sampled, uint32_t samples)
{
    if (s->rounds && samples == s->last_samples) {
        if (s->same_samples < THERMAL_LEVEL_STALE_ROUNDS) s->same_samples++;
    } else s->same_samples = 0;
    s->last_samples = samples;
    const int usable = sampled && s->same_samples < THERMAL_LEVEL_STALE_ROUNDS;
    unsigned level = 0;
    for (unsigned i = 0; i < THERMAL_LEVEL_INPUTS; ++i) {
        const int32_t mc = usable ? groups[thermal_level_input_group[i]] : THERMAL_NONE;
        s->mc[i] = mc;
        s->crossed[i] = (uint8_t)thermal_level_input(s->crossed[i], mc, thermal_level_trips_of(i));
        if (s->crossed[i] > level) level = s->crossed[i];
    }
    if (!usable) s->stale++;
    if (level != s->level) s->changes++;
    if (level > s->highest) s->highest = (uint8_t)level;
    s->level = (uint8_t)level;
    s->rounds++;
    return level;
}

/* The answers of selectors 1 and 2: the key codes, and the values in the same order. Slots without a key are zero, as in Apple's. */
static inline void thermal_level_fill_keys(uint32_t out[THERMAL_LEVEL_SLOTS])
{
    for (unsigned i = 0; i < THERMAL_LEVEL_SLOTS; ++i) out[i] = 0;
    out[0] = THERMAL_LEVEL_KEY_MTPL;
}
static inline void thermal_level_fill_values(uint32_t out[THERMAL_LEVEL_SLOTS], unsigned level)
{
    for (unsigned i = 0; i < THERMAL_LEVEL_SLOTS; ++i) out[i] = 0;
    out[0] = level;
}

/* What the clients did, for the row. */
enum {THERMAL_LEVEL_CLIENTS = 4, THERMAL_LEVEL_WRITTEN = 4};
struct thermal_level_use {
    uint32_t opens, refused, closes;            /* refused: the caller lacks the entitlement */
    struct {int32_t pid; uint32_t reads;} client[THERMAL_LEVEL_CLIENTS];
    uint32_t other_reads;                       /* value reads by clients beyond the table */
    uint32_t counts, lists;                     /* selectors 0 and 1 */
    uint32_t writes, write_keys, bad_writes;    /* selector 3: requests, keys in them, requests with a bad count */
    uint32_t written[THERMAL_LEVEL_WRITTEN];    /* the last distinct key codes a client wrote: keys this service does not have */
    uint32_t unsupported, last_unsupported;     /* other selectors, and the last one */
};
/* One value read (selector 2) by pid. */
static inline void thermal_level_use_read(struct thermal_level_use *u, int32_t pid)
{
    for (unsigned i = 0; i < THERMAL_LEVEL_CLIENTS; ++i) {
        if (u->client[i].reads && u->client[i].pid != pid) continue;
        u->client[i].pid = pid;
        u->client[i].reads++;
        return;
    }
    u->other_reads++;
}
/* One write request (selector 3), 88 bytes. Returns 0 when its count is over the five pairs the structure holds. */
static inline int thermal_level_use_write(struct thermal_level_use *u, const uint8_t *request)
{
    const unsigned pairs = request[0];
    u->writes++;
    if (pairs > THERMAL_LEVEL_WRITE_PAIRS) {u->bad_writes++; return 0;}
    for (unsigned p = 0; p < pairs; ++p) {
        const uint8_t *at = request + 8 + 16 * p;
        const uint32_t key = (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
        unsigned known = 0;
        u->write_keys++;
        for (unsigned i = 0; i < THERMAL_LEVEL_WRITTEN; ++i) known |= u->written[i] == key;
        if (known) continue;
        for (unsigned i = THERMAL_LEVEL_WRITTEN - 1; i > 0; --i) u->written[i] = u->written[i - 1];
        u->written[0] = key;
    }
    return 1;
}

/* A key code as text; bytes that do not print become '?'. */
static inline unsigned thermal_level_put_key(char *row, unsigned n, unsigned size, uint32_t key)
{
    char text[5];
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned c = key >> (24 - 8 * i) & 0xff;
        text[i] = c > 0x20 && c < 0x7f ? (char)c : '?';
    }
    text[4] = 0;
    return thermal_put(row, n, size, "%s", text);
}

/* [thermal level] st=<state> level=<n> <name> from cpu0-2 <t> cpu3-6 <t> cpu7 <t> gpu <t> ("-": no reading used) | n=<levels
 * computed> high=<highest level> chg=<level changes> stale=<levels computed without readings> | open <opens>/<refused>/<closes> |
 * reads <pid>:<value reads> ... +<reads by further clients> | count <selector 0> list <selector 1> | write <requests>/<keys>/<bad>
 * <last key codes written> | other <unsupported selectors>/<the last one>
 * state: 1 published; -1 the temperature probe is not running; -2 no memory; -3 the service did not attach or start; -10, -11 the
 * classes did not register; -12 a class of that name exists already. */
static inline unsigned thermal_level_row(char *row, unsigned size, int state, const struct thermal_level_state *s,
                                         const struct thermal_level_use *u)
{
    unsigned n = thermal_put(row, 0, size, "[thermal level] st=%d level=%u %s from", state, s->level,
                             s->level < 5 ? thermal_level_name[s->level] : "?");
    for (unsigned i = 0; i < THERMAL_LEVEL_INPUTS; ++i) {
        n = thermal_put(row, n, size, " %s ", thermal_group_name[thermal_level_input_group[i]]);
        n = thermal_put_degrees(row, n, size, s->mc[i]);
    }
    n = thermal_put(row, n, size, " | n=%u high=%u chg=%u stale=%u | open %u/%u/%u | reads", s->rounds, s->highest, s->changes, s->stale,
                    u->opens, u->refused, u->closes);
    for (unsigned i = 0; i < THERMAL_LEVEL_CLIENTS; ++i)
        if (u->client[i].reads) n = thermal_put(row, n, size, " %d:%u", u->client[i].pid, u->client[i].reads);
    n = thermal_put(row, n, size, " +%u | count %u list %u | write %u/%u/%u", u->other_reads, u->counts, u->lists, u->writes, u->write_keys,
                    u->bad_writes);
    for (unsigned i = 0; i < THERMAL_LEVEL_WRITTEN; ++i)
        if (u->written[i]) {
            n = thermal_put(row, n, size, " ");
            n = thermal_level_put_key(row, n, size, u->written[i]);
        }
    return thermal_put(row, n, size, " | other %u/%u", u->unsupported, u->last_unsupported);
}
#endif
