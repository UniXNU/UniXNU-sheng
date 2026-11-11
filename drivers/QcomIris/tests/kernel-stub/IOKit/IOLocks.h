/* SPDX-License-Identifier: GPL-2.0-only */
/* Host stand-ins for the kernel's locks: one thread runs the test, so a lock only counts its holds. */
#ifndef HOST_IOLOCKS_H
#define HOST_IOLOCKS_H
struct IOLock { int held; };
static inline void IOLockLock(IOLock *l) { ++l->held; }
static inline void IOLockUnlock(IOLock *l) { --l->held; }
static inline void IOLockWakeup(IOLock *, void *, bool) {}
#define THREAD_UNINT 0
int IOLockSleep(IOLock *, void *, int);
#endif
