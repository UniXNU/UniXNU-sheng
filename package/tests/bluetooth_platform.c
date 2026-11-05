/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Sm8550GeniUart.h"
#include "Sm8550QupDma.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct model {uint32_t regs[0xd0000/4];unsigned writes,delays,published,stream_writes,smr_reads[128];bool clock_stuck,tlb_stuck,selection_only;};
static uint32_t rd(void *p,uint32_t r) {
    struct model *m=p;assert(r<sizeof(m->regs));
    if(m->selection_only) {
        assert(r<0x20000);
        if(r>=0x800 && r<0xa00)assert(m->smr_reads[(r-0x800)/4]++==0);
    }
    if(r==0x1e760 && !m->clock_stuck)m->regs[r/4]&=~1u;
    if(m->tlb_stuck && r>0x10000 && (r&0xfff)==0x7f4)return 1;
    return m->regs[r/4];
}
static void wr(void *p,uint32_t r,uint32_t v) {
    struct model *m=p;assert(r<sizeof(m->regs));m->writes++;
    if(r>=0x800 && r<0xa00 && (v>>31)){assert(m->published);m->stream_writes++;}
    m->regs[r/4]=v;
}
static void delay(void *p,unsigned us){struct model *m=p;assert(us==1);m->delays++;}
static void publish(void *p,void *va,size_t bytes){struct model *m=p;assert(va && bytes>=3*4096 && bytes<=4*4096);m->published++;}
static void init(struct model *m) {
    memset(m,0,sizeof(*m));m->regs[0]=1u<<10;
    m->regs[0x20/4]=(1u<<30)|(1u<<27)|16;m->regs[0x24/4]=0x40000008;m->regs[0x28/4]=0x1111;
    /* Root storage and a shared bypass bank remain owned by their drivers. */
    m->regs[0x800/4]=0x80000060;m->regs[0xc00/4]=1;
    m->regs[(32+1)*4096/4]=1;m->regs[((32+1)*4096+0x24)/4]=1u<<16;
    m->regs[(0x1000+4)/4]=0x1f300;m->regs[(0x1800+4)/4]=1;
    m->regs[0x804/4]=0x800000a3;m->regs[0xc04/4]=7;
    m->regs[(0x1000+7*4)/4]=0x10000;m->regs[(0x1800+7*4)/4]=1;
}
static uint64_t physical(uint64_t *tables,uint64_t tpa,uint64_t iova) {
    size_t at=0;for(unsigned level=1;level<4;level++) {
        uint64_t d=tables[at+((iova>>(39-9*level))&511)];assert((d&3)==3);
        if(level==3)return d&0x0000fffffffff000ULL;
        uint64_t pa=d&0x0000fffffffff000ULL;assert(pa>=tpa && pa<tpa+16384);at=(pa-tpa)/8;
    }abort();
}
int main(void) {
    struct model *m=calloc(1,sizeof(*m));assert(m);
    struct sm8550_geni_clock_io clock={m,rd,wr,delay};uint32_t hz=0;
    const uint32_t rates[]={115200,3000000,3200000,4000000},freq[]={7372800,48000000,51200000,64000000};
    for(unsigned i=0;i<4;i++) {
        init(m);m->regs[(0x1e760+0x14)/4]=1;m->regs[(0x1e760+4)/4]=0x80000000;
        assert(!sm8550_geni_uart14_source(&clock,rates[i],&hz) && hz==freq[i]);
        assert(!m->regs[(0x1e760+0x14)/4]);assert(m->regs[(0x1e760+4)/4]==0x80002601);
        uint32_t n=m->regs[(0x1e760+8)/4]+(~m->regs[(0x1e760+12)/4]&65535);
        assert(300000000ULL*m->regs[(0x1e760+8)/4]/n==freq[i]);
    }
    unsigned before=m->writes;assert(sm8550_geni_uart14_source(&clock,123,&hz)==-EINVAL && m->writes==before);
    m->clock_stuck=true;assert(sm8550_geni_uart14_source(&clock,3200000,&hz)==-ETIMEDOUT && m->delays==1000);
    uint64_t *tables=aligned_alloc(4096,16384);assert(tables);struct sm8550_qup_dma_io io={m,rd,wr,delay,publish};
    struct sm8550_qup_dma d={0};init(m);
    /* A disabled bank may retain SCTLR configuration bits with M cleared. */
    m->regs[(32+2)*4096/4]=0x00c01020;
    assert(!sm8550_qup_dma_attach(&io,&d,0x423,0x109000000ULL,32768,0x108000000ULL,tables,4));
    assert(d.active && !d.poisoned && d.bank==2 && d.slot==2 && d.asid==2 && m->stream_writes==1);
    for(unsigned page=0;page<8;page++)assert(physical(tables,0x108000000ULL,SM8550_QUP_DMA_IOVA+page*4096)==0x109000000ULL+page*4096);
    assert(m->regs[0x800/4]==0x80000060 && m->regs[0xc00/4]==1 && m->regs[0x804/4]==0x800000a3 && m->regs[0xc04/4]==7);
    d=(struct sm8550_qup_dma){0};before=m->writes;
    assert(sm8550_qup_dma_attach(&io,&d,0x423,0x109000000ULL,32768,0x108000000ULL,tables,4)==-EBUSY && m->writes==before);
    init(m);m->tlb_stuck=true;d=(struct sm8550_qup_dma){0};
    assert(sm8550_qup_dma_attach(&io,&d,0x423,0x109000000ULL,32768,0x108000000ULL,tables,4)==-ETIMEDOUT);
    assert(d.poisoned && !d.active && !m->stream_writes && m->delays==1000);
    init(m);m->regs[0x20/4]|=1u<<8;d=(struct sm8550_qup_dma){0};
    assert(sm8550_qup_dma_attach(&io,&d,0x423,0x109000000ULL,32768,0x108000000ULL,tables,4)==-ENOTSUP && !m->writes);
    init(m);m->selection_only=true;d=(struct sm8550_qup_dma){0};
    for(unsigned i=2;i<7;i++)m->regs[(0x1000+i*4)/4]=0x1f300;
    assert(sm8550_qup_dma_attach(&io,&d,0x423,0x109000000ULL,32768,0x108000000ULL,tables,4)==-ENOSPC);
    assert(d.exhausted==2 && d.free_slots==14 && d.unused_banks==5 && d.rejected_config==5 &&
        d.rejected_cbar==0x1f300 && !m->writes);
    init(m);d=(struct sm8550_qup_dma){0};m->regs[(32+2)*4096/4]=0x00c01021;
    assert(!sm8550_qup_dma_attach(&io,&d,0x423,0x109000000ULL,32768,0x108000000ULL,tables,4) && d.bank==3);
    free(tables);free(m);
    puts("PASS: exact UART rates, DFS handoff, bounded clock/TLB timeout, isolated QUP DMA tables, ASID allocation and preserved storage/touch routes");
}
