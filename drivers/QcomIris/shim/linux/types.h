/* SPDX-License-Identifier: GPL-2.0-only */
/* The kernel types and helpers the pinned iris buffer calculator uses, for both the XNU build and the host tests. */
#ifndef QCOM_IRIS_SHIM_LINUX_TYPES_H
#define QCOM_IRIS_SHIM_LINUX_TYPES_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#define BIT(n) (1U << (n))
#define SZ_4K 0x1000
#define ALIGN(x, a) (((x) + ((__typeof__(x))(a) - 1)) & ~((__typeof__(x))(a) - 1))   /* __ALIGN_KERNEL */
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define __maybe_unused __attribute__((unused))
/* The calculator's helpers, for C only: C++ users of the HFI definitions (the kernel glue) see the system's own. */
#ifndef __cplusplus
#ifndef max
#define max(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#endif
#ifndef min
#define min(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#endif
#define max_t(t, a, b) ({ t _a = (t)(a); t _b = (t)(b); _a > _b ? _a : _b; })
#define min_t(t, a, b) ({ t _a = (t)(a); t _b = (t)(b); _a < _b ? _a : _b; })
#define max3(a, b, c) max(max(a, b), c)
/* compiler_types.h: the kernel's inline has GNU semantics, so a plain inline definition is also emitted out of line */
#define inline inline __attribute__((__gnu_inline__))
#endif
#endif
