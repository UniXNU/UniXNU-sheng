// Host test for TouchBoard.h helpers against hand-computed values from the cited Linux tables.
#include "TouchBoard.h"
#include "SmmuBypass.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(c) do{if(!(c)){printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
// Register file standing in for the apps SMMU: IDR0 stage-1+AArch64 with 8 groups, IDR1 geometry for 4 KiB pages and 4 banks
// (page count field 1 -> 4 pages), IDR2 address widths 36 bits everywhere with VA64.
struct fake {uint32_t reg[0xd0000 / 4]; unsigned writes;};
static uint32_t frd(void *c, unsigned o) {return ((struct fake *)c)->reg[o / 4];}
static void fwr(void *c, unsigned o, uint32_t v) {((struct fake *)c)->reg[o / 4] = v; ((struct fake *)c)->writes++;}
static void fwr_drop_s2cr(void *c, unsigned o, uint32_t v) {if (o >= 0xc00 && o < 0x1000) return; ((struct fake *)c)->reg[o / 4] = v;}
static void fresh(struct fake *f) {
    memset(f, 0, sizeof *f);
    f->reg[0x20 / 4] = (1u << 30) | 8;
    f->reg[0x24 / 4] = (1u << 28) | 4;
    f->reg[0x28 / 4] = (1u << 12) | 0x111;
}
static void smmu_tests(void) {
    struct fake f; struct smmu_io io = {&f, frd, fwr};
    // fresh bypass: bank 3 (last), pages = 4 -> bank page at (4+3)*4096
    fresh(&f);
    struct smmu_plan p = smmu_plan_bypass(&io, 0xa3, 0xc8000000ULL, 0x20000);
    CHECK(!p.error && p.slot == 0 && p.bank == 3 && p.pages == 4 && !p.reuse && !p.bank_ready);
    CHECK(!smmu_configure_bypass(&io, &p, 0xa3));
    CHECK(f.reg[0x800 / 4] == (0x80000000u | 0xa3) && f.reg[0xc00 / 4] == 3 && f.reg[0x1000 / 4 + 3] == 0x10000 && f.reg[0x1800 / 4 + 3] == 1);
    // second stream (UFS 0x60) shares the configured bank and takes the next group
    p = smmu_plan_bypass(&io, 0x60, 0xc8000000ULL, 0x1000);
    CHECK(!p.error && p.slot == 1 && p.bank_ready && !p.reuse);
    CHECK(!smmu_configure_bypass(&io, &p, 0x60) && f.reg[0x804 / 4] == (0x80000000u | 0x60));
    // idempotent: the first stream finds itself, no write needed beyond verification
    p = smmu_plan_bypass(&io, 0xa3, 0xc8000000ULL, 0x1000);
    CHECK(!p.error && p.reuse && p.slot == 0);
    unsigned w = f.writes; CHECK(!smmu_configure_bypass(&io, &p, 0xa3) && f.writes == w);
    // conflicts: a masked entry matching the stream, or the same stream pointing at another bank
    fresh(&f); f.reg[0x800 / 4] = 0x80000000u | (0x7fu << 16); f.reg[0xc00 / 4] = 3;
    p = smmu_plan_bypass(&io, 0xa3, 0xc8000000ULL, 0x1000); CHECK(p.error);
    fresh(&f); f.reg[0x800 / 4] = 0x80000000u | 0xa3; f.reg[0xc00 / 4] = 1;
    p = smmu_plan_bypass(&io, 0xa3, 0xc8000000ULL, 0x1000); CHECK(p.error);
    // UART14 joins the same firmware bypass bank with a 64-bit DMA address.
    fresh(&f);
    p = smmu_plan_bypass(&io, 0xa3, 0xc8000000ULL, 0x20000);
    CHECK(!p.error && !smmu_configure_bypass(&io, &p, 0xa3));
    unsigned shared_bank=p.bank, before=f.writes;
    p = smmu_plan_bypass(&io, 0x423, 0x103000000ULL, 32768);
    CHECK(!p.error && p.bank==shared_bank && p.bank_ready && !p.reuse);
    CHECK(!smmu_configure_bypass(&io,&p,0x423) && f.writes==before+2);
    CHECK(f.reg[0x800/4]==(0x80000000u|0xa3) && f.reg[(0x800+4*p.slot)/4]==(0x80000000u|0x423));
    // another stream already uses the bank but the bank is not a bypass bank: refused, nothing written
    fresh(&f); f.reg[0x800 / 4] = 0x80000000u | 0x60; f.reg[0xc00 / 4] = 3; f.reg[0x1000 / 4 + 3] = 0;
    p = smmu_plan_bypass(&io, 0xa3, 0xc8000000ULL, 0x1000); CHECK(p.error && f.writes == 0);
    // translation enabled on the bank
    fresh(&f); f.reg[(4 + 3) * 1024] = 1;
    p = smmu_plan_bypass(&io, 0xa3, 0xc8000000ULL, 0x1000); CHECK(p.error);
    // all groups used
    fresh(&f); for (unsigned i = 0; i < 8; i++) {f.reg[0x800 / 4 + i] = 0x80000000u | (0x10 + i); f.reg[0xc00 / 4 + i] = 0;}
    p = smmu_plan_bypass(&io, 0xa3, 0xc8000000ULL, 0x1000); CHECK(p.error);
    // DMA beyond 36 bits
    fresh(&f); p = smmu_plan_bypass(&io, 0xa3, 1ULL << 36, 0x1000); CHECK(p.error);
    // no stage-1 / no VA64
    fresh(&f); f.reg[0x20 / 4] = 8; p = smmu_plan_bypass(&io, 0xa3, 0xc8000000ULL, 0x1000); CHECK(p.error);
    // a write that does not read back is reported
    fresh(&f); p = smmu_plan_bypass(&io, 0xa3, 0xc8000000ULL, 0x1000);
    struct smmu_io bad = {&f, frd, fwr_drop_s2cr};
    CHECK(smmu_configure_bypass(&bad, &p, 0xa3));
}

