#!/usr/bin/env python3
"""Check production GIC dispatch and shared GCC voting used by Bluetooth."""
from pathlib import Path
import hashlib,json,subprocess
root=Path(__file__).resolve().parents[1];cpu=root.parent/'UniXNU/drivers/ArmCpu/ArmCpu.cpp';soc=root.parent/'UniXNU-sm8550-common/kits/Sm8550Platform'
s=cpu.read_text();a=s[s.index('#define ARMCPU_SPI_SLOTS'):s.index('IOService *ArmCpu::probe')]
b=s[s.index('static IOReturn spiAttachUnlocked'):s.index('extern "C" uint64_t armcpu_spi_count')]
gcc=(soc/'Sm8550Gcc.c').read_text().replace('#include <IOKit/IOLocks.h>','')
body=a+b+gcc
body=body.replace('__asm__ volatile ("dsb sy" ::: "memory");','__atomic_thread_fence(__ATOMIC_SEQ_CST);')
body=body.replace('__asm__ volatile("dsb sy" ::: "memory");','__atomic_thread_fence(__ATOMIC_SEQ_CST);')
prefix=r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <cstdio>
using IOLock=std::mutex;
static IOLock *IOLockAlloc(){return new IOLock;}
static void IOLockFree(IOLock *l){delete l;}
static void IOLockLock(IOLock *l){l->lock();}
static void IOLockUnlock(IOLock *l){l->unlock();}
using IOReturn=int;
enum {kIOReturnSuccess,kIOReturnNotReady,kIOReturnBadArgument,kIOReturnExclusiveAccess,kIOReturnNoResources,kIOReturnTimeout,kIOReturnNotFound};
static uint32_t memory[0x10000/4];
static uintptr_t gDist=reinterpret_cast<uintptr_t>(memory);
static unsigned armcpu_gic_ready=1;
static uint32_t read32(uintptr_t base,unsigned offset){assert(base==gDist && offset<sizeof(memory));return memory[offset/4];}
static void write32(uintptr_t base,unsigned offset,uint32_t value){
 assert(base==gDist && offset<sizeof(memory));
 if(offset>=0x100 && offset<0x180)memory[offset/4]|=value;
 else if(offset>=0x180 && offset<0x200)memory[(offset-0x80)/4]&=~value;
 else memory[offset/4]=value;
}
static bool waitClear(uintptr_t base,unsigned reg,uint32_t bits){return !(read32(base,reg)&bits);}
'''
tests=r'''
static void count(void *p){++*static_cast<unsigned *>(p);}
int main(){
 memory[1]=31;unsigned a=0,b=0,c=0,d=0;uint32_t flags=0;uint64_t router=0;
 assert(!armcpu_spi_attach(493,false,count,&a,&flags,&router));assert((flags&3)==3);
 assert(!armcpu_spi_share(240,count,&b));assert(!armcpu_spi_share(240,count,&c));
 unsigned used=gSpiUsed;assert(!armcpu_spi_share(240,count,&c) && gSpiUsed==used);
 assert(spiDispatch(240) && b==1 && c==1 && !a);
 assert(spiDispatch(493) && a==1);assert(!spiDispatch(499));
 assert(armcpu_spi_attach(240,false,count,&d,nullptr,nullptr)==kIOReturnExclusiveAccess);
 assert(!armcpu_spi_attach(500,true,count,&d,nullptr,nullptr));
 assert(armcpu_spi_share(500,count,&a)==kIOReturnBadArgument);
 assert(!armcpu_spi_mask(493,true));assert(!(memory[(0x100+(493/32)*4)/4]&(1u<<(493%32))));
 assert(!armcpu_spi_mask(493,false));assert(memory[(0x100+(493/32)*4)/4]&(1u<<(493%32)));
 assert(armcpu_spi_mask(501,true)==kIOReturnNotFound);
 std::thread first([&]{assert(!armcpu_spi_attach(496,false,count,&a,nullptr,nullptr));});
 std::thread second([&]{assert(!armcpu_spi_attach(497,false,count,&d,nullptr,nullptr));});
 first.join();second.join();assert(gSpiUsed==used+3);
 uint32_t votes=0xf;assert(sm8550_geni_uart14_vote(&votes,false) && votes==0xf);
 votes=0;assert(sm8550_geni_uart14_vote(&votes,true) && votes==0x40f);
 assert(sm8550_geni_uart14_vote(&votes,false) && !votes);
 assert(sm8550_geni_uart14_vote(&votes,true));assert(sm8550_gcc_update32(&votes,0,0x1f));
 assert(sm8550_geni_uart14_vote(&votes,false) && votes==0x1f);
 assert(sm8550_geni_uart14_vote(&votes,true));assert(sm8550_gcc_update32(&votes,0x10,1u<<17));
 assert(sm8550_geni_uart14_vote(&votes,false) && votes==0x2000f);
 assert(sm8550_gcc_update32(&votes,1u<<17,0));assert(sm8550_geni_uart14_vote(&votes,false) && !votes);
 uart14_claimed=false;votes=0xf;assert(sm8550_geni_uart14_vote(&votes,true));
 assert(sm8550_geni_uart14_vote(&votes,false) && votes==0xf);
 assert(sm8550_apps_smmu_lock());sm8550_apps_smmu_unlock();
 delete gSpiRegistrationLock;delete gcc_lock;delete smmu_lock;
 puts("PASS: shared level IRQ dispatch, exclusive/edge checks, GIC masking, concurrent registration and UART/audio clock vote lifetime");
}
'''
out=root/'drivers/QcaBluetooth/out/shared-test';out.mkdir(parents=True,exist_ok=True);(out/'shared.cpp').write_text(prefix+body+tests)
subprocess.run(['clang++','-std=c++17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(soc),str(out/'shared.cpp'),'-o',str(out/'shared')],check=True)
r=subprocess.run([out/'shared'],text=True,capture_output=True);(out/'output.txt').write_text(r.stdout+r.stderr)
if r.returncode:raise SystemExit(r.stdout+r.stderr)
(out/'report.json').write_text(json.dumps(dict(passed=True,registers_simulated=True,hardware_verified=False,
    inputs={str(cpu):hashlib.sha256((a+b).encode()).hexdigest(),str(soc/'Sm8550Gcc.c'):hashlib.sha256(gcc.encode()).hexdigest()},output=r.stdout),indent=2)+'\n')
print(r.stdout,end='')
