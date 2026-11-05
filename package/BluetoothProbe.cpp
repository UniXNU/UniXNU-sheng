/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <stdarg.h>
#include "qbt.h"
#include "qbt_geni.h"
#include "qbt_transport.h"
#include "Sm8550GeniUart.h"
#include "Sm8550QupDma.h"
#include "WcnPower.h"
#include "BluetoothFirmware.h"

extern "C" int armcpu_spi_attach(uint32_t,bool,void(*)(void *),void *,uint32_t *,uint64_t *);
extern "C" int armcpu_spi_share(uint32_t,void(*)(void *),void *);
extern "C" int armcpu_spi_mask(uint32_t,bool);
extern "C" volatile int native_gpu_addon_stage;
extern "C" {volatile uint64_t native_bluetooth_stats[32];}
namespace {
struct Region {IOMemoryMap *map;volatile uint32_t *va;};
struct Bluetooth {
    Region gcc,uart,wrapper,tlmm,smmu;
    IOService *radio;
    IOBufferMemoryDescriptor *dma,*tables;
    struct sm8550_qup_dma domain;
    struct qbt_geni geni;
    struct qbt *core;
    uint64_t rx_sequence;
    uint32_t pending,rx_wake,power_request,started;
    unsigned log,baud;
    uint8_t address[6];
    bool resources,powered,gated,clocked,serial_irq,wake_irq,service;
} bt;
constexpr unsigned kRx=79,kBtEnable=81;
constexpr uint32_t kDmaBytes=32768,kTableBytes=16384;
bool map(Region &r,uint64_t base,size_t bytes) {
    if(r.va)return true;
    IODeviceMemory *memory=IODeviceMemory::withRange(base,bytes);
    r.map=memory?memory->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
    if(memory)memory->release();r.va=r.map?reinterpret_cast<volatile uint32_t *>(r.map->getVirtualAddress()):nullptr;
    return r.va!=nullptr;
}
void barrier(){__asm__ volatile("dsb sy" ::: "memory");}
uint64_t nowNs(void *){uint64_t ns;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns;}
void wake(void *){__atomic_store_n(&bt.pending,1u,__ATOMIC_RELEASE);thread_wakeup((event_t)&bt.pending);}
void waitUntil(void *,uint64_t deadline) {
    if(__atomic_exchange_n(&bt.pending,0u,__ATOMIC_ACQ_REL))return;
    if(deadline==UINT64_MAX)assert_wait((event_t)&bt.pending,THREAD_UNINT);
    else {uint64_t absolute;nanoseconds_to_absolutetime(deadline,&absolute);assert_wait_deadline((event_t)&bt.pending,THREAD_UNINT,absolute);}
    if(__atomic_exchange_n(&bt.pending,0u,__ATOMIC_ACQ_REL))clear_wait(current_thread(),THREAD_AWAKENED);
    else thread_block(THREAD_CONTINUE_NULL);
}
uint32_t urd(void *,uint32_t reg){return bt.uart.va[reg/4];}
void uwr(void *,uint32_t reg,uint32_t value){bt.uart.va[reg/4]=value;barrier();}
uint32_t grd(void *,uint32_t reg){return bt.gcc.va[reg/4];}
void gwr(void *,uint32_t reg,uint32_t value){bt.gcc.va[reg/4]=value;barrier();}
void delay(void *,unsigned us){IODelay(us);}
void dmaBarrier(void *){barrier();}
void dmaSync(void *,const struct qbt_dma_buffer *,size_t,bool,bool){barrier();} // QUP2 is dma-coherent.
void publish(void *,void *data,size_t n) {
    uintptr_t at=reinterpret_cast<uintptr_t>(data)&~63ULL,end=reinterpret_cast<uintptr_t>(data)+n;
    for(;at<end;at+=64)__asm__ volatile("dc cvac, %0"::"r"(at):"memory");barrier();
}
uint32_t srd(void *,uint32_t reg){return bt.smmu.va[reg/4];}
void swr(void *,uint32_t reg,uint32_t value){bt.smmu.va[reg/4]=value;barrier();}
void irqMask(void *,bool masked) {
    if(bt.serial_irq)(void)armcpu_spi_mask(SM8550_UART14_INTID,masked);
}
void serialIrq(void *){qbt_geni_irq(&bt.geni);}
void pin(unsigned gpio,uint32_t cfg,int output=-1) {
    if(output>=0)bt.tlmm.va[(gpio*4096+4)/4]=output?2:0;
    bt.tlmm.va[gpio*4096/4]=cfg;barrier();
}
void rxIrq(void *) {
    volatile uint32_t *r=bt.tlmm.va+kRx*4096/4;
    if(!(r[3]&1))return;
    r[2]&=~1u;r[3]=0;barrier();
    __atomic_store_n(&bt.rx_wake,1u,__ATOMIC_RELEASE);wake(nullptr);
}
void disarmRxWake(){bt.tlmm.va[(kRx*4096+8)/4]&=~1u;bt.tlmm.va[(kRx*4096+12)/4]=0;pin(kRx,3u|(1u<<2));}
bool serialClock(bool enabled) {
    if(!sm8550_geni_uart14_vote(bt.gcc.va+0x52010/4,enabled))return false;
    if(!enabled){bt.clocked=false;return true;}
    for(unsigned i=0;i<1000;i++){if(!(grd(nullptr,0x1e754)&(1u<<31))){bt.clocked=true;return true;}IODelay(1);}
    return false;
}
size_t rxBytes(void *,const uint8_t *bytes,size_t n) {
    bt.rx_sequence+=n;return qbt_receive(bt.core,bytes,n);
}
size_t nativeRx(void *,const uint8_t *bytes,size_t n){return qbt_transport_receive(bytes,(uint32_t)n);}
size_t writeUart(void *,const uint8_t *p,size_t n){return qbt_geni_write(&bt.geni,p,n);}
bool drained(void *){return bt.gated || qbt_geni_drained(&bt.geni);}
int flow(void *,bool enabled){qbt_geni_set_flow(&bt.geni,enabled);return 0;}
void pump(void *) {
    if(__atomic_exchange_n(&bt.rx_wake,0u,__ATOMIC_ACQ_REL) && bt.powered) {
        qbt_rx_wake(bt.core);
        /* The GPIO consumed the first IBS wake byte while the UART was gated. */
        const uint8_t wakeByte=0xfd;(void)qbt_receive(bt.core,&wakeByte,1);
    }
    if(!bt.gated) {
        if(qbt_geni_service(&bt.geni))wake(nullptr);
        if(bt.geni.state==QBT_GENI_FAILED)qbt_fail(bt.core,bt.geni.error);
    }
}
int quiesce() {
    uint64_t end=nowNs(nullptr)+40000000ULL;
    for(;;) {
        int e=qbt_geni_quiesce(&bt.geni);if(!e)return 0;
        if(e!=-EINPROGRESS && e!=-EBUSY)return e;
        pump(nullptr);if(bt.geni.state==QBT_GENI_STOPPED)return 0;
        if(nowNs(nullptr)>=end)return -ETIMEDOUT;
        uint64_t next=qbt_geni_deadline(&bt.geni);if(next>end)next=end;waitUntil(nullptr,next);
    }
}
int clockVote(void *,bool enabled) {
    if(enabled) {
        if(!bt.gated)return 0;
        if(!serialClock(true))return -EIO;
        disarmRxWake();IODelay(10);bt.gated=false;
        return qbt_geni_resume(&bt.geni);
    }
    if(bt.gated)return 0;
    uint64_t sequence=bt.rx_sequence;
    int e=quiesce();if(e)return e;
    if(sequence!=bt.rx_sequence || qbt_transport_tx_pending()) {qbt_geni_resume(&bt.geni);return -EBUSY;}
    volatile uint32_t *r=bt.tlmm.va+kRx*4096/4;
    r[2]=0;r[3]=0;pin(kRx,3u);
    r[2]=(3u<<5)|(2u<<2)|(1u<<4)|1u;barrier();
    if(!(r[1]&1) || (r[3]&1)) {disarmRxWake();qbt_geni_resume(&bt.geni);return -EBUSY;}
    /* Leave RTS ready so the controller can send its IBS wake indication. */
    uwr(nullptr,0x2ac,0x80000001u);
    bt.gated=true;
    if(!serialClock(false)){bt.gated=false;disarmRxWake();qbt_geni_resume(&bt.geni);return -EIO;}
    return 0;
}
int setRate(void *,uint32_t baud) {
    int e=quiesce();if(e)return e;
    uint32_t hz=0;struct sm8550_geni_clock_io io={nullptr,grd,gwr,delay};
    e=sm8550_geni_uart14_source(&io,baud,&hz);
    if(!e)e=qbt_geni_set_rate(&bt.geni,baud,hz,16);
    if(!e)e=qbt_geni_resume(&bt.geni);
    if(!e)bt.baud=baud;return e;
}
void *allocate(void *,size_t n){return IOMallocData(n);}
void release(void *,void *p,size_t n){IOFreeData(p,n);}
int firmware(void *,const char *name,const uint8_t **data,size_t *size) {
    uint8_t *bytes=nullptr;int e=sheng_bt_firmware_read(name,&bytes,size);*data=bytes;return -e;
}
void firmwarePut(void *,const uint8_t *data,size_t size){sheng_bt_firmware_release(const_cast<uint8_t *>(data),size);}
void log(void *,int level,const char *line){if(level<=3 || bt.log)printf("[bluetooth] %s\n",line);}
bool address() {
    IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
    OSData *id=chosen?OSDynamicCast(OSData,chosen->getProperty("unique-chip-id")):nullptr;
    if(!id || id->getLength()<8){if(chosen)chosen->release();return false;}
    uint64_t hash=0xcbf29ce484222325ULL;
    static const char domain[]="unixnu-bluetooth";
    for(unsigned i=0;i<sizeof(domain)-1;i++)hash=(hash^(uint8_t)domain[i])*0x100000001b3ULL;
    const uint8_t *seed=static_cast<const uint8_t *>(id->getBytesNoCopy());
    bool nonzero=false;
    for(unsigned i=0;i<id->getLength();i++){nonzero|=seed[i]!=0;hash=(hash^seed[i])*0x100000001b3ULL;}
    bool published=false;
    if(nonzero) {
        for(unsigned i=0;i<6;i++)bt.address[i]=(uint8_t)(hash>>(i*8));
        bt.address[0]=(bt.address[0]&0xfe)|2;
        IORegistryEntry *node=IORegistryEntry::fromPath("/arm-io/bluetooth",gIODTPlane);
        OSData *encoding=node?OSDynamicCast(OSData,node->getProperty("transport-encoding")):nullptr;
        OSData *speed=node?OSDynamicCast(OSData,node->getProperty("transport-speed")):nullptr;
        uint32_t wire=0,baud=0;
        if(encoding && encoding->getLength()==4)memcpy(&wire,encoding->getBytesNoCopy(),4);
        if(speed && speed->getLength()==4)memcpy(&baud,speed->getBytesNoCopy(),4);
        if(node && wire==1 && baud==3200000) {
            published=chosen->setProperty("mac-address-bluetooth0",bt.address,6) &&
                node->setProperty("local-mac-address",bt.address,6);
            if(published && !bt.radio) {
                IOService *radio=new IOService;
                if(radio && radio->init()) {
                    radio->setName("bluetooth");
                    published=radio->setProperty("device_type","bluetooth") &&
                        radio->setProperty("transport-encoding",encoding) &&
                        radio->setProperty("transport-speed",speed) &&
                        radio->setProperty("local-mac-address",bt.address,6);
                    IOService *provider=IOService::getPlatform();
                    if(published && provider && radio->attach(provider)) {
                        bt.radio=radio;radio->registerService();
                    } else {published=false;radio->release();}
                } else {published=false;if(radio)radio->release();}
            }
        }
        if(node)node->release();
    }
    chosen->release();return published;

}
int resources() {
    if(bt.resources)return 0;
    if(!map(bt.gcc,0x100000,0x80000) || !map(bt.uart,SM8550_UART14_BASE,0x4000) ||
        !map(bt.wrapper,SM8550_QUP2_BASE,0x2000) || !map(bt.tlmm,0xf100000,0x60000) || !map(bt.smmu,0x15000000,0xd0000))return -ENOMEM;
    if(!address())return -ENOENT;
    const IOOptionBits options=static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous)|static_cast<IOOptionBits>(kIOMemoryMapperNone);
    if(!bt.dma)bt.dma=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,options,kDmaBytes,4096);
    if(!bt.tables)bt.tables=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,options,kTableBytes,4096);
    if(!bt.dma || !bt.tables)return -ENOMEM;
    IOByteCount contiguous=0,tableContiguous=0;
    uint64_t pa=bt.dma->getPhysicalSegment(0,&contiguous,kIOMemoryMapperNone),tpa=bt.tables->getPhysicalSegment(0,&tableContiguous,kIOMemoryMapperNone);
    if(!pa || !tpa || contiguous<kDmaBytes || tableContiguous<kTableBytes)return -EIO;
    if(!sm8550_apps_smmu_lock())return -ENOMEM;
    struct sm8550_qup_dma_io io={nullptr,srd,swr,delay,publish};
    int e=sm8550_qup_dma_attach(&io,&bt.domain,SM8550_QUP2_SID,pa,kDmaBytes,tpa,
        static_cast<uint64_t *>(bt.tables->getBytesNoCopy()),kTableBytes/4096);
    if(e) {
        uint32_t id0=srd(nullptr,0x20),id1=srd(nullptr,0x24);
        unsigned slots=id0&255,banks=id1&255,pages=1u<<(((id1>>28)&7)+1),freeSlots=0,disabled=0,shown=0;
        for(unsigned i=0;i<slots && i<128;i++)if(!(srd(nullptr,0x800+i*4)&0x80000000u))freeSlots++;
        printf("[bluetooth dma] attach=%d id=%x/%x/%x scr0=%x slots=%u free=%u banks=%u pages=%u\n",
            e,id0,id1,srd(nullptr,0x28),srd(nullptr,0),slots,freeSlots,banks,pages);
        if((pages+banks)*4096<=0xd0000)for(unsigned i=1;i<banks && i<128;i++) {
            uint32_t cb=(pages+i)*4096,control=srd(nullptr,cb);
            if(!(control&1)) {
                disabled++;
                if(shown++<8)printf("[bluetooth dma] bank=%u sctlr=%x cbar=%x cba2r=%x fsr=%x\n",
                    i,control,srd(nullptr,0x1000+i*4),srd(nullptr,0x1800+i*4),srd(nullptr,cb+0x58));
            }
        }
        printf("[bluetooth dma] disabled=%u\n",disabled);
    }
    sm8550_apps_smmu_unlock();if(e)return e;
    bt.resources=true;return 0;
}
int stopEngine() {
    irqMask(nullptr,true);uwr(nullptr,0xe1c,0);
    if((urd(nullptr,0x40)&0x1001) || (urd(nullptr,0xe40)&3)) {
        uwr(nullptr,0x604,2);uwr(nullptr,0x634,2);
        unsigned i=0;for(;i<1000 && (urd(nullptr,0x40)&0x1001);i++)IODelay(1);
        if(i==1000)return -ETIMEDOUT;
        uwr(nullptr,0xc58,1);uwr(nullptr,0xd58,1);
        for(i=0;i<1000 && (urd(nullptr,0xe40)&3);i++)IODelay(1);
        if(i==1000)return -ETIMEDOUT;
    }
    uwr(nullptr,0x618,UINT32_MAX);uwr(nullptr,0x648,UINT32_MAX);
    uwr(nullptr,0xc44,UINT32_MAX);uwr(nullptr,0xd44,UINT32_MAX);
    bt.geni.state=QBT_GENI_OFF;bt.geni.rx_busy=bt.geni.tx_busy=false;
    return 0;
}
int powerOn() {
    if(bt.powered)return 0;
    if(bt.domain.poisoned)return -EIO;
    native_bluetooth_stats[14]=1;int e=resources();if(e)return e;
    native_bluetooth_stats[14]=2;
    e=sheng_wcn_power_get(SHENG_WCN_BLUETOOTH);if(e)return -e;
    bt.powered=true;bt.gated=true;
    native_bluetooth_stats[14]=3;
    for(unsigned gpio=76;gpio<=79;gpio++)pin(gpio,(1u<<2)|(gpio>=78?3u:1u));
    pin(kBtEnable,1u|(1u<<9),0);IOSleep(100);
    if(!sm8550_gcc_update32(bt.gcc.va+0x52018/4,0,1u))return -EIO;
    for(unsigned i=0;i<1000 && !(grd(nullptr,0)&(1u<<31));i++)IODelay(1);
    if(!(grd(nullptr,0)&(1u<<31)) || !serialClock(true))return -EIO;
    bt.gated=false;
    native_bluetooth_stats[11]=urd(nullptr,0x68);native_bluetooth_stats[12]=bt.wrapper.va[1];
    native_bluetooth_stats[13]=grd(nullptr,0x52010);native_bluetooth_stats[14]=4;
    if(bt.wrapper.va[1]<0x20050000)return -ENOTSUP;
    if(((urd(nullptr,0x68)>>8)&255)!=2)return -ENXIO;
    e=stopEngine();if(e)return e;
    uint8_t *bytes=static_cast<uint8_t *>(bt.dma->getBytesNoCopy());
    struct qbt_dma_buffer tx={bytes,SM8550_QUP_DMA_IOVA,QBT_GENI_TX_SIZE},rx[QBT_GENI_RX_COUNT];
    for(unsigned i=0;i<QBT_GENI_RX_COUNT;i++)rx[i]={bytes+4096+i*QBT_GENI_RX_SIZE,SM8550_QUP_DMA_IOVA+4096+i*QBT_GENI_RX_SIZE,QBT_GENI_RX_SIZE};
    struct qbt_geni_ops ops={nullptr,urd,uwr,dmaBarrier,dmaSync,nowNs,irqMask,wake,rxBytes};
    e=qbt_geni_prepare(&bt.geni,&ops,&tx,rx);if(e)return e;
    native_bluetooth_stats[14]=5;
    uint32_t hz;struct sm8550_geni_clock_io clocks={nullptr,grd,gwr,delay};
    e=sm8550_geni_uart14_source(&clocks,115200,&hz);if(e)return e;
    e=qbt_geni_set_rate(&bt.geni,115200,hz,16);if(e)return e;
    bt.baud=115200;
    if(!bt.serial_irq){e=armcpu_spi_attach(SM8550_UART14_INTID,false,serialIrq,nullptr,nullptr,nullptr);if(e)return -EIO;bt.serial_irq=true;irqMask(nullptr,true);}
    if(!bt.wake_irq){e=armcpu_spi_share(240,rxIrq,nullptr);if(e)return -EIO;bt.wake_irq=true;}
    e=qbt_geni_start(&bt.geni);if(e)return e;
    native_bluetooth_stats[14]=6;
    disarmRxWake();pin(kBtEnable,1u|(1u<<9),1);IOSleep(60);
    return qbt_boot(bt.core,3200000,bt.address);
}
int powerOff() {
    if(!bt.powered)return 0;
    qbt_set_service(bt.core,false);bt.service=false;
    int e=0;
    if(bt.domain.active) {
        uint32_t cb=(bt.domain.pages+bt.domain.bank)*4096,fsr=srd(nullptr,cb+0x58);
        if(fsr&0xc00001feu) {
            native_bluetooth_stats[10]=fsr;qbt_fail(bt.core,-EIO);
            if(fsr&(1u<<30))swr(nullptr,cb+8,1);
            swr(nullptr,cb+0x58,fsr);e=-EIO;
        }
    }
    if(bt.geni.state!=QBT_GENI_OFF) {
        if(bt.gated){int resumed=clockVote(nullptr,true);if(!e)e=resumed;}
        if(bt.clocked){int shutdown=qbt_shutdown(bt.core);if(!e)e=shutdown;}
    }
    if(bt.clocked && bt.geni.state!=QBT_GENI_OFF){int stopped=quiesce();if(!e)e=stopped;}
    irqMask(nullptr,true);disarmRxWake();pin(kBtEnable,1u|(1u<<9),0);IOSleep(100);
    int retired=bt.clocked?stopEngine():0;if(!e)e=retired;
    if(!retired)bt.gated=serialClock(false);
    bt.powered=false;__atomic_store_n(&bt.rx_wake,0u,__ATOMIC_RELEASE);
    int released=sheng_wcn_power_put(SHENG_WCN_BLUETOOTH);if(!e)e=-released;
    return e;
}
int requestPower(void *,bool enabled) {
    uint32_t expected=0;
    if(!__atomic_compare_exchange_n(&bt.power_request,&expected,enabled?1u:2u,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return EBUSY;
    wake(nullptr);return 0;
}
void powerResult(bool enabled,int error) {
    struct qbt_info info;qbt_get_info(bt.core,&info);
    native_bluetooth_stats[17]=info.product;native_bluetooth_stats[18]=info.soc;
    native_bluetooth_stats[19]=((uint32_t)info.rom<<16)|info.patch;
    if(enabled && !error)native_bluetooth_stats[14]=7;
    qbt_transport_power_result(enabled,error<0?-error:error,info.firmware,info.baud);
    native_bluetooth_stats[20]=(unsigned)bt.powered|((unsigned)bt.gated<<1)|((unsigned)bt.clocked<<2);
    native_bluetooth_stats[21]=sheng_wcn_power_owners();
    native_bluetooth_stats[0]=error?3:enabled?2:0;native_bluetooth_stats[1]=(uint64_t)(int64_t)error;
    printf("[bluetooth] power=%u result=%d baud=%u\n",enabled,error,info.baud);
}
void worker(void *,wait_result_t) {
    thread_set_thread_name(current_thread(),"sheng-bluetooth");
    struct qbt_platform ops={nullptr,allocate,release,nowNs,writeUart,drained,setRate,flow,clockVote,pump,waitUntil,nativeRx,firmware,firmwarePut,log};
    bt.core=qbt_create(&ops);if(!bt.core){native_bluetooth_stats[0]=3;return;}
    struct qbt_transport_ops transport={nullptr,wake,requestPower};
    int e=qbt_transport_init(&transport);if(e){native_bluetooth_stats[0]=3;return;}
    while(native_gpu_addon_stage<1)waitUntil(nullptr,nowNs(nullptr)+10000000ULL);
    e=powerOn();if(e)(void)powerOff();powerResult(true,e);
    uint64_t health=nowNs(nullptr)+1000000000ULL,closeAt=0;
    for(;;) {
        uint32_t command=__atomic_exchange_n(&bt.power_request,0u,__ATOMIC_ACQ_REL);
        if(command){e=command==1?powerOn():powerOff();if(e && command==1)(void)powerOff();powerResult(command==1,e);}
        if(bt.powered) {
            pump(nullptr);bool active=qbt_transport_active();
            if(active!=bt.service){qbt_set_service(bt.core,active);bt.service=active;closeAt=active?0:nowNs(nullptr)+500000000ULL;}
            uint8_t tx[2048];size_t room=qbt_transmit_capacity(bt.core);
            if(room>sizeof(tx))room=sizeof(tx);
            if(room){uint32_t n=qbt_transport_tx_take(tx,(uint32_t)room);if(n && qbt_transmit(bt.core,tx,n))e=-EIO;}
            qbt_step(bt.core);
            struct qbt_info info;qbt_get_info(bt.core,&info);
            if(info.state==3 || bt.geni.state==QBT_GENI_FAILED || (closeAt && nowNs(nullptr)>=closeAt)) {
                int error=info.state==3?info.error:bt.geni.error;
                (void)powerOff();powerResult(false,error);closeAt=0;
            }
        }
        uint64_t t=nowNs(nullptr);
        if(t>=health) {
            struct qbt_stats stats;qbt_get_stats(bt.core,&stats);
            native_bluetooth_stats[2]=bt.baud;native_bluetooth_stats[3]=stats.tx_bytes;native_bluetooth_stats[4]=stats.rx_bytes;
            native_bluetooth_stats[5]=bt.geni.stats.interrupts;native_bluetooth_stats[6]=stats.timeouts;native_bluetooth_stats[7]=stats.clock_sleeps;
            native_bluetooth_stats[8]=bt.domain.slot;native_bluetooth_stats[9]=bt.domain.bank;
            native_bluetooth_stats[20]=(unsigned)bt.powered|((unsigned)bt.gated<<1)|((unsigned)bt.clocked<<2);
            native_bluetooth_stats[21]=sheng_wcn_power_owners();
            if(bt.domain.active){uint32_t fsr=srd(nullptr,(bt.domain.pages+bt.domain.bank)*4096+0x58);native_bluetooth_stats[10]=fsr;
                if(fsr&0xc00001feu){qbt_fail(bt.core,-EIO);(void)powerOff();powerResult(false,-EIO);}}
            health=t+1000000000ULL;
        }
        uint64_t deadline=bt.powered?qbt_deadline(bt.core):UINT64_MAX;
        uint64_t geni=bt.powered && !bt.gated?qbt_geni_deadline(&bt.geni):UINT64_MAX;
        if(geni<deadline)deadline=geni;if(health<deadline)deadline=health;if(closeAt && closeAt<deadline)deadline=closeAt;
        if(!deadline){thread_yield_to_preemption();continue;}
        waitUntil(nullptr,deadline);
    }
}
}
extern "C" void sheng_bluetooth_probe(void) {
    unsigned enabled=0;
    if(!PE_parse_boot_argn("sheng-bluetooth",&enabled,sizeof(enabled)) || !enabled)return;
    uint32_t expected=0;if(!__atomic_compare_exchange_n(&bt.started,&expected,1u,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return;
    native_bluetooth_stats[0]=1;
    if(!address()){native_bluetooth_stats[0]=3;native_bluetooth_stats[1]=(uint64_t)(int64_t)-ENOENT;return;}
    PE_parse_boot_argn("log-bluetooth",&bt.log,sizeof(bt.log));
    thread_t thread=nullptr;
    if(kernel_thread_start(worker,nullptr,&thread)==KERN_SUCCESS)thread_deallocate(thread);
    else native_bluetooth_stats[0]=3;
}
