// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SHENG_WIFI_HOTSPOT_IE_H
#define SHENG_WIFI_HOTSPOT_IE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline void wifi_freq_to_channel(uint32_t mhz, uint32_t *ch_out, uint32_t *flags_out)
{
    if (mhz >= 5925) {
        *ch_out = mhz == 5935 ? 2 : (mhz - 5950) / 5;
        *flags_out = 0x2u | 0x2000u;
    } else if (mhz >= 4900) {
        *ch_out = (mhz - 5000) / 5;
        *flags_out = 0x2u | 0x10u;
    } else {
        *ch_out = mhz == 2484 ? 14 : (mhz >= 2412 ? (mhz - 2407) / 5 : 0);
        *flags_out = 0x2u | 0x8u;
    }
}

/* Rewrite WPA2/WPA3-Transitional RSN IEs (which carry both WPA2-PSK 00-0f-ac:2/:6 and WPA3-SAE 00-0f-ac:8)
 * to advertise only PSK/PSK-SHA256 AKMs, clear MFPR when MFPC is set, drop RSNXE (tag 244) whenever a
 * transitional RSN IE was rewritten, and synthesize Apple's Personal Hotspot IE (00:17:f2:06) when an
 * Android/Xiaomi hotspot OUI (00:1a:11) or DHCP ANDROID_METERED flag (`force_hotspot_badge`) is detected
 * and Apple's 00:17:f2:06 IE is not already present. */
