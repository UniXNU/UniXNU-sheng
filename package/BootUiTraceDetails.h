// SPDX-License-Identifier: MIT
#ifndef SHENG_BOOT_UI_TRACE_DETAILS_H
#define SHENG_BOOT_UI_TRACE_DETAILS_H
// kernel, kstate, khint, kevent: the main thread's kernel side when it was sampled waiting (its kernel return addresses, thread state,
// block hint and wait event), which tells a thread blocked inside a page fault or a trap from one running in user mode.
struct bt_main {struct bt_entry entry;uint64_t frames[32],image;unsigned error,image_error;int pid;unsigned slide;
                uint64_t kernel[12],kevent;unsigned kstate,khint;};
static char bt_role_char(unsigned role,const struct bt_main *m) {
 return role==0?(m->entry.role?m->entry.role:'B'):"BSTU"[role&3];
}
// Two roles per page (first_role, first_role+1); roles beyond the captured set stay blank.
static void bt_main_rows(const struct bt_main *main,unsigned first_role,char rows[BT_ROWS][120]) {
 memset(rows,0,BT_ROWS*120);
 for(unsigned pair=0;pair<2;++pair) {
  unsigned role=first_role+pair;
  const struct bt_main *m=&main[role];const struct bt_entry *e=&m->entry;unsigned row=pair*7;
  if(!m->pid)continue;
  snprintf(rows[row],120,"MAIN %c p%d image=%llx err=%u cache+=%x PC=%llx LR=%llx FPerr=%u",
   bt_role_char(role,m),m->pid,m->image,m->image_error,m->slide,e->pc,e->lr,m->error);
  snprintf(rows[row+1],120,"trap=%llx opt=%llx remote=%x recv=%x msgid=%x peer=%d e%u (sampled, not atomic)",
   e->trap,e->regs[1],(unsigned)e->regs[3],(unsigned)(e->regs[5]>>32),(unsigned)(e->regs[4]>>32),e->peer,e->peer_code);
  for(unsigned slice=0;slice<4;++slice) {
   char a[8][9];for(unsigned i=0;i<8;++i)bt_address(a[i],m->frames[slice*8+i]);
   snprintf(rows[row+2+slice],120,"%c FP%02u %s %s %s %s %s %s %s %s",
    bt_role_char(role,m),slice*8+1,a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7]);
  }
  snprintf(rows[row+6],120,"peer e0=owner(-1=none) e1=not Mach2 e2=no remote e3=no space e4=lookup failed e5=receive arg candidate; FPerr 0/1/2/3");
 }
}
#endif
