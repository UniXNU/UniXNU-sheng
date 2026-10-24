/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "ShengBattmgr.h"

/* Battery properties as this board's firmware numbers them (one more after charge control limit max than the common list): status 0,
 * health 1, present 2, charge type 3, capacity 4 (hundredths of a percent), SOH 5, OCV 6, voltage now 7 (uV), voltage max 8, current
 * now 9 (uA), thermal level 10, its max 11, charge current 12 (uA), temperature 13 (hundredths of a degree), technology 14, charge
 * counter 15 (uAh), cycle count 16, full design 17 (uAh), full 18 (uAh), model 19 (a string), time to full 20 (s), time to empty 21 (s).
 * USB: online 0, voltage now 1 (uV), voltage max 2, current now 3 (uA), current max 4, input current limit 5, type 6, adapter type 7.
 * Board: battery authenticated 3, adapter type 7, PD adapter authenticated 0x18, input suspended 0x2a, adapter watts 0x33, the fuel
 * gauges 0x8a/0x8b and 0xa6/0xa7, second cell authenticated 0xa5. */
static const struct {uint8_t opcode, property, slow;} kFields[BmFields] = {
    [BmStatus] = {SHENG_BATTMGR_OP_BATTERY_GET, 0, 0},
    [BmPresent] = {SHENG_BATTMGR_OP_BATTERY_GET, 2, 0},
    [BmCapacity] = {SHENG_BATTMGR_OP_BATTERY_GET, 4, 0},
    [BmVoltage] = {SHENG_BATTMGR_OP_BATTERY_GET, 7, 0},
    [BmCurrent] = {SHENG_BATTMGR_OP_BATTERY_GET, 9, 0},
    [BmTemperature] = {SHENG_BATTMGR_OP_BATTERY_GET, 13, 0},
    [BmCharge] = {SHENG_BATTMGR_OP_BATTERY_GET, 15, 0},
    [BmUsbOnline] = {SHENG_BATTMGR_OP_USB_GET, 0, 0},
    [BmUsbType] = {SHENG_BATTMGR_OP_USB_GET, 6, 0},
    [BmUsbVoltage] = {SHENG_BATTMGR_OP_USB_GET, 1, 0},
    [BmUsbCurrent] = {SHENG_BATTMGR_OP_USB_GET, 3, 0},
    [BmTimeToEmpty] = {SHENG_BATTMGR_OP_BATTERY_GET, 21, 0},
    [BmTimeToFull] = {SHENG_BATTMGR_OP_BATTERY_GET, 20, 0},
    /* the fuel gauges: remaining and full charge of each (mAh) */
    [BmGauge1Remaining] = {SHENG_BATTMGR_OP_BOARD_GET, 0x8a, 0},
    [BmGauge1Full] = {SHENG_BATTMGR_OP_BOARD_GET, 0x8b, 0},
    [BmGauge2Remaining] = {SHENG_BATTMGR_OP_BOARD_GET, 0xa6, 0},
    [BmGauge2Full] = {SHENG_BATTMGR_OP_BOARD_GET, 0xa7, 0},
    [BmFull] = {SHENG_BATTMGR_OP_BATTERY_GET, 18, 1},
    [BmDesign] = {SHENG_BATTMGR_OP_BATTERY_GET, 17, 1},
    [BmCycles] = {SHENG_BATTMGR_OP_BATTERY_GET, 16, 1},
    [BmUsbAdapter] = {SHENG_BATTMGR_OP_USB_GET, 7, 0},
    [BmUsbLimit] = {SHENG_BATTMGR_OP_USB_GET, 5, 0},
    [BmUsbMax] = {SHENG_BATTMGR_OP_USB_GET, 4, 0},
    [BmChargeType] = {SHENG_BATTMGR_OP_BATTERY_GET, 3, 0},
    [BmThermalLevel] = {SHENG_BATTMGR_OP_BATTERY_GET, 10, 0},
    [BmChargeCurrent] = {SHENG_BATTMGR_OP_BATTERY_GET, 12, 0},
    [BmRealType] = {SHENG_BATTMGR_OP_BOARD_GET, 0x07, 0},
    [BmPowerMax] = {SHENG_BATTMGR_OP_BOARD_GET, 0x33, 0},
    [BmPdVerified] = {SHENG_BATTMGR_OP_BOARD_GET, 0x18, 0},
    [BmAuthentic] = {SHENG_BATTMGR_OP_BOARD_GET, 0x03, 1},
    [BmSlaveAuthentic] = {SHENG_BATTMGR_OP_BOARD_GET, 0xa5, 1},
    [BmInputSuspend] = {SHENG_BATTMGR_OP_BOARD_GET, 0x2a, 0},
};
#define NO_CYCLE 0xff