static inline uint16_t wifi_sanitize_hotspot_ies_ex(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, bool force_hotspot_badge)
{
    bool has_transitional_psk = false;
    bool has_apple_ios_ie = false;
    bool has_android_hotspot_oui = false;
    size_t scan_pos = 0;
    while (scan_pos + 2 <= in_len) {
        uint8_t tag = in[scan_pos];
        uint8_t len = in[scan_pos + 1];
        if (scan_pos + 2 + len > in_len) break;
        const uint8_t *body = in + scan_pos + 2;
        if (tag == 0xdd && len >= 4) {
            if (body[0] == 0x00 && body[1] == 0x17 && body[2] == 0xf2 && body[3] == 0x06) {
                has_apple_ios_ie = true;
            } else if (body[0] == 0x00 && body[1] == 0x1a && body[2] == 0x11) {
                has_android_hotspot_oui = true;
            } else if (body[0] == 0x00 && body[1] == 0x50 && body[2] == 0xf2 && body[3] == 0x04) {
                /* WPS IE: match Primary Device Type (0x1054) category 0x000a (Telephony / Smartphone). */
                size_t wps = 4;
                while (wps + 4 <= len) {
                    uint16_t wt = ((uint16_t)body[wps] << 8) | body[wps + 1];
                    uint16_t wl = ((uint16_t)body[wps + 2] << 8) | body[wps + 3];
                    if (wps + 4 + wl > len) break;
                    if (wt == 0x1054 && wl == 8 && body[wps + 4] == 0x00 && body[wps + 5] == 0x0a) {
                        has_android_hotspot_oui = true;
                        break;
                    }
                    wps += 4 + wl;
                }
            } else if (body[0] == 0x50 && body[1] == 0x6f && body[2] == 0x9a && body[3] == 0x16) {
                /* WFA MBO IE: match Cellular Data Capabilities (attr 0x02) == 1 or 2 (exclude 0/3 home routers). */
                size_t mbo = 4;
                while (mbo + 2 <= len) {
                    uint8_t mid = body[mbo];
                    uint8_t ml = body[mbo + 1];
                    if (mbo + 2 + ml > len) break;
                    if (mid == 0x02 && ml >= 1 && (body[mbo + 2] == 0x01 || body[mbo + 2] == 0x02)) {
                        has_android_hotspot_oui = true;
                        break;
                    }
                    mbo += 2 + ml;
                }
            }
        }
        if (tag == 48 && len >= 10) {
            uint16_t ver = (uint16_t)body[0] | ((uint16_t)body[1] << 8);
            if (ver == 1 && 8 <= len) {
                uint16_t pw_cnt = (uint16_t)body[6] | ((uint16_t)body[7] << 8);
                size_t akm_off = 8 + (size_t)pw_cnt * 4;
                if (akm_off + 2 <= len) {
                    uint16_t akm_cnt = (uint16_t)body[akm_off] | ((uint16_t)body[akm_off + 1] << 8);
                    if (akm_off + 2 + (size_t)akm_cnt * 4 <= len) {
                        bool any_psk = false, any_other = false;
                        for (uint16_t k = 0; k < akm_cnt; ++k) {
                            const uint8_t *suite = body + akm_off + 2 + (size_t)k * 4;
                            if (suite[0] == 0x00 && suite[1] == 0x0f && suite[2] == 0xac &&
                                (suite[3] == 2 || suite[3] == 6)) {
                                any_psk = true;
                            } else {
                                any_other = true;
                            }
                        }
                        if (any_psk && any_other) has_transitional_psk = true;
                    }
                }
            }
        }
        scan_pos += 2 + len;
    }

    size_t pos = 0, w = 0;
    while (pos + 2 <= in_len) {
        uint8_t tag = in[pos];
        uint8_t len = in[pos + 1];
        if (pos + 2 + len > in_len) break;
        const uint8_t *body = in + pos + 2;
        pos += 2 + len;

        if (tag == 244 && has_transitional_psk) {
            continue;
        }
        if (tag == 48 && has_transitional_psk && len >= 10) {
            uint16_t ver = (uint16_t)body[0] | ((uint16_t)body[1] << 8);
            uint16_t pw_cnt = (uint16_t)body[6] | ((uint16_t)body[7] << 8);
            size_t akm_off = 8 + (size_t)pw_cnt * 4;
            if (ver == 1 && akm_off + 2 <= len) {
                uint16_t akm_cnt = (uint16_t)body[akm_off] | ((uint16_t)body[akm_off + 1] << 8);
                size_t tail_off = akm_off + 2 + (size_t)akm_cnt * 4;
                if (tail_off <= len) {
                    uint8_t psk_suites[16];
                    uint16_t kept = 0;
                    for (uint16_t k = 0; k < akm_cnt && kept < 4; ++k) {
                        const uint8_t *suite = body + akm_off + 2 + (size_t)k * 4;
                        if (suite[0] == 0x00 && suite[1] == 0x0f && suite[2] == 0xac &&
                            (suite[3] == 2 || suite[3] == 6)) {
                            memcpy(psk_suites + kept * 4, suite, 4);
                            ++kept;
                        }
                    }
                    size_t tail_len = len - tail_off;
                    size_t new_len = akm_off + 2 + (size_t)kept * 4 + tail_len;
                    if (kept > 0 && new_len <= 255 && w + 2 + new_len <= out_cap) {
                        out[w++] = 48;
                        out[w++] = (uint8_t)new_len;
                        memcpy(out + w, body, akm_off);
                        w += akm_off;
                        out[w++] = (uint8_t)(kept & 0xff);
                        out[w++] = (uint8_t)(kept >> 8);
                        memcpy(out + w, psk_suites, (size_t)kept * 4);
                        w += (size_t)kept * 4;
                        if (tail_len > 0) {
                            memcpy(out + w, body + tail_off, tail_len);
                            if (tail_len >= 2 && (out[w] & 0x80u)) {
                                out[w] &= (uint8_t)~0x40u; /* clear MFPR bit6 when MFPC bit7 is set */
                            }
                            w += tail_len;
                        }
                        continue;
                    }
                }
            }
        }
        if (w + 2 + len > out_cap) break;
        out[w++] = tag;
        out[w++] = len;
        if (len) memcpy(out + w, body, len);
        w += len;
    }

    if ((force_hotspot_badge || has_android_hotspot_oui) && !has_apple_ios_ie && w + 10 <= out_cap) {
        static const uint8_t kAppleHotspotIe[10] = {
            0xdd, 0x08, 0x00, 0x17, 0xf2, 0x06, 0x01, 0x01, 0x01, 0x01
        };
        memcpy(out + w, kAppleHotspotIe, sizeof(kAppleHotspotIe));
        w += sizeof(kAppleHotspotIe);
    }
    return (uint16_t)w;
}

static inline uint16_t wifi_sanitize_hotspot_ies(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap)
{
    return wifi_sanitize_hotspot_ies_ex(in, in_len, out, out_cap, false);
}

#endif // SHENG_WIFI_HOTSPOT_IE_H
