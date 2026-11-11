/* SPDX-License-Identifier: GPL-2.0-only */
/* Host stand-in for a memory descriptor: its length and the cache operations asked of it. */
#ifndef HOST_IOMEMORYDESCRIPTOR_H
#define HOST_IOMEMORYDESCRIPTOR_H
#include <stdint.h>
enum { kIOMemoryIncoherentIOFlush = 1 };
class IOMemoryMap;
class IOMemoryDescriptor {
public:
    uint64_t length = 0;
    unsigned flushes = 0;
    uint64_t flushed = 0;
    uint64_t getLength() const { return length; }
    int performOperation(int, uint64_t, uint64_t bytes) { ++flushes; flushed += bytes; return 0; }
};
#endif
