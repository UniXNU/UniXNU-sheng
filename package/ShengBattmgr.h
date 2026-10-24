/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The battery manager the audio DSP runs (PMIC GLINK, channel "PMIC_RTR_ADSP_APPS"): messages start with a 12-byte header (owner, type,
 * opcode); a property read is a request {battery 0, property, 0} answered by {property, value, result}; the DSP also sends notifications
 * when something changed. This is the message codec, the refresh cycle (one request in flight, the next one only when it is answered
 * or the owner's timer gives up on it) and the mapping to what iOS reads from a power source. No I/O, no waits: the owner sends the
 * bytes and feeds back what arrives. Charging itself stays with the DSP firmware; the one setting written here is the USB input current
 * limit on a computer's port, which the firmware keeps low until the host side asks for more (as a USB device stack does once the
 * port configured it). */
#ifndef SHENG_BATTMGR_H
#define SHENG_BATTMGR_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define SHENG_BATTMGR_OWNER 32778u  /* the battery manager; 32779 is the USB-C port manager on the same channel */
#define SHENG_BATTMGR_TYPE_REQUEST 1u
#define SHENG_BATTMGR_TYPE_NOTIFY 2u
#define SHENG_BATTMGR_OP_REQUEST_NOTIFICATION 0x04u
#define SHENG_BATTMGR_OP_NOTIFICATION 0x07u
#define SHENG_BATTMGR_OP_BATTERY_GET 0x30u
#define SHENG_BATTMGR_OP_USB_GET 0x32u
#define SHENG_BATTMGR_OP_USB_SET 0x33u
#define SHENG_BATTMGR_OP_BOARD_GET 0x50u  /* the board's own properties, same request and answer shape: fuel gauges, charger state */
#define SHENG_BATTMGR_REQUEST_BYTES 24u
#define SHENG_BATTMGR_ENABLE_BYTES 28u

/* The fields one refresh reads, in order (at most 32: valid has a bit per field). */
enum {
    BmStatus, BmPresent, BmCapacity, BmVoltage, BmCurrent, BmTemperature, BmCharge, BmUsbOnline, BmUsbType, BmUsbVoltage,
    BmUsbCurrent, BmTimeToEmpty, BmTimeToFull, BmGauge1Remaining, BmGauge1Full, BmGauge2Remaining, BmGauge2Full, BmFull, BmDesign,
    BmCycles,
    /* the charger's state: port type as charger detection saw it, input current limit and the most the port offers (uA), charge type,
     * thermal level, the charge current the charger runs, the board's view of the adapter (type, watts, PD authenticated), battery
     * authentication of both cells, input suspended */
    BmUsbAdapter, BmUsbLimit, BmUsbMax, BmChargeType, BmThermalLevel, BmChargeCurrent, BmRealType, BmPowerMax, BmPdVerified,
    BmAuthentic, BmSlaveAuthentic, BmInputSuspend, BmFields
};

typedef struct {
    int32_t value[BmFields];
    uint32_t valid;               /* bit per field */
    uint8_t cursor, in_flight, enabled, slow_due;
    uint32_t cycles_done, responses, notifications, errors, timeouts, ignored;
    uint32_t refresh_wanted;      /* a notification asked for a new cycle while one ran */
    int last_timeout;             /* the field the last timeout skipped, -1 none */
    /* the input current limit on a computer's port: what a standard port (SDP) gets, 0 to leave the firmware's; the port type the
     * tries count for; the value due to be sent, the one in flight, tries for this port type, sets the firmware took, the last result
     * (the firmware's, or -60 timed out) */
    uint32_t sdp_limit_ua;
    int32_t limit_port;
    uint32_t limit_due, limit_in_flight, limit_tries, limit_sets;
    int32_t limit_result;
} ShengBattmgr;

/* What a power source shows. Temperatures are hundredths of a degree Celsius, as the firmware reports them. The charge comes from the
 * board's two fuel gauges (remaining over full, both summed) when they answer, else from the battery's capacity property; the board
 * reports the current with discharge positive, the power source with charge positive. */
typedef struct {
    int present, external, charging, fully_charged, critical;
    int percent;                  /* 0..100 */
    int charge_known;             /* percent comes from a reading, not a default */
    int voltage_mv, current_ma;   /* current: positive while the battery takes charge */
    int temperature, temperature_valid;  /* valid: a reading in the range a battery can have */
    int raw_mah, full_mah, design_mah, cycles;
    int time_to_empty_min, time_to_full_min;  /* -1 unknown */
    uint32_t adapter_family;      /* kIOPSFamilyCode value, 0 when no adapter */
    int adapter_mv, adapter_ma, adapter_watts;
} ShengBatterySnapshot;

enum {BmIgnored = 0, BmAnswer = 1, BmCycleDone = 2, BmNotified = 3, BmEnabled = 4};

void sheng_battmgr_init(ShengBattmgr *b);
/* The notification enable request (sent once the channel is up). */
uint32_t sheng_battmgr_enable_request(uint8_t out[SHENG_BATTMGR_ENABLE_BYTES]);
/* Starts a refresh cycle (no effect while one runs: it is remembered and runs after). slow: also the fields that hardly change. */
void sheng_battmgr_refresh(ShengBattmgr *b, int slow);
/* The next request: an input current limit due after the last cycle, else the running cycle's next field; 0 when nothing is to be
 * sent now (one in flight, or nothing due). */
uint32_t sheng_battmgr_next(ShengBattmgr *b, uint8_t out[SHENG_BATTMGR_REQUEST_BYTES]);
/* The owner's request timer ran out: the field is skipped (a limit set counts as tried). */
void sheng_battmgr_timeout(ShengBattmgr *b);
/* A message from the channel; returns one of the Bm* results. */
int sheng_battmgr_receive(ShengBattmgr *b, const uint8_t *data, uint32_t length);
/* The power source values from the fields read so far. */
void sheng_battmgr_snapshot(const ShengBattmgr *b, ShengBatterySnapshot *s);

#ifdef __cplusplus
}
#endif
#endif
