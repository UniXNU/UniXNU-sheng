// SPDX-License-Identifier: BSD-2-Clause-Patent
// kern.unixnu_wifi_log (BootDiagExport.c): the WiFi module's ring of lines (WifiProbe.cpp native_wifi_text: `lines` slots, `written`
// lines so far, the newest in slot (written - 1) % lines), as text, oldest first, one line each. The ring is read while the module may
// write it: a slot without its terminator is cut at SHENG_WIFI_LOG_LINE - 1 bytes. Host-tested by tests/wifi_log_export_host.c.
#ifndef SHENG_WIFI_LOG_EXPORT_H
#define SHENG_WIFI_LOG_EXPORT_H
#include <stddef.h>
#include <stdint.h>

#define SHENG_WIFI_LOG_LINE 112u

// The bytes written to out (at most cap): whole lines only.
static inline size_t sheng_wifi_log_export(const char (*ring)[SHENG_WIFI_LOG_LINE], uint32_t lines, uint32_t written, char *out, size_t cap)
{
    size_t at = 0;
    if (!ring || !lines || !out) return 0;
    for (uint32_t i = written > lines ? written - lines : 0; i < written; ++i) {
        const char *line = ring[i % lines];
        size_t length = 0;
        while (length < SHENG_WIFI_LOG_LINE - 1 && line[length]) ++length;
        if (at + length + 1 > cap) break;
        for (size_t k = 0; k < length; ++k) out[at + k] = line[k];
        at += length;
        out[at++] = '\n';
    }
    return at;
}

#endif
