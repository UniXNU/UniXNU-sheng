/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SHENG_CHARGE_AUTH_H
#define SHENG_CHARGE_AUTH_H
#include <stdint.h>

/* Xiaomi's PMIC GLINK authentication protocol, as used by sm8550-mainline
 * 87ac0f2347eb and xiaomi-mipps-auth 9176efbdf276. The firmware owns PD/PPS
 * contracts, charge pumps, current limits and thermal regulation. */
#define SHENG_CHARGE_REQUEST_BYTES 52u
enum {
    ChargeIdle, ChargeGaugeCheck, ChargeGaugeChallenge, ChargeGaugeDigest, ChargeGaugeMark,
    ChargeAdapterCheck, ChargeHostRole, ChargeVdmWrite, ChargeVdmState, ChargeVdmDigest,
    ChargeAdapterId, ChargeVerified, ChargeReverse, ChargeCompensation, ChargePdMark,
    ChargeDiagnostics, ChargeDone, ChargeFailed
};
enum {
    ChargeAdapterSvid, ChargePdo2, ChargeAdapterIdValue, ChargeApdoMax, ChargePowerMax,
    ChargeFastMode, ChargeQuickType, ChargePpsPtf, ChargePump1Ok, ChargePump2Ok,
    ChargePump1Current, ChargePump2Current, ChargePumpVoltage, ChargePumpBatteryVoltage,
    ChargeGaugeFast, ChargeVdmVersion, ChargeVdmVoltage, ChargeVdmTemperature, ChargeDiagnosticCount
};
typedef struct {
    void (*random)(void *, uint32_t);
    void (*hmac)(const uint8_t *, uint32_t, const uint8_t *, uint32_t, uint8_t[32]);
} ShengChargeCrypto;
typedef struct {
    ShengChargeCrypto crypto;
    uint32_t state, pending, opcode, property, pending_bytes;
    uint64_t due_ms, deadline_ms;
    uint32_t generation, reads, writes, errors, timeouts, ignored, retries;
    uint32_t gauge_verified, adapter_verified, diagnostic_valid;
    int32_t diagnostic[ChargeDiagnosticCount];
    int32_t result, last_result, host_ready;
    uint8_t online, enabled, slave, tries, seed_index, vdm_command, diagnostic_cursor, diagnostic_due, cancelled;
    uint8_t challenge[32], expected[32];
} ShengChargeAuth;

void sheng_charge_auth_init(ShengChargeAuth *, const ShengChargeCrypto *);
/* Retire an attachment. An outstanding exchange keeps its queue slot until its
 * reply or timeout, so the reply cannot advance a replacement attachment. */
void sheng_charge_auth_reconnect(ShengChargeAuth *, uint64_t now_ms);
/* Called after a battery refresh. Authentication runs once per PD attachment. */
void sheng_charge_auth_observe(ShengChargeAuth *, int online, int usb_type, uint64_t now_ms);
/* All requests share the battery manager's serialized GLINK queue. */
uint32_t sheng_charge_auth_next(ShengChargeAuth *, uint64_t now_ms, uint8_t out[SHENG_CHARGE_REQUEST_BYTES]);
int sheng_charge_auth_receive(ShengChargeAuth *, const uint8_t *, uint32_t, uint64_t now_ms);
void sheng_charge_auth_timeout(ShengChargeAuth *, uint64_t now_ms);
uint32_t sheng_charge_auth_wait_ms(const ShengChargeAuth *, uint64_t now_ms);

#endif
