/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HOST_KERN_THREAD_H
#define HOST_KERN_THREAD_H
typedef int wait_result_t;
typedef void *thread_t;
#define KERN_SUCCESS 0
int kernel_thread_start(void (*)(void *, wait_result_t), void *, thread_t *);
static inline void thread_deallocate(thread_t) {}
#endif
