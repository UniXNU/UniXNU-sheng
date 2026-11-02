/* SPDX-License-Identifier: MIT */
#ifndef BOOT_LATENCY_IPC_H
#define BOOT_LATENCY_IPC_H
#include <stdint.h>
enum {BlIpcCapacity=8192,BlIpcProbe=8};
struct bl_ipc_slot {uint32_t busy,id;uint64_t key,request;};
struct bl_ipc_filter {
 struct bl_ipc_slot messages[BlIpcCapacity],replies[BlIpcCapacity];
 uint64_t unpaired,untracked;
};
/* Bounded nonblocking lookup. Exhaustion is reported, never hidden as a match. */
static inline unsigned bl_ipc_hash(uint64_t key) {return (unsigned)((key>>4)^(key>>19))&(BlIpcCapacity-1);}
static inline int bl_ipc_remember(struct bl_ipc_slot *slots,uint64_t key,uint32_t id,uint64_t request,int reply) {
 if(!key)return 1;
 for(unsigned i=0;i<BlIpcProbe;++i) {
  struct bl_ipc_slot *s=&slots[(bl_ipc_hash(key)+i)&(BlIpcCapacity-1)];uint32_t free=0;
  if(!__atomic_compare_exchange_n(&s->busy,&free,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))continue;
  int found=!s->key || (s->key==key && s->id==id);
  if(found) {
   if(reply && s->key==key && s->request!=request) {
    s->request=UINT64_MAX;__atomic_store_n(&s->busy,0,__ATOMIC_RELEASE);return 0;
   }
   s->key=key;s->id=id;s->request=request;
  }
  __atomic_store_n(&s->busy,0,__ATOMIC_RELEASE);if(found)return 1;
 }
 return 0;
}
static inline uint64_t bl_ipc_take(struct bl_ipc_slot *slots,uint64_t key,uint32_t id) {
 if(!key)return 0;
 for(unsigned i=0;i<BlIpcProbe;++i) {
  struct bl_ipc_slot *s=&slots[(bl_ipc_hash(key)+i)&(BlIpcCapacity-1)];uint32_t free=0;
  if(!__atomic_compare_exchange_n(&s->busy,&free,1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))continue;
  uint64_t request=s->key==key && s->id==id?s->request:0;
  if(request){s->key=0;s->request=0;}
  __atomic_store_n(&s->busy,0,__ATOMIC_RELEASE);if(request)return request;
 }
 return 0;
}
static inline void bl_ipc_sent(struct bl_ipc_filter *f,uint64_t cookie,uint32_t id,uint64_t reply,int inbound) {
 if(inbound && !bl_ipc_remember(f->messages,cookie,id,cookie,0))__atomic_fetch_add(&f->untracked,1,__ATOMIC_RELAXED);
 if(reply && !bl_ipc_remember(f->replies,reply,0,cookie,1))__atomic_fetch_add(&f->untracked,1,__ATOMIC_RELAXED);
}
static inline int bl_ipc_received(struct bl_ipc_filter *f,uint64_t cookie,uint32_t id,uint64_t port,uint64_t *request) {
 uint64_t sent=bl_ipc_take(f->messages,cookie,id);
 *request=bl_ipc_take(f->replies,port,0);
 if(*request==UINT64_MAX){*request=0;__atomic_fetch_add(&f->untracked,1,__ATOMIC_RELAXED);}
 if(sent || *request)return 1;
 __atomic_fetch_add(&f->unpaired,1,__ATOMIC_RELAXED);return 0;
}
#endif
