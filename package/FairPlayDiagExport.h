// SPDX-License-Identifier: BSD-2-Clause-Patent
// kern.unixnu_fairplay_diag (BootDiagExport.c): read-only summary of SoftwareAES registration,
// selector usage counters, the last unsupported hardware key selector, and active vmm-hide boot-args.
// Host-tested by scripts/check-fairplay-diag-export-host.py.
#ifndef SHENG_FAIRPLAY_DIAG_EXPORT_H
#define SHENG_FAIRPLAY_DIAG_EXPORT_H
#include <stddef.h>
#include <stdint.h>

enum { SHENG_FAIRPLAY_DIAG_TEXT = 512 };

static inline void sheng_fairplay_diag_puts(char *out, size_t limit, size_t *at, const char *s)
{
    while (*s && *at < limit) {
        out[(*at)++] = *s++;
    }
}

static inline void sheng_fairplay_diag_putu(char *out, size_t limit, size_t *at, uint32_t value)
{
    char digits[16];
    unsigned count = 0;
    do {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u);
    while (count > 0 && *at < limit) {
        out[(*at)++] = digits[--count];
    }
}

static inline void sheng_fairplay_diag_putx(char *out, size_t limit, size_t *at, uint32_t value)
{
    static const char hex[] = "0123456789abcdef";
    char digits[16];
    unsigned count = 0;
    sheng_fairplay_diag_puts(out, limit, at, "0x");
    do {
        digits[count++] = hex[value & 0xfu];
        value >>= 4;
    } while (value != 0u);
    while (count > 0 && *at < limit) {
        out[(*at)++] = digits[--count];
    }
}

static inline size_t sheng_fairplay_diag_text(
    uint32_t aes_registered,
    uint32_t vmm_hide_hits,
    uint32_t fairplay_uses,
    uint32_t nonce_uses,
    uint32_t unsupported_uses,
    uint32_t last_unsupported_selector,
    const char *vmm_hide,
    char *out,
    size_t capacity)
{
    if (!out || capacity == 0) {
        return 0;
    }
    if (capacity == 1) {
        out[0] = '\0';
        return 0;
    }
    const size_t limit = capacity - 2;
    size_t at = 0;
    sheng_fairplay_diag_puts(out, limit, &at, "aes_registered=");
    sheng_fairplay_diag_putu(out, limit, &at, aes_registered);
    sheng_fairplay_diag_puts(out, limit, &at, " vmm_hide_hits=");
    sheng_fairplay_diag_putu(out, limit, &at, vmm_hide_hits);
    sheng_fairplay_diag_puts(out, limit, &at, " fairplay_0x899_uses=");
    sheng_fairplay_diag_putu(out, limit, &at, fairplay_uses);
    sheng_fairplay_diag_puts(out, limit, &at, " nonce_0x8a4_uses=");
    sheng_fairplay_diag_putu(out, limit, &at, nonce_uses);
    sheng_fairplay_diag_puts(out, limit, &at, " unsupported_uses=");
    sheng_fairplay_diag_putu(out, limit, &at, unsupported_uses);
    sheng_fairplay_diag_puts(out, limit, &at, " last_unsupported_selector=");
    sheng_fairplay_diag_putx(out, limit, &at, last_unsupported_selector);
    sheng_fairplay_diag_puts(out, limit, &at, " vmm_hide=");
    sheng_fairplay_diag_puts(out, limit, &at, (vmm_hide && vmm_hide[0]) ? vmm_hide : "none");
    out[at++] = '\n';
    out[at] = '\0';
    return at;
}

#endif
