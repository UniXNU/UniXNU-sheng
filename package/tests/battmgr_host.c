// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the battery manager codec, refresh cycle and power source mapping (package/ShengBattmgr.c) against a scripted firmware.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ShengBattmgr.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)
static uint32_t le32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static void put32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}

/* The scripted firmware: battery and USB property tables; a property marked failing answers with result 1. */
static int32_t battery[24], usb[8], board[256];
static uint32_t usb_set[8], usb_set_count, usb_set_result;
static int board_answers;  /* 0: the firmware refuses the board properties */
static uint32_t failing_battery = 0xffffffffu;
static unsigned asked_battery[24], asked_usb[8];
static uint32_t answer(const uint8_t *request, uint8_t *out)
{
    CHECK(le32(request) == SHENG_BATTMGR_OWNER && le32(request + 4) == SHENG_BATTMGR_TYPE_REQUEST && le32(request + 12) == 0);
    const uint32_t opcode = le32(request + 8), property = le32(request + 16);
    put32(out, SHENG_BATTMGR_OWNER); put32(out + 4, SHENG_BATTMGR_TYPE_REQUEST); put32(out + 8, opcode);
    put32(out + 12, property);
    if (opcode == SHENG_BATTMGR_OP_BATTERY_GET) {
        CHECK(property < 24);
        asked_battery[property]++;
        put32(out + 16, (uint32_t)battery[property]); put32(out + 20, property == failing_battery);
    } else if (opcode == SHENG_BATTMGR_OP_BOARD_GET) {
        CHECK(property < 256);
        put32(out + 16, (uint32_t)board[property]); put32(out + 20, !board_answers);
    } else if (opcode == SHENG_BATTMGR_OP_USB_SET) {
        CHECK(property < 8);
        usb_set[property] = le32(request + 20);
        usb_set_count++;
        if (!usb_set_result) usb[property] = (int32_t)usb_set[property];
        put32(out + 16, usb_set[property]); put32(out + 20, usb_set_result);
    } else {
        CHECK(opcode == SHENG_BATTMGR_OP_USB_GET && property < 8);
        asked_usb[property]++;
        put32(out + 16, (uint32_t)usb[property]); put32(out + 20, 0);
    }
    return 24;
}
/* Runs a cycle to its end; returns the number of requests. */
static unsigned run_cycle(ShengBattmgr *b)
{
    uint8_t request[SHENG_BATTMGR_REQUEST_BYTES], reply[24], spare[24];
    unsigned n = 0;
    for (;;) {
        const uint32_t length = sheng_battmgr_next(b, request);
        if (!length) break;
        CHECK(length == 24);
        CHECK(sheng_battmgr_next(b, spare) == 0);  /* one in flight */
        answer(request, reply);
        const int r = sheng_battmgr_receive(b, reply, 24);
        ++n;
        if (r == BmCycleDone) break;
        CHECK(r == BmAnswer);
    }
    return n;
}

