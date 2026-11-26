// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SHENG_WIFI_RA_FILTER_H
#define SHENG_WIFI_RA_FILTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Compute RFC 4443 16-bit 1's-complement ICMPv6 checksum over IPv6 pseudo-header + ICMPv6 payload. */
static inline uint16_t sheng_icmp6_checksum(const uint8_t ip6[40], const uint8_t *icmp, size_t plen)
{
    uint32_t sum = 0;
    for (size_t i = 8; i < 40; i += 2) {
        sum += ((uint32_t)ip6[i] << 8) | ip6[i + 1];
    }
    sum += (uint32_t)((plen >> 16) & 0xffffu);
    sum += (uint32_t)(plen & 0xffffu);
    sum += 58u; /* Next Header: ICMPv6 */
    for (size_t i = 0; i + 1 < plen; i += 2) {
        if (i == 2) continue; /* skip checksum field itself */
        sum += ((uint32_t)icmp[i] << 8) | icmp[i + 1];
    }
    if (plen & 1u) {
        sum += (uint32_t)icmp[plen - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    uint16_t res = (uint16_t)(~sum & 0xffffu);
    return res == 0 ? 0xffffu : res;
}

/* Neutralize unroutable ULA-only (fc00::/7) SLAAC Prefix Information Options (type 3) and
 * Route Information Options (type 24) in inbound Ethernet+IPv6 ICMPv6 Router Advertisements
 * (type 134, code 0) when NO global unicast (2000::/3) prefix is advertised in the RA.
 * Prevents home routers advertising local-only fd00::/8 prefixes without IPv6 WAN from
 * assigning broken ULA source addresses that stall WebKit/Safari Happy Eyeballs. */
static inline bool sheng_wifi_scrub_ula_only_ra(uint8_t *frame, size_t held)
{
    if (!frame || held < 70) return false;
    if (frame[12] != 0x86 || frame[13] != 0xdd) return false;
    uint8_t *ip6 = frame + 14;
    if ((ip6[0] >> 4) != 6 || ip6[6] != 58) return false;
    const size_t plen = ((size_t)ip6[4] << 8) | ip6[5];
    if (plen < 16 || 14 + 40 + plen > held) return false;
    uint8_t *ra = ip6 + 40;
    if (ra[0] != 134 || ra[1] != 0) return false;

    bool has_global = false;
    bool has_ula = false;
    size_t off = 16;
    while (off + 2 <= plen) {
        const uint8_t opt_type = ra[off];
        const uint8_t opt_units = ra[off + 1];
        if (opt_units == 0) return false; /* malformed NDP option */
        const size_t opt_len = (size_t)opt_units * 8u;
        if (off + opt_len > plen) return false;
        if (opt_type == 3 && opt_units == 4) {
            const uint8_t p0 = ra[off + 16];
            if ((p0 & 0xe0u) == 0x20u) has_global = true;
            else if ((p0 & 0xfeu) == 0xfcu) has_ula = true;
        } else if (opt_type == 24 && opt_units >= 2) {
            const uint8_t pfxlen = ra[off + 2];
            if (pfxlen > 0 && opt_len >= 16) {
                const uint8_t p0 = ra[off + 8];
                if ((p0 & 0xe0u) == 0x20u) has_global = true;
                else if ((p0 & 0xfeu) == 0xfcu) has_ula = true;
            }
        }
        off += opt_len;
    }

    if (!has_ula || has_global) return false;

    /* Zero Router Lifetime so en0 does not install a dead default IPv6 route via fe80:: router. */
    ra[6] = 0;
    ra[7] = 0;

    off = 16;
    while (off + 2 <= plen) {
        const uint8_t opt_type = ra[off];
        const uint8_t opt_units = ra[off + 1];
        const size_t opt_len = (size_t)opt_units * 8u;
        if (opt_type == 3 && opt_units == 4 && (ra[off + 16] & 0xfeu) == 0xfcu) {
            ra[off + 3] &= (uint8_t)~0xc0u; /* clear L (0x80) and A (0x40) SLAAC flags */
            memset(ra + off + 4, 0, 8);     /* zero valid_lifetime and preferred_lifetime */
        } else if (opt_type == 24 && opt_units >= 2) {
            const uint8_t pfxlen = ra[off + 2];
            if (pfxlen == 0 || (opt_len >= 16 && (ra[off + 8] & 0xfeu) == 0xfcu)) {
                memset(ra + off + 4, 0, 4); /* zero route_lifetime */
            }
        }
        off += opt_len;
    }

    const uint16_t csum = sheng_icmp6_checksum(ip6, ra, plen);
    ra[2] = (uint8_t)(csum >> 8);
    ra[3] = (uint8_t)(csum & 0xffu);
    return true;
}

#endif // SHENG_WIFI_RA_FILTER_H
