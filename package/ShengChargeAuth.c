/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "ShengChargeAuth.h"
#include <string.h>

enum {Owner = 32778, Get = 0x50, Set = 0x51, VerifyDigest = 1, Authentic = 3, SlaveAuthentic = 0xa5,
      PdVerified = 0x18, VdmState = 0x1a};
static const uint8_t diagnostic_properties[ChargeDiagnosticCount] = {
    0x17, 0x19, 0x16, 0x32, 0x33, 0x2b, 0x31, 0xe7, 0x1b, 0x1c, 0x1d, 0x1e, 0x20, 0x23,
    0x8f, 0x0d, 0x0e, 0x0f
};
/* Public protocol constants from xiaomi-mipps-auth. */
static const char *const gauge_keys[] = {
    "45f0f5a023b09a7437d9b85a6ea2033539321d5437b9543f3f2292e0c966d675",
    "02b3a85ea4c1a6db6fbd1fd5767c1a42314f37ba7c9460c9ed79a12bb2b5d99c"
};
static const char *const session_seeds[] = {
    "d6e9770506c7f5f4df7a40008c7467b8", "09604eb9ad37e17bd6b2a4f2a3515984",
    "f6295ce3bde8678c9ffeab616098aba0", "ae0c8ee9b05ba233799badcb9fa22b41",
    "56b7470c9d6f22ca003e40326fa77eb5", "6019b457cd77fd5fedffdc3d0ed06106",
    "09e47c32760fb2facfc5843df0716465", "a601934c1403bfd955ebd77f509ba17f",
    "3e02b44e19dc24f438e62f026ade4aa7", "343d6b41b900beb4c8bf0e8fa7df8c13"
};
static const char *const adapter_keys[] = {
    "5c4d4179da1501ed11747492e26083d96b8fbc73c82e7bdf5d3255c2a036a1dc",
    "4554ea907fc3250d2078bd6277d6cf0cb823e62258d087378af29c466b101026",
    "83d30625cb0f7ce6d49649ff4cfcf9cebe62469b57e31ba62d01de2810ff621a",
    "a1447305166b4a786df5ddd707a8360a7328ced7038487607fa21128a15d1864",
    "e4fb00afd15186af1a44502005d3d1b0b1e9d1a636a51c26349422e7dbadc273",
    "46b8cae9a46a3254ee551282cf07b7bb6b2ef12bf615463c239bb63b984e42a9",
    "eaf45b6ff3ccce769e82bd0b007b69c406a66b98f427a2bbe455d1ffcb1ecdcc",
    "e787db568b7d704d29cc50a2bb1e971f6f1fd7fe9604deb4d4a1a7a72c63ff42",
    "b5f704a75d0db54a0b5e731779949b321105adc9d5c0858c50efc9bf92892cf4",
    "df47cc3249833bdefeb4a04a257489fc7500d827b2b66b2689bded7ffe49465e"
};
static uint32_t get32(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static void put32(uint8_t *p, uint32_t v) {p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);}
static unsigned nibble(char c) {return c <= '9' ? (unsigned)(c - '0') : (unsigned)(c - 'a' + 10);}
static void unhex(uint8_t *out, const char *hex, unsigned count) {
    for (unsigned i = 0; i < count; ++i) out[i] = (uint8_t)(nibble(hex[2*i]) << 4 | nibble(hex[2*i+1]));
}
static int equal(const uint8_t *a, const uint8_t *b, unsigned count) {
    uint8_t difference = 0;
    for (unsigned i = 0; i < count; ++i) difference |= a[i] ^ b[i];
    return difference == 0;
}
static uint32_t request(ShengChargeAuth *a, uint8_t *out, uint32_t opcode, uint32_t property, unsigned bytes) {
    memset(out, 0, SHENG_CHARGE_REQUEST_BYTES);
    put32(out, Owner); put32(out + 4, 1); put32(out + 8, opcode);
    put32(out + (bytes == 24 ? 16 : 12), property);
    a->pending = 1; a->opcode = opcode; a->property = property; a->pending_bytes = bytes;
    if (opcode == Get) a->reads++; else a->writes++;
    return bytes;
}
static uint32_t scalar(ShengChargeAuth *a, uint8_t *out, uint32_t opcode, uint32_t property, uint32_t value) {
    request(a, out, opcode, property, 24); put32(out + 20, value); return 24;
}
/* VDM words are little endian integers whose hex spelling is the wire octets. */
static void vdm_words(uint8_t *out, const uint8_t *bytes) {
    for (unsigned i = 0; i < 16; i += 4)
        for (unsigned j = 0; j < 4; ++j) out[i+j] = bytes[i+3-j];
}
static void diagnostics(ShengChargeAuth *a, int result) {
    a->state = ChargeDiagnostics; a->result = result; a->diagnostic_cursor = 0; a->diagnostic_due = 0;
    memset(a->challenge, 0, sizeof a->challenge);
    memset(a->expected, 0, sizeof a->expected);
}
static void failure(ShengChargeAuth *a, int result, uint64_t now) {
    a->errors++; a->last_result = result;
    if (a->state >= ChargeGaugeCheck && a->state <= ChargeGaugeMark && a->tries < 10) {
        a->retries++; a->state = ChargeGaugeChallenge; a->due_ms = now + 200; return;
    }
    if (a->state == ChargeReverse || (a->state == ChargeVdmState && a->vdm_command == 8)) {
        a->state = ChargeCompensation; a->due_ms = now; return;
    }
    diagnostics(a, result);
}
static void gauge_done(ShengChargeAuth *a, uint64_t now) {
    a->gauge_verified |= 1u << a->slave;
    a->tries = 0;
    a->state = a->slave ? ChargeAdapterCheck : ChargeGaugeCheck;
    a->slave = 1;
    a->due_ms = now + 500;
}
static void vdm_next(ShengChargeAuth *a, uint64_t now) {
    if (a->vdm_command == 8) {a->state = ChargeCompensation; a->due_ms = now;}
    else if (a->vdm_command < 5) {a->vdm_command++; a->state = ChargeVdmWrite; a->due_ms = now;}
    else {a->state = ChargeVdmDigest; a->due_ms = now;}
}
void sheng_charge_auth_init(ShengChargeAuth *a, const ShengChargeCrypto *crypto) {
    memset(a, 0, sizeof *a);
    if (crypto) {a->crypto = *crypto; a->enabled = crypto->random && crypto->hmac;}
}
void sheng_charge_auth_observe(ShengChargeAuth *a, int online, int usb_type, uint64_t now) {
    const int pd = usb_type == 6 || usb_type == 7 || usb_type == 8;
    if (!online || (!pd && a->online)) {
        const ShengChargeCrypto crypto = a->crypto;
        const uint32_t generation = a->generation;
        sheng_charge_auth_init(a, &crypto); a->generation = generation;
        return;
    }
    if (!a->enabled || !pd) return;
    if (!a->online) {
        a->online = 1; a->generation++; a->state = ChargeGaugeCheck; a->due_ms = now;
    } else if (a->state == ChargeDone || a->state == ChargeFailed) {
        a->diagnostic_due = 1;
    }
}
uint32_t sheng_charge_auth_next(ShengChargeAuth *a, uint64_t now, uint8_t out[SHENG_CHARGE_REQUEST_BYTES]) {
    if (!a->online || !a->enabled || a->pending || now < a->due_ms) return 0;
    if ((a->state == ChargeDone || a->state == ChargeFailed) && a->diagnostic_due) diagnostics(a, a->result);
    switch (a->state) {
    case ChargeGaugeCheck:
        return scalar(a, out, Get, a->slave ? SlaveAuthentic : Authentic, 0);
    case ChargeGaugeChallenge:
        a->crypto.random(a->challenge, 32); a->tries++;
        request(a, out, Set, VerifyDigest, 52); memcpy(out + 16, a->challenge, 32); out[48] = a->slave; return 52;
    case ChargeGaugeDigest:
        request(a, out, Get, VerifyDigest, 52); out[48] = a->slave; return 52;
    case ChargeGaugeMark:
        return scalar(a, out, Set, a->slave ? SlaveAuthentic : Authentic, 1);
    case ChargeAdapterCheck:
        return scalar(a, out, Get, a->diagnostic_valid & (1u << ChargeAdapterSvid) ? 0x19 : 0x17, 0);
    case ChargeHostRole:
        if (a->host_ready > 0) {a->vdm_command = 1; a->state = ChargeVdmWrite; return sheng_charge_auth_next(a, now, out);}
        if (a->host_ready < 0 || now >= a->deadline_ms) {failure(a, -95, now); return sheng_charge_auth_next(a, now, out);}
        a->due_ms = now + 100; return 0;
    case ChargeVdmWrite:
        if (a->vdm_command <= 3) return scalar(a, out, Set, 0x0c + a->vdm_command, 0);
        request(a, out, Set, 0x0c + a->vdm_command, 32);
        if (a->vdm_command == 4) {
            uint8_t seed[16]; unhex(seed, session_seeds[a->seed_index], 16); vdm_words(out + 16, seed);
        } else {
            a->crypto.random(a->challenge, 16); vdm_words(out + 16, a->challenge);
        }
        return 32;
    case ChargeVdmState:
        return scalar(a, out, Get, VdmState, 0);
    case ChargeVdmDigest:
        return request(a, out, Get, 0x11, 32);
    case ChargeAdapterId:
        return scalar(a, out, Get, 0x16, 0);
    case ChargeVerified:
        return scalar(a, out, Set, 0x12, a->adapter_verified);
    case ChargeReverse:
        request(a, out, Set, 0x14, 32); vdm_words(out + 16, a->expected + 16); return 32;
    case ChargeCompensation:
        return scalar(a, out, Set, 0x13, a->adapter_verified);
    case ChargePdMark:
        return scalar(a, out, Set, PdVerified, a->adapter_verified);
    case ChargeDiagnostics:
        if (a->diagnostic_cursor < ChargeDiagnosticCount)
            return scalar(a, out, Get, diagnostic_properties[a->diagnostic_cursor], 0);
        a->state = a->result ? ChargeFailed : ChargeDone;
        return 0;
    default:
        return 0;
    }
}
int sheng_charge_auth_receive(ShengChargeAuth *a, const uint8_t *data, uint32_t bytes, uint64_t now) {
    if (!a->pending || bytes < 16 || get32(data) != Owner || get32(data+4) != 1 ||
        get32(data+8) != a->opcode || get32(data+12) != a->property) return 0;
    const int scalar_reply = bytes == 24;
    const int32_t result = scalar_reply ? (int32_t)get32(data+20) : 0;
    if ((!scalar_reply && bytes < 16 + (a->pending_bytes == 52 ? 32u : 16u)) ||
        (a->opcode == Get && a->pending_bytes > 24 && scalar_reply && !result)) {
        a->pending = 0; failure(a, -61, now); return 1;
    }
    a->pending = 0; a->last_result = result; a->due_ms = now;
    if (result) {
        if (a->state == ChargeDiagnostics) {
            a->diagnostic_valid &= ~(1u << a->diagnostic_cursor); a->diagnostic_cursor++; a->errors++;
        } else failure(a, result, now);
        return 1;
    }
    const uint32_t value = get32(data + 16);
    switch (a->state) {
    case ChargeGaugeCheck:
        if (value == 1) gauge_done(a, now); else a->state = ChargeGaugeChallenge;
        break;
    case ChargeGaugeChallenge:
        a->state = ChargeGaugeDigest; a->due_ms = now + 1400;
        break;
    case ChargeGaugeDigest: {
        uint8_t key[32], digest[32]; int matched = 0;
        for (unsigned i = 0; i < sizeof gauge_keys / sizeof *gauge_keys; ++i) {
            unhex(key, gauge_keys[i], 32); a->crypto.hmac(key, 32, a->challenge, 32, digest);
            matched |= equal(digest, data + 16, 32);
        }
        memset(key, 0, sizeof key); memset(digest, 0, sizeof digest);
        if (matched) a->state = ChargeGaugeMark; else failure(a, -74, now);
        break;
    }
    case ChargeGaugeMark:
        gauge_done(a, now);
        break;
    case ChargeAdapterCheck:
        if (a->property == 0x17) {
            a->diagnostic[ChargeAdapterSvid] = (int32_t)value; a->diagnostic_valid |= 1u << ChargeAdapterSvid;
            if (value != 0x2717) diagnostics(a, 0);
        } else {
            a->diagnostic[ChargePdo2] = (int32_t)value; a->diagnostic_valid |= 1u << ChargePdo2;
            if (!value) diagnostics(a, 0);
            else {
                uint8_t seed; a->crypto.random(&seed, 1); a->seed_index = seed % 10;
                a->state = ChargeHostRole; a->host_ready = 0; a->deadline_ms = now + 5000;
            }
        }
        break;
    case ChargeVdmWrite:
        a->state = ChargeVdmState; a->deadline_ms = now + 2000;
        a->due_ms = now + (a->vdm_command >= 4 ? 500 : 5);
        break;
    case ChargeVdmState:
        if (value == a->vdm_command) vdm_next(a, now);
        else if (now >= a->deadline_ms) failure(a, -60, now);
        else a->due_ms = now + 20;
        break;
    case ChargeVdmDigest:
        vdm_words(a->expected, data + 16); a->state = ChargeAdapterId;
        break;
    case ChargeAdapterId: {
        if (!value) {failure(a, -61, now); break;}
        uint8_t key[32], message[20], digest[32];
        a->diagnostic[ChargeAdapterIdValue] = (int32_t)value; a->diagnostic_valid |= 1u << ChargeAdapterIdValue;
        memcpy(message, a->challenge, 16);
        for (unsigned i = 0; i < 4; ++i) message[16+i] = (uint8_t)(value >> (24-8*i));
        unhex(key, adapter_keys[a->seed_index], 32); a->crypto.hmac(key, 32, message, 20, digest);
        a->adapter_verified = (uint32_t)equal(a->expected, digest, 16);
        memcpy(a->expected, digest, 32);
        memset(key, 0, sizeof key); memset(message, 0, sizeof message); memset(digest, 0, sizeof digest);
        if (!a->adapter_verified) {a->result = -74; a->errors++;}
        a->state = ChargeVerified;
        break;
    }
    case ChargeVerified:
        a->state = a->adapter_verified ? ChargeReverse : ChargeCompensation; a->due_ms = now + 100;
        break;
    case ChargeReverse:
        a->vdm_command = 8; a->state = ChargeVdmState; a->deadline_ms = now + 2000; a->due_ms = now + 20;
        break;
    case ChargeCompensation:
        a->state = ChargePdMark; a->due_ms = now + 100;
        break;
    case ChargePdMark:
        diagnostics(a, a->result);
        break;
    case ChargeDiagnostics:
        a->diagnostic[a->diagnostic_cursor] = (int32_t)value;
        a->diagnostic_valid |= 1u << a->diagnostic_cursor; a->diagnostic_cursor++;
        break;
    default:
        break;
    }
    return 1;
}
void sheng_charge_auth_timeout(ShengChargeAuth *a, uint64_t now) {
    if (!a->pending) return;
    a->pending = 0; a->timeouts++;
    if (a->state == ChargeDiagnostics) {a->diagnostic_valid &= ~(1u << a->diagnostic_cursor); a->diagnostic_cursor++; return;}
    failure(a, -60, now);
}
uint32_t sheng_charge_auth_wait_ms(const ShengChargeAuth *a, uint64_t now) {
    if (!a->enabled || !a->online || a->pending || a->state == ChargeIdle || a->state == ChargeDone || a->state == ChargeFailed) return 0;
    return a->due_ms > now ? (uint32_t)(a->due_ms - now) : 1;
}
