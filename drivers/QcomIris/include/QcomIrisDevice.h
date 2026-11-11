/* SPDX-License-Identifier: GPL-2.0-only */
/* What the board gives the iris driver's kernel side: the core with its lock, memory the VPU can address, and the session plumbing.
 * On sheng it is package/VideoProbe.cpp. Every call except QcomIrisDeviceReady and the lock itself is made with the lock held. */
#ifndef QCOM_IRIS_DEVICE_H
#define QCOM_IRIS_DEVICE_H
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
extern "C" {
#include "QcomIrisCore.h"
}
struct QcomIrisBuffer {
    IOMemoryDescriptor *memory;    /* retained and prepared while mapped */
    uint32_t iova;                 /* the device address of offset */
    uint64_t offset, bytes;        /* the mapped part of memory, page aligned */
    void *cpu;                     /* the kernel's mapping, for buffers QcomIrisDeviceAllocate made */
    IOMemoryMap *map;
};
/* True once the platform is set up (the SMMU domain, the queues, the interrupt and the threads). */
bool QcomIrisDeviceReady(void);
IOLock *QcomIrisDeviceLock(void);
struct qcom_iris_core *QcomIrisDeviceCore(void);
/* Wired memory, zeroed, mapped for the VPU and the kernel. */
int QcomIrisDeviceAllocate(uint64_t bytes, QcomIrisBuffer *);
/* An existing buffer (an IOSurface's memory): prepared and mapped for the VPU from the page holding offset to the one holding its
 * last byte; iova then addresses that first page, so the buffer starts at iova + (offset & 4095). */
int QcomIrisDeviceMap(IOMemoryDescriptor *, uint64_t offset, uint64_t length, QcomIrisBuffer *);
void QcomIrisDeviceRelease(QcomIrisBuffer *);
/* The session ops' plumbing: the command write, the wait for a response (the lock dropped while asleep) and its wake-up. */
int QcomIrisDeviceSend(const void *message, uint32_t size);
bool QcomIrisDeviceWait(struct qcom_iris_session *, unsigned milliseconds);
void QcomIrisDeviceSignal(struct qcom_iris_session *);
/* Internal buffers for the session ops' allocate and release: handle is a QcomIrisBuffer. */
int QcomIrisDeviceAllocateInternal(uint32_t bytes, uint64_t *device_addr, void **handle);
void QcomIrisDeviceReleaseInternal(void *handle);
#endif
