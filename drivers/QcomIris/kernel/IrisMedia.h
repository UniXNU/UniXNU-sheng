/* SPDX-License-Identifier: GPL-2.0-only */
/* The iris decoder as a stateful V4L2 decoder device, for the VideoToolbox host: sessions, the handful of ioctls a decoder client
 * uses (64-bit Linux layouts and numbers) and the dequeued buffers and events through a sink, as iris_vidc.c, iris_vdec.c and
 * iris_vb2.c give them to user space. Buffers are the client's memory (USERPTR): a queued buffer is mapped for the VPU from its
 * memory descriptor and stays mapped while the client queues the same memory at that index. The VPU writes pictures past the
 * CPU's caches: a client that reads one from memory it read before drops its cache lines over it first. */
#ifndef QCOM_IRIS_MEDIA_H
#define QCOM_IRIS_MEDIA_H
#include <stdint.h>
class IOMemoryDescriptor;
enum : unsigned { IrisMediaDecoder = 0, IrisMediaDevices = 1 };
unsigned iris_media_devices(void);
/* True once the VPU platform is ready. */
bool iris_media_is_up(unsigned device);
const char *iris_media_card(unsigned device);
/* Dequeued buffers and events: kind u32, session u32, then a v4l2_buffer with one plane (kind 1) or the event type u32 (kind 2);
 * kind 0 says the session failed. Called from the device's event thread with no lock held. */
void iris_media_set_sink(unsigned device, void (*sink)(void *context, const uint8_t *event, uint32_t size), void *context);
/* 0 or a Linux errno. */
int iris_media_open(unsigned device, uint32_t *session);
int iris_media_close(unsigned device, uint32_t session);
int iris_media_ioctl(unsigned device, uint32_t session, uint32_t code, void *payload, uint32_t out, uint32_t in);
/* VIDIOC_QBUF of a USERPTR buffer: `memory` holds the plane (prepared by the caller, at least the plane's length). */
int iris_media_qbuf(unsigned device, uint32_t session, void *v4l2_buffer, IOMemoryDescriptor *memory);
#endif
