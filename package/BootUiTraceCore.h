// SPDX-License-Identifier: MIT
// Bounded stack slices, not a stopped-task or atomic stack snapshot.
#ifndef SHENG_BOOT_UI_TRACE_CORE_H
#define SHENG_BOOT_UI_TRACE_CORE_H
#define BT_MAX_THREADS 64
#define BT_MAX_GROUPS (2*BT_MAX_THREADS)
#define BT_FRAMES 18   /* the banner shows 8; the boot log's deep rows the rest (a blocked thread's caller names what it waits for) */
#define BT_ROWS 14
#define BT_HALF 60
struct bt_entry {
 uint64_t pc,lr,fp,frames[BT_FRAMES],regs[8],trap;
 int peer;unsigned peer_code;
 unsigned index,count,error;
 char role;
};
struct bt_groups {struct bt_entry entries[BT_MAX_GROUPS];unsigned count,threads,read_failures;};
typedef int (*bt_reader)(void *,uint64_t,void *,unsigned);
static int bt_user(uint64_t address,unsigned size) {
 return address>=4096 && address<(UINT64_C(1)<<39) && size<=(UINT64_C(1)<<39)-address;
}
static void bt_walk_n(struct bt_entry *e,bt_reader read,void *context,uint64_t *frames,unsigned limit) {
 uint64_t fp=e->fp;
 for(unsigned i=0;i<limit && fp;++i) {
  uint64_t pair[2]={0};
  if(!bt_user(fp,sizeof(pair)) || (fp&15)){e->error=2;break;}
  if(read(context,fp,pair,sizeof(pair))){e->error=1;break;}
  if(pair[1] && !bt_user(pair[1],4)){e->error=2;break;}
  frames[i]=pair[1];
  if(!pair[0])break;
  if(pair[0]<=fp || pair[0]-fp>1048576){e->error=3;break;}
  fp=pair[0];
 }
}
static void bt_walk(struct bt_entry *e,bt_reader read,void *context) {
 bt_walk_n(e,read,context,e->frames,BT_FRAMES);
}
// Exact integer encoding, not truncated pointers. Seven radix-64 digits cover
// all native 39-bit user addresses. This alphabet avoids punctuation separators.
static void bt_address(char out[9],uint64_t value) {
 static const char digits[]="0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_";
 char reverse[11];unsigned n=0;
 do{reverse[n++]=digits[value&63];value>>=6;}while(value && n<sizeof(reverse));
 if(n>8){out[0]='?';out[1]=0;return;}
 for(unsigned i=0;i<n;++i)out[i]=reverse[n-i-1];out[n]=0;
}
static int bt_same(const struct bt_entry *a,const struct bt_entry *b) {
 return a->role==b->role && a->pc==b->pc && a->lr==b->lr && a->error==b->error && a->peer==b->peer && a->peer_code==b->peer_code &&
  !memcmp(a->frames,b->frames,sizeof(a->frames));
}
static void bt_add(struct bt_groups *g,const struct bt_entry *entry) {
 ++g->threads;g->read_failures+=entry->error==1;
 for(unsigned i=0;i<g->count;++i)if(bt_same(&g->entries[i],entry)){++g->entries[i].count;return;}
 if(g->count<BT_MAX_GROUPS){g->entries[g->count]=*entry;g->entries[g->count++].count=1;}
}
static void bt_format(const struct bt_entry *e,unsigned half,char out[BT_HALF]) {
 char a[6][9];
 if(!half) {
  bt_address(a[0],e->pc);bt_address(a[1],e->lr);
  for(unsigned i=0;i<4;++i)bt_address(a[i+2],e->frames[i]);
  snprintf(out,BT_HALF,"%c%02ux%u %s %s %s %s %s %s !%u",
   e->role,e->index,e->count,a[0],a[1],a[2],a[3],a[4],a[5],e->error);
 } else {
  for(unsigned i=0;i<4;++i)bt_address(a[i],e->frames[i+4]);
  snprintf(out,BT_HALF,"%c%02ux%u f5 %s %s %s %s p%d/e%u",
   e->role,e->index,e->count,a[0],a[1],a[2],a[3],e->peer,e->peer_code);
 }
}
static void bt_rows(const struct bt_groups *g,unsigned page,char rows[BT_ROWS][120]) {
 unsigned start=page*BT_ROWS;
 for(unsigned row=0;row<BT_ROWS;++row) {
  memset(rows[row],' ',119);rows[row][119]=0;
  for(unsigned column=0;column<2;++column) {
   unsigned n=start+(row/2)*2+column;
   if(n>=g->count)continue;
   char text[BT_HALF];bt_format(&g->entries[n],row%2,text);
   memcpy(rows[row]+column*BT_HALF,text,strlen(text));
  }
 }
}
#endif
