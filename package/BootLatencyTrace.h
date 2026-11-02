/* SPDX-License-Identifier: MIT */
#ifndef BOOT_LATENCY_TRACE_H
#define BOOT_LATENCY_TRACE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* 1 IPC send attempt, 2 receive, 3 exception, 4 GPU vote, 5 touch publication. */
void boot_latency_record(uint32_t kind,uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f);
void boot_latency_start(void);
int boot_latency_enabled(void);
void boot_latency_fault_drain(void);
void boot_latency_waiter(int pid,uint64_t tid,unsigned state,uint64_t wait,unsigned switches,int peer,unsigned peer_code,unsigned error,const uint64_t frames[12],const uint64_t kernel[12]);
uint32_t boot_latency_snapshot(void *out,uint32_t bytes);
#ifdef __cplusplus
}
#endif
#endif