int main(void)
{
    ShengBattmgr b;
    ShengBatterySnapshot s;
    uint8_t enable[SHENG_BATTMGR_ENABLE_BYTES], reply[24], request[24];
    CHECK(sheng_battmgr_enable_request(enable) == 28);
    CHECK(le32(enable) == 32778 && le32(enable + 4) == 1 && le32(enable + 8) == 4 && le32(enable + 20) == 1 && le32(enable + 24) == 100);

    /* On battery: 87.5 %, 3.95 V, -812 mA, 29.5 C, 3200 of 8600 mAh, design 8800, 37 cycles, 6 h left. */
    battery[0] = 2; battery[2] = 1; battery[4] = 8750; battery[7] = 3950000; battery[9] = 812000; battery[13] = 2950;
    battery[15] = 3200000; battery[18] = 8600000; battery[17] = 8800000; battery[16] = 37; battery[21] = 6 * 3600; battery[20] = -1;
    usb[0] = 0;
    sheng_battmgr_init(&b);
    CHECK(sheng_battmgr_next(&b, request) == 0);  /* no cycle yet */
    sheng_battmgr_refresh(&b, 0);                  /* the first cycle reads the slow fields anyway */
    CHECK(run_cycle(&b) == BmFields && b.cycles_done == 1);
    sheng_battmgr_snapshot(&b, &s);
    CHECK(s.present && !s.external && !s.charging && !s.fully_charged && !s.critical);
    CHECK(s.percent == 88 && s.charge_known && s.voltage_mv == 3950 && s.current_ma == -812 && s.temperature == 2950 && s.temperature_valid);
    CHECK(s.raw_mah == 3200 && s.full_mah == 8600 && s.design_mah == 8800 && s.cycles == 37);
    CHECK(s.time_to_empty_min == 360 && s.time_to_full_min == -1 && s.adapter_family == 0 && s.adapter_watts == 0);

    /* A plain refresh skips full, design, cycles and the authentication of both cells. */
    memset(asked_battery, 0, sizeof asked_battery);
    sheng_battmgr_refresh(&b, 0);
    CHECK(run_cycle(&b) == BmFields - 5 && !asked_battery[16] && !asked_battery[17] && !asked_battery[18]);
    CHECK(asked_battery[13] && asked_battery[15] && asked_battery[20] && asked_battery[21] && !asked_battery[19] && !asked_battery[14]);
    sheng_battmgr_refresh(&b, 1);
    CHECK(run_cycle(&b) == BmFields);

    /* On a PD charger, charging: 9 V at 2.6 A in, 23 W; then full. */
    battery[0] = 1; usb[0] = 1; usb[6] = 6; usb[1] = 9000000; usb[3] = -2600000; battery[9] = -2400000; battery[20] = 1800;
    sheng_battmgr_refresh(&b, 0);
    run_cycle(&b);
    sheng_battmgr_snapshot(&b, &s);
    CHECK(s.external && s.charging && !s.fully_charged && s.current_ma == 2400 && s.time_to_full_min == 30);
    CHECK(s.adapter_family == 0xe000400au && s.adapter_mv == 9000 && s.adapter_ma == 2600 && s.adapter_watts == 23);
    battery[0] = 4; battery[4] = 10000;
    sheng_battmgr_refresh(&b, 0);
    run_cycle(&b);
    sheng_battmgr_snapshot(&b, &s);
    CHECK(s.fully_charged && !s.charging && s.percent == 100);
    usb[6] = 2;  /* a dedicated charging port */
    sheng_battmgr_refresh(&b, 0);
    run_cycle(&b);
    sheng_battmgr_snapshot(&b, &s);
    CHECK(s.adapter_family == 0xe0004004u);

    /* Critical on battery at 2 %. */
    usb[0] = 0; battery[0] = 2; battery[4] = 180;
    sheng_battmgr_refresh(&b, 0);
    run_cycle(&b);
    sheng_battmgr_snapshot(&b, &s);
    CHECK(s.percent == 2 && s.critical);

    /* A failing property is an error and stays invalid; the cycle goes on. */
    failing_battery = 13;
    const uint32_t errors_before = b.errors;  /* the eight board properties this firmware refuses count too */
    sheng_battmgr_refresh(&b, 0);
    run_cycle(&b);
    sheng_battmgr_snapshot(&b, &s);
    CHECK(b.errors == errors_before + 9 && s.temperature == 0 && !((b.valid >> BmTemperature) & 1));
    failing_battery = 0xffffffffu;

    /* The fixed out-of-range temperature the firmware answers without a reading is no temperature. */
    battery[13] = 2000000;
    sheng_battmgr_refresh(&b, 0);
    run_cycle(&b);
    sheng_battmgr_snapshot(&b, &s);
    CHECK(!s.temperature_valid && s.temperature == 0);
    battery[13] = -4500;
    sheng_battmgr_refresh(&b, 0);
    run_cycle(&b);
    sheng_battmgr_snapshot(&b, &s);
    CHECK(!s.temperature_valid);
    battery[13] = 2950;

    /* With the fuel gauges answering, the charge is their remaining over full: (1000 + 1000) / (5000 + 4800) = 20 %, whatever the
     * capacity property says; a gauge that stops answering falls back to the property. */
    board_answers = 1;
    board[0x8a] = 1000; board[0x8b] = 5000; board[0xa6] = 1000; board[0xa7] = 4800;
    battery[4] = 100;
    sheng_battmgr_refresh(&b, 0);
    run_cycle(&b);
    sheng_battmgr_snapshot(&b, &s);
    CHECK(s.percent == 20 && s.charge_known && !s.critical);
    board[0xa6] = 4800;  /* rounds: 5800 * 100 / 9800 = 59.18 */
    sheng_battmgr_refresh(&b, 0);
    run_cycle(&b);
    sheng_battmgr_snapshot(&b, &s);
    CHECK(s.percent == 59);
    board[0x8a] = 5000; board[0xa6] = 4900;  /* more than full reads 100 */
    sheng_battmgr_refresh(&b, 0);
    run_cycle(&b);
    sheng_battmgr_snapshot(&b, &s);
    CHECK(s.percent == 100);
    board_answers = 0;
    sheng_battmgr_refresh(&b, 0);
    run_cycle(&b);
    sheng_battmgr_snapshot(&b, &s);
    CHECK(s.percent == 1 && s.charge_known && s.critical);
    battery[4] = 180;

    /* A timeout skips the field in flight. */
    sheng_battmgr_refresh(&b, 0);
    CHECK(sheng_battmgr_next(&b, request) == 24 && le32(request + 16) == 0);
    sheng_battmgr_timeout(&b);
    CHECK(b.timeouts == 1 && !((b.valid >> BmStatus) & 1));
    CHECK(sheng_battmgr_next(&b, request) == 24 && le32(request + 16) == 2);
    /* An answer to something else, a short one and another owner's are ignored and leave the request in flight. */
    const uint32_t ignored = b.ignored;
    answer(request, reply);
    put32(reply + 12, 9);
    CHECK(sheng_battmgr_receive(&b, reply, 24) == BmIgnored);
    CHECK(sheng_battmgr_receive(&b, reply, 11) == BmIgnored);
    put32(reply, 32779);
    CHECK(sheng_battmgr_receive(&b, reply, 24) == BmIgnored && b.ignored == ignored + 3);
    answer(request, reply);
    CHECK(sheng_battmgr_receive(&b, reply, 24) == BmAnswer);
    /* A notification during the cycle runs another cycle right after it. */
    uint8_t note[16];
    put32(note, SHENG_BATTMGR_OWNER); put32(note + 4, SHENG_BATTMGR_TYPE_NOTIFY); put32(note + 8, SHENG_BATTMGR_OP_NOTIFICATION);
    put32(note + 12, 0x80);
    CHECK(sheng_battmgr_receive(&b, note, 16) == BmNotified && b.refresh_wanted);
    const uint32_t done = b.cycles_done;
    run_cycle(&b);
    CHECK(b.cycles_done == done + 1 && !b.refresh_wanted && b.cursor == 0 && !b.in_flight);
    run_cycle(&b);
    CHECK(b.cycles_done == done + 2 && sheng_battmgr_next(&b, request) == 0);
    /* With no cycle running, a notification starts one at once; the enable answer is recognized. */
    CHECK(sheng_battmgr_receive(&b, note, 16) == BmNotified && sheng_battmgr_next(&b, request) == 24);
    uint8_t enabled[24];
    memcpy(enabled, enable, 12);
    CHECK(sheng_battmgr_receive(&b, enabled, 24) == BmEnabled && b.enabled);
    /* A timeout on the last field ends the cycle. */
    sheng_battmgr_init(&b);
    sheng_battmgr_refresh(&b, 1);
    for (unsigned i = 0; i < BmFields; ++i) {CHECK(sheng_battmgr_next(&b, request) == 24); sheng_battmgr_timeout(&b);}
    CHECK(b.cycles_done == 1 && b.timeouts == BmFields && sheng_battmgr_next(&b, request) == 0);
    sheng_battmgr_snapshot(&b, &s);
    CHECK(!s.present && !s.external && s.percent == 0 && s.time_to_empty_min == -1);

    /* A computer's standard port (SDP) at 100 mA: after the cycle the limit goes up to 900 mA, once; the next cycle reads it back. */
    sheng_battmgr_init(&b);
    usb[0] = 1; usb[6] = 1; usb[7] = 1; usb[5] = 100000; usb_set_count = 0; usb_set_result = 0;
    sheng_battmgr_refresh(&b, 1);
    CHECK(run_cycle(&b) == BmFields && b.limit_due == 900000);
    CHECK(sheng_battmgr_next(&b, request) == 24 && le32(request + 8) == SHENG_BATTMGR_OP_USB_SET && le32(request + 16) == 5);
    CHECK(le32(request + 20) == 900000 && sheng_battmgr_next(&b, reply) == 0);  /* one in flight */
    answer(request, reply);
    CHECK(sheng_battmgr_receive(&b, reply, 24) == BmAnswer && b.limit_sets == 1 && !b.limit_result && b.cursor == 0);
    run_cycle(&b);
    CHECK(usb_set_count == 1 && b.value[BmUsbLimit] == 900000 && !b.limit_due);
    sheng_battmgr_refresh(&b, 0);
    run_cycle(&b);
    CHECK(usb_set_count == 1);
    /* A firmware that keeps a lower limit (its own current limiting) is asked three times per port type, no more. */
    usb[5] = 500000; usb_set_result = 0;
    for (unsigned i = 0; i < 6; ++i) {
        sheng_battmgr_refresh(&b, 0);
        run_cycle(&b);
        usb[5] = 500000;
        if (b.limit_due) {CHECK(sheng_battmgr_next(&b, request) == 24); answer(request, reply); usb[5] = 500000; sheng_battmgr_receive(&b, reply, 24);}
    }
    CHECK(usb_set_count == 3 && b.limit_tries == 3);
    /* A charging downstream port (CDP) asks for 1.5 A, tries counted afresh; a dedicated charger and an unset value ask for nothing. */
    usb[7] = 3; usb[6] = 3;
    sheng_battmgr_refresh(&b, 0);
    run_cycle(&b);
    CHECK(b.limit_due == 1500000 && b.limit_tries == 0);
    /* a refused set and a timed-out one count as tries */
    usb_set_result = 7;
    CHECK(sheng_battmgr_next(&b, request) == 24); answer(request, reply);
    CHECK(sheng_battmgr_receive(&b, reply, 24) == BmAnswer && b.limit_result == 7 && b.limit_sets == 3);
    run_cycle(&b);
    CHECK(b.limit_due == 1500000 && sheng_battmgr_next(&b, request) == 24);
    sheng_battmgr_timeout(&b);
    CHECK(b.limit_result == -60 && !b.limit_in_flight && b.limit_tries == 2);
    CHECK(sheng_battmgr_receive(&b, reply, 24) == BmIgnored);  /* an answer to a set nobody waits for */
    usb[7] = 2; usb[6] = 2;
    sheng_battmgr_refresh(&b, 0);
    run_cycle(&b);
    CHECK(!b.limit_due);
    sheng_battmgr_init(&b);
    b.sdp_limit_ua = 0;
    usb[7] = 1; usb[6] = 1; usb[5] = 100000;
    sheng_battmgr_refresh(&b, 1);
    run_cycle(&b);
    CHECK(!b.limit_due && sheng_battmgr_next(&b, request) == 0);
    printf("PASS battery manager: %u checks\n", checks);
    return 0;
}
