// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../WifiHotspotIe.h"
#include "../WifiRaFilter.h"

static void test_ra_filter(void)
{
    /* Ethernet (14) + IPv6 (40) + Router Advertisement (16) + PIO (32) = 102 bytes */
    uint8_t ula_ra[102];
    memset(ula_ra, 0, sizeof(ula_ra));
    ula_ra[12] = 0x86; ula_ra[13] = 0xdd;
    uint8_t *ip6 = ula_ra + 14;
    ip6[0] = 0x60;
    ip6[4] = 0; ip6[5] = 48; /* payload len = 16 + 32 */
    ip6[6] = 58;             /* ICMPv6 */
    ip6[7] = 255;
    ip6[8] = 0xfe; ip6[9] = 0x80; ip6[23] = 1;
    ip6[24] = 0xff; ip6[25] = 0x02; ip6[39] = 1;
    uint8_t *ra = ip6 + 40;
    ra[0] = 134; ra[1] = 0;  /* Router Advertisement */
    ra[6] = 0x07; ra[7] = 0x08; /* router lifetime 1800s */
    uint8_t *pio = ra + 16;
    pio[0] = 3; pio[1] = 4; pio[2] = 64; pio[3] = 0xc0; /* L|A */
    pio[4] = 0; pio[5] = 0; pio[6] = 0x0e; pio[7] = 0x10; /* valid 3600s */
    pio[8] = 0; pio[9] = 0; pio[10] = 0x07; pio[11] = 0x08; /* preferred 1800s */
    pio[16] = 0xfd; pio[17] = 0x12; /* ULA fd12::/64 */

    uint8_t dual_ra[134];
    memset(dual_ra, 0, sizeof(dual_ra));
    memcpy(dual_ra, ula_ra, sizeof(ula_ra));
    dual_ra[14 + 5] = 80; /* 16 + 32 + 32 */
    uint8_t *gua = dual_ra + 102;
    gua[0] = 3; gua[1] = 4; gua[2] = 64; gua[3] = 0xc0;
    gua[7] = 0x10; gua[11] = 0x08;
    gua[16] = 0x24; gua[17] = 0x0e; /* Global 240e::/64 */

    /* ULA-only RA is neutralized (router lifetime=0, A=0, lifetimes=0, valid checksum) */
    assert(sheng_wifi_scrub_ula_only_ra(ula_ra, sizeof(ula_ra)));
    assert(ra[6] == 0 && ra[7] == 0);
    assert((pio[3] & 0x40u) == 0);
    assert(memcmp(pio + 4, "\0\0\0\0\0\0\0\0", 8) == 0);
    uint16_t expect_cksum = sheng_icmp6_checksum(ip6, ra, 48);
    assert((((uint16_t)ra[2] << 8) | ra[3]) == expect_cksum);

    /* Dual GUA + ULA RA is left untouched */
    assert(!sheng_wifi_scrub_ula_only_ra(dual_ra, sizeof(dual_ra)));
    assert(dual_ra[14 + 40 + 6] == 0x07 && dual_ra[14 + 40 + 16 + 3] == 0xc0);
}

int main(void)
{
    uint32_t ch = 0, fl = 0;
    wifi_freq_to_channel(2472, &ch, &fl);
    assert(ch == 13 && fl == 0xau);
    wifi_freq_to_channel(5745, &ch, &fl);
    assert(ch == 149 && fl == 0x12u);
    wifi_freq_to_channel(5975, &ch, &fl);
    assert(ch == 5 && fl == 0x2002u);

    /* Transitional WPA2-PSK (00-0f-ac:2) + WPA3-SAE (00-0f-ac:8) with MFPC|MFPR (0xc0) + RSNXE (tag 244) */
    const uint8_t trans_ie[] = {
        0, 5, 'P', 'h', 'o', 'n', 'e',
        48, 24,
            1, 0,
            0x00, 0x0f, 0xac, 4,
            1, 0, 0x00, 0x0f, 0xac, 4,
            2, 0, 0x00, 0x0f, 0xac, 2, 0x00, 0x0f, 0xac, 8,
            0xc0, 0x00,
        244, 1, 0x20
    };
    uint8_t out[128] = {0};
    uint16_t out_len = wifi_sanitize_hotspot_ies(trans_ie, sizeof(trans_ie), out, sizeof(out));
    assert(out_len == 29);
    assert(out[7] == 48 && out[8] == 20);
    assert(out[21] == 1 && out[22] == 0);
    assert(out[26] == 2);
    assert((out[27] & 0xc0) == 0x80);

    /* ANDROID_METERED / force_hotspot_badge appends 10-byte 00:17:f2:06 Apple Personal Hotspot IE */
    uint16_t hot_len = wifi_sanitize_hotspot_ies_ex(trans_ie, sizeof(trans_ie), out, sizeof(out), true);
    assert(hot_len == 39);
    assert(out[29] == 0xdd && out[30] == 0x08 && out[31] == 0x00 && out[32] == 0x17 && out[33] == 0xf2 && out[34] == 0x06);

    /* Android OUI 00:1a:11 also auto-synthesizes 00:17:f2:06 without duplicating if already present */
    const uint8_t android_ie[] = { 0, 3, 'M', 'i', '1', 0xdd, 0x04, 0x00, 0x1a, 0x11, 0x01 };
    uint16_t and_len = wifi_sanitize_hotspot_ies_ex(android_ie, sizeof(android_ie), out, sizeof(out), false);
    assert(and_len == sizeof(android_ie) + 10);
    assert(out[11] == 0xdd && out[13] == 0x00 && out[14] == 0x17 && out[15] == 0xf2 && out[16] == 0x06);
    uint8_t out2[128] = {0};
    uint16_t idemp_len = wifi_sanitize_hotspot_ies_ex(out, and_len, out2, sizeof(out2), true);
    assert(idemp_len == and_len);

    test_ra_filter();
    printf("wifi_hotspot_ie_host: ok\n");
    return 0;
}
