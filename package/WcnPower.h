/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SHENG_WCN_POWER_H
#define SHENG_WCN_POWER_H
#ifdef __cplusplus
extern "C" {
#endif
enum { SHENG_WCN_WLAN=0, SHENG_WCN_BLUETOOTH=1 };
/* One ownership vote per radio, idempotent until put. Return positive errno. */
int sheng_wcn_power_get(unsigned radio);
int sheng_wcn_power_put(unsigned radio);
unsigned sheng_wcn_power_owners(void);
#ifdef __cplusplus
}
#endif
#endif
