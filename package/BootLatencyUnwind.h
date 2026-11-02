/* SPDX-License-Identifier: MIT */
#ifndef BOOT_LATENCY_UNWIND_H
#define BOOT_LATENCY_UNWIND_H
#include <stdint.h>
struct bl_unwind_result {unsigned count,flags;int error;};
typedef int (*bl_read_frame)(void *,uint64_t,uint64_t[2]);
static inline struct bl_unwind_result bl_unwind(uint64_t fp,uint64_t sp,uint64_t pc,uint64_t lr,
                                               uint64_t *frames,unsigned capacity,bl_read_frame read,void *context) {
 struct bl_unwind_result out={0,0,0};
 if(capacity<2){out.flags=4;return out;}
 frames[0]=pc;frames[1]=lr;out.count=2;fp&=0x00007fffffffffffULL;sp&=0x00007fffffffffffULL;
 while(fp && out.count<capacity) {
  if((fp&15) || fp<sp || fp>0x00007ffffffffff0ULL){out.flags|=2;break;}
  uint64_t pair[2]={0};out.error=read(context,fp,pair);if(out.error){out.flags|=1;break;}
  frames[out.count++]=pair[1];uint64_t next=pair[0]&0x00007fffffffffffULL;
  if(next && (next<=fp || next-fp>1048576)){out.flags|=2;break;}fp=next;
 }
 if(out.count==capacity && fp)out.flags|=4;
 return out;
}
#endif