int main(void) {
    smmu_tests();
    // TLMM address of gpio44 control and the configured value for qup1_se4 at 6 mA.
    CHECK(tb_tlmm_ctl(44)==0x2c000 && tb_tlmm_io(25)==0x19004);
    CHECK(tb_pin_cfg(TB_FUNC_QUP1_SE4,TB_DRIVE_6MA)==0x84);
    // Frequency table entries from ftbl_gcc_qupv3_wrap1_s0_clk_src: 100 MHz = GPLL0 main (src 1), hid div 6 -> field 11.
    CHECK(tb_dfs_level_hz((1u<<8)|11,0,0)==100000000ULL);
    // 75 MHz = GPLL0 even (src 6) / 4 -> field 7.
    CHECK(tb_dfs_level_hz((6u<<8)|7,0,0)==75000000ULL);
    // 19.2 MHz TCXO, bypass.
    CHECK(tb_dfs_level_hz(0,0,0)==19200000ULL);
    // 48 MHz = even, M/N 4/25: mode dual edge (2), hid 1 field 0 -> N register stores ~(n-m) = ~21.
    CHECK(tb_dfs_level_hz((6u<<8)|(2u<<12)|0,4,(~21u)&0xffff)==48000000ULL);
    uint64_t lv[TB_DFS_LEVELS]={19200000ULL,100000000ULL,75000000ULL,0,0,0,0,0};
    struct tb_clock_choice c;
    CHECK(!tb_choose_clock(lv,TB_DFS_LEVELS,1,19200000,&c) && c.level==0 && c.div==1 && c.actual_hz==19200000ULL);
    CHECK(!tb_choose_clock(lv,TB_DFS_LEVELS,1,9600000,&c) && c.actual_hz==9600000ULL);   // 19.2/2 or 100/11 -> 9.09; picks 9.6
    CHECK(!tb_choose_clock(lv,TB_DFS_LEVELS,1,15000000,&c) && c.actual_hz<=15000000ULL && c.actual_hz>=14285714ULL);   // 100/7
    CHECK(!tb_choose_clock(lv,TB_DFS_LEVELS,2,19200000,&c) && c.actual_hz<=19200000ULL);
    uint64_t none[TB_DFS_LEVELS]={0};
    CHECK(tb_choose_clock(none,TB_DFS_LEVELS,1,9600000,&c)<0);
    puts("PASS: TouchBoard address, pin and DFS clock helpers; SMMU bypass planning against a register fixture");return 0;
}