static void put32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static void header(uint8_t *out, uint32_t opcode)
{
    put32(out, SHENG_BATTMGR_OWNER);
    put32(out + 4, SHENG_BATTMGR_TYPE_REQUEST);
    put32(out + 8, opcode);
}

void sheng_battmgr_init(ShengBattmgr *b)
{
    for (unsigned i = 0; i < sizeof *b; ++i) ((uint8_t *)b)[i] = 0;
    b->cursor = NO_CYCLE;
    b->last_timeout = -1;
    b->sdp_limit_ua = 900000;  /* a configured USB 3 port's 900 mA; the firmware's current limiting backs off if the port sags */
    b->limit_port = -1;
}

uint32_t sheng_battmgr_enable_request(uint8_t out[SHENG_BATTMGR_ENABLE_BYTES])
{
    header(out, SHENG_BATTMGR_OP_REQUEST_NOTIFICATION);
    put32(out + 12, 0);    /* battery 0 */
    put32(out + 16, 0);    /* power state */
    put32(out + 20, 1);    /* low capacity notification threshold, percent */
    put32(out + 24, 100);  /* high */
    return SHENG_BATTMGR_ENABLE_BYTES;
}

static void skip_to_wanted(ShengBattmgr *b)
{
    while (b->cursor < BmFields && kFields[b->cursor].slow && !b->slow_due) b->cursor++;
}

static void advance(ShengBattmgr *b)
{
    b->in_flight = 0;
    b->cursor++;
    skip_to_wanted(b);
}

void sheng_battmgr_refresh(ShengBattmgr *b, int slow)
{
    if (b->cursor != NO_CYCLE) {b->refresh_wanted = 1 + (slow != 0); return;}
    b->slow_due = slow || !(b->valid & (1u << BmFull));
    b->cursor = 0;
    b->in_flight = 0;
    skip_to_wanted(b);
}

uint32_t sheng_battmgr_next(ShengBattmgr *b, uint8_t out[SHENG_BATTMGR_REQUEST_BYTES])
{
    if (b->in_flight || b->limit_in_flight) return 0;
    if (b->limit_due) {
        header(out, SHENG_BATTMGR_OP_USB_SET);
        put32(out + 12, 0);
        put32(out + 16, 5);  /* input current limit */
        put32(out + 20, b->limit_due);
        b->limit_in_flight = b->limit_due;
        b->limit_due = 0;
        b->limit_tries++;
        return SHENG_BATTMGR_REQUEST_BYTES;
    }
    if (b->cursor == NO_CYCLE || b->cursor >= BmFields) return 0;
    header(out, kFields[b->cursor].opcode);
    put32(out + 12, 0);
    put32(out + 16, kFields[b->cursor].property);
    put32(out + 20, 0);
    b->in_flight = 1;
    return SHENG_BATTMGR_REQUEST_BYTES;
}

/* The input current limit a port type gets from the host side: a standard port (SDP 1) the configured value, a charging downstream port
 * (CDP 3) its 1.5 A; others (dedicated chargers, Type-C current, PD contracts) are the firmware's own. */
static uint32_t wanted_limit(const ShengBattmgr *b, int port)
{
    return port == 1 ? b->sdp_limit_ua : port == 3 ? 1500000u : 0u;
}

