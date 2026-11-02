/* SPDX-License-Identifier: MIT */
#ifndef BOOT_LATENCY_STORE_H
#define BOOT_LATENCY_STORE_H
#include <stdint.h>
#include <string.h>
struct bl_record {uint64_t us,tid;uint32_t kind,pid;uint64_t value[6];};
struct bl_slot {uint32_t busy;uint64_t sequence;struct bl_record row;};
struct bl_store {struct bl_slot *slots;uint32_t capacity;uint64_t issued,dropped;};
static inline int bl_push(struct bl_store *s,const struct bl_record *r) {
 if(!s->slots)return 0;
 uint64_t seq=__atomic_add_fetch(&s->issued,1,__ATOMIC_RELAXED);
 struct bl_slot *slot=&s->slots[(seq-1)%s->capacity];uint32_t free=0;
 if(!__atomic_compare_exchange_n(&slot->busy,&free,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
  __atomic_fetch_add(&s->dropped,1,__ATOMIC_RELAXED);return 0;
 }
 if(slot->sequence<seq){slot->row=*r;slot->sequence=seq;}
 else __atomic_fetch_add(&s->dropped,1,__ATOMIC_RELAXED);
 __atomic_store_n(&slot->busy,0,__ATOMIC_RELEASE);return 1;
}
static inline int bl_read(struct bl_store *s,uint64_t seq,struct bl_record *r) {
 struct bl_slot *slot=&s->slots[(seq-1)%s->capacity];uint32_t free=0;
 if(!__atomic_compare_exchange_n(&slot->busy,&free,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 0;
 int found=slot->sequence==seq;if(found)*r=slot->row;
 __atomic_store_n(&slot->busy,0,__ATOMIC_RELEASE);return found;
}
#endif
