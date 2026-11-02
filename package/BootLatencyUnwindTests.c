/* SPDX-License-Identifier: MIT */
#include "BootLatencyUnwind.h"
#include <assert.h>
struct memory {unsigned calls;int error;uint64_t next;};
static int read_frame(void *context,uint64_t address,uint64_t pair[2]) {
 struct memory *m=context;++m->calls;if(m->error)return m->error;
 pair[0]=m->next?m->next:(address==0x1000?0x1020:0);pair[1]=address+8;return 0;
}
int main(void) {
 uint64_t frames[8]={0};struct memory m={0,0,0};
 struct bl_unwind_result r=bl_unwind(0x1000,0x1000,1,2,frames,8,read_frame,&m);
 assert(r.count==4 && !r.flags && m.calls==2 && frames[2]==0x1008);
 m=(struct memory){0,3,0};r=bl_unwind(0x1000,0x1000,1,2,frames,8,read_frame,&m);assert(r.flags==1 && r.error==3 && r.count==2);
 m=(struct memory){0,0,0x1000};r=bl_unwind(0x1000,0x1000,1,2,frames,8,read_frame,&m);assert(r.flags==2 && m.calls==1);
 m=(struct memory){0,0,0};r=bl_unwind(0x1000,0x1000,1,2,frames,3,read_frame,&m);assert(r.flags==4 && m.calls==1);
 m=(struct memory){0,0,0};r=bl_unwind(0x1008,0x1000,1,2,frames,8,read_frame,&m);assert(r.flags==2 && !m.calls);
}