/* After a cycle: a set is due when the port wants a higher limit than the firmware reports, at most three tries per port type. */
static void check_limit(ShengBattmgr *b)
{
    const int port = (b->valid >> BmUsbAdapter) & 1 ? b->value[BmUsbAdapter] : (b->valid >> BmUsbType) & 1 ? b->value[BmUsbType] : 0;
    if (port != b->limit_port) {b->limit_port = port; b->limit_tries = 0;}
    const uint32_t want = wanted_limit(b, port);
    if (!want || b->limit_tries >= 3) return;
    if ((b->valid >> BmUsbLimit) & 1 && b->value[BmUsbLimit] >= (int32_t)want - 100000) return;  /* 100 mA under it: input limiting */
    b->limit_due = want;
}

/* Ends the cycle when the cursor ran past the last field; starts the remembered one. Returns 1 when a cycle ended. */
static int finish(ShengBattmgr *b)
{
    if (b->cursor == NO_CYCLE || b->cursor < BmFields) return 0;
    b->cursor = NO_CYCLE;
    b->cycles_done++;
    check_limit(b);
    if (b->refresh_wanted) {
        const int slow = b->refresh_wanted == 2;
        b->refresh_wanted = 0;
        sheng_battmgr_refresh(b, slow);
    }
    return 1;
}

void sheng_battmgr_timeout(ShengBattmgr *b)
{
    if (b->limit_in_flight) {b->limit_in_flight = 0; b->limit_result = -60; b->timeouts++; return;}
    if (b->cursor == NO_CYCLE || !b->in_flight) return;
    b->timeouts++;
    b->last_timeout = b->cursor;
    b->valid &= ~(1u << b->cursor);
    advance(b);
    finish(b);
}

int sheng_battmgr_receive(ShengBattmgr *b, const uint8_t *data, uint32_t length)
{
    if (length < 12 || le32(data) != SHENG_BATTMGR_OWNER) {b->ignored++; return BmIgnored;}
    const uint32_t type = le32(data + 4), opcode = le32(data + 8);
    if (opcode == SHENG_BATTMGR_OP_NOTIFICATION) {
        b->notifications++;
        sheng_battmgr_refresh(b, 0);
        return BmNotified;
    }
    if (type != SHENG_BATTMGR_TYPE_REQUEST) {b->ignored++; return BmIgnored;}
    if (opcode == SHENG_BATTMGR_OP_REQUEST_NOTIFICATION) {b->enabled = 1; return BmEnabled;}
    if (opcode == SHENG_BATTMGR_OP_USB_SET) {
        if (!b->limit_in_flight || length < 24) {b->ignored++; return BmIgnored;}
        b->limit_result = (int32_t)le32(data + 20);
        if (!b->limit_result) b->limit_sets++;
        b->limit_in_flight = 0;
        sheng_battmgr_refresh(b, 0);  /* read the limit back */
        return BmAnswer;
    }
    if (b->cursor == NO_CYCLE || b->cursor >= BmFields || !b->in_flight || length < 24) {b->ignored++; return BmIgnored;}
    const uint32_t property = le32(data + 12), value = le32(data + 16), result = le32(data + 20);
    if (opcode != kFields[b->cursor].opcode || property != kFields[b->cursor].property) {b->ignored++; return BmIgnored;}
    b->responses++;
    if (result) {
        b->errors++;
        b->valid &= ~(1u << b->cursor);
    } else {
        b->value[b->cursor] = (int32_t)value;
        b->valid |= 1u << b->cursor;
    }
    advance(b);
    return finish(b) ? BmCycleDone : BmAnswer;
}

/* POWER_SUPPLY_USB_TYPE (unknown 0, SDP 1, DCP 2, CDP 3, ACA 4, C 5, PD 6, PD_DRP 7, PD_PPS 8) to the kIOPSFamilyCode values (the
 * USB family's error space, 0xe0004000 + n: host 0, dedicated charging port 4, downstream charging port 5, unknown 7, Type-C 9, PD 10). */
