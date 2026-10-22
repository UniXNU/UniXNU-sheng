// SPDX-License-Identifier: MIT
// Caller supplies serialization. Keep failures separate from ordinary exits so
// successful short-lived helpers cannot erase the last useful crash records.
#ifndef SHENG_BOOT_PROGRESS_LIFECYCLE_H
#define SHENG_BOOT_PROGRESS_LIFECYCLE_H
struct bp_exits {
 unsigned total,failed,crashes;
 char recent[4][120],failures[3][120],crash[3][400];
 char reason[330];
 char thrown[330];
};
static void bp_exit_add(struct bp_exits *e,int pid,const char *name,int status,
                        unsigned ns,uint64_t code) {
 char *row=e->recent[e->total++%4];
 unsigned signal=(unsigned)status&127,exit_code=((unsigned)status>>8)&255;
 snprintf(row,120,"p%d %.24s status=%x sig=%u exit=%u ns=%u sub=%llx",
          pid,name,(unsigned)status,signal,exit_code,ns,(unsigned long long)code);
 if(signal>=3 && signal<=12 && signal!=9)snprintf(e->crash[e->crashes++%3],400,"%s",row);
 if(status || ns) {
  char *bad=e->failures[e->failed++%3];
  snprintf(bad,120,"%s",row);
 }
}
#endif