static uint32_t family(int type)
{
    switch (type) {
    case 1: return 0xe0004000u;
    case 2: return 0xe0004004u;
    case 3: return 0xe0004005u;
    case 5: return 0xe0004009u;
    case 6: case 7: case 8: return 0xe000400au;
    default: return 0xe0004007u;
    }
}

void sheng_battmgr_snapshot(const ShengBattmgr *b, ShengBatterySnapshot *s)
{
#define HAS(f) ((b->valid >> (f)) & 1u)
#define V(f) (b->value[(f)])
    const int status = HAS(BmStatus) ? V(BmStatus) : 0;  /* POWER_SUPPLY_STATUS: 1 charging, 2 discharging, 3 not charging, 4 full */
    s->present = HAS(BmPresent) ? V(BmPresent) != 0 : HAS(BmCapacity);
    s->external = HAS(BmUsbOnline) && V(BmUsbOnline) != 0;
    const int gauges = HAS(BmGauge1Remaining) && HAS(BmGauge1Full) && HAS(BmGauge2Remaining) && HAS(BmGauge2Full);
    const int64_t remaining = gauges ? (int64_t)(uint32_t)V(BmGauge1Remaining) + (uint32_t)V(BmGauge2Remaining) : 0;
    const int64_t full = gauges ? (int64_t)(uint32_t)V(BmGauge1Full) + (uint32_t)V(BmGauge2Full) : 0;
    int percent = full > 0 ? (int)((remaining * 100 + full / 2) / full) : HAS(BmCapacity) ? (V(BmCapacity) + 50) / 100 : 0;
    s->percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    s->charge_known = full > 0 || HAS(BmCapacity);
    s->charging = s->external && status == 1;
    s->fully_charged = s->external && (status == 4 || s->percent == 100);
    s->critical = s->present && !s->external && s->percent <= 2;
    s->voltage_mv = HAS(BmVoltage) ? V(BmVoltage) / 1000 : 0;
    s->current_ma = HAS(BmCurrent) ? -V(BmCurrent) / 1000 : 0;
    /* -40 to 90 C; the firmware answers a fixed out-of-range value when it has no reading */
    s->temperature_valid = HAS(BmTemperature) && V(BmTemperature) >= -4000 && V(BmTemperature) <= 9000;
    s->temperature = s->temperature_valid ? V(BmTemperature) : 0;
    s->raw_mah = HAS(BmCharge) ? V(BmCharge) / 1000 : 0;
    s->full_mah = HAS(BmFull) ? V(BmFull) / 1000 : 0;
    s->design_mah = HAS(BmDesign) ? V(BmDesign) / 1000 : 0;
    s->cycles = HAS(BmCycles) ? V(BmCycles) : 0;
    /* The gauge reports times in seconds, and a negative or huge value when it has none. */
    s->time_to_empty_min = HAS(BmTimeToEmpty) && V(BmTimeToEmpty) > 0 && V(BmTimeToEmpty) < 100 * 3600 ? V(BmTimeToEmpty) / 60 : -1;
    s->time_to_full_min = HAS(BmTimeToFull) && V(BmTimeToFull) > 0 && V(BmTimeToFull) < 100 * 3600 ? V(BmTimeToFull) / 60 : -1;
    if (s->external) {
        s->adapter_family = family(HAS(BmUsbType) ? V(BmUsbType) : 0);
        s->adapter_mv = HAS(BmUsbVoltage) ? V(BmUsbVoltage) / 1000 : 0;
        s->adapter_ma = HAS(BmUsbCurrent) ? V(BmUsbCurrent) / 1000 : 0;
        if (s->adapter_mv < 0) s->adapter_mv = 0;
        if (s->adapter_ma < 0) s->adapter_ma = -s->adapter_ma;
        s->adapter_watts = (int)(((int64_t)s->adapter_mv * s->adapter_ma + 500000) / 1000000);
    } else {
        s->adapter_family = 0;
        s->adapter_mv = s->adapter_ma = s->adapter_watts = 0;
    }
#undef HAS
#undef V
}
