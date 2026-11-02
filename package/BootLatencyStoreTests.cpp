// SPDX-License-Identifier: MIT
#include "BootLatencyStore.h"
#include "BootLatencyFiles.h"
#include <cassert>
#include <thread>
#include <string>
#include <vector>
int main() {
 bl_slot slots[8]{};bl_store s{slots,8,0,0};bl_record r{};
 for(unsigned i=0;i<32;++i){r.value[0]=i;assert(bl_push(&s,&r));}
 assert(!bl_read(&s,24,&r));assert(bl_read(&s,25,&r)&&r.value[0]==24);
 slots[0].busy=1;auto old=s.dropped;assert(!bl_push(&s,&r));assert(s.dropped==old+1);slots[0].busy=0;
 std::vector<std::thread> threads;
 for(unsigned t=0;t<4;++t)threads.emplace_back([&]{for(unsigned i=0;i<20000;++i){bl_record v{};v.value[0]=i;v.value[1]=~uint64_t(i);bl_push(&s,&v);}});
 for(auto &t:threads)t.join();
 for(uint64_t n=s.issued-7;n<=s.issued;++n)if(bl_read(&s,n,&r))assert(r.value[1]==~r.value[0]);
 bl_slot normal_slots[8]{},critical_slots[4]{};bl_store normal{normal_slots,8,0,0},saved{critical_slots,4,0,0};
 bl_record fault{};fault.kind=3;fault.value[0]=0xfeed;
 assert(bl_push(bl_critical_kind(fault.kind)?&saved:&normal,&fault));
 for(unsigned i=0;i<100000;++i){bl_record v{};v.kind=5;bl_push(bl_critical_kind(v.kind)?&saved:&normal,&v);}
 assert(saved.issued==1 && bl_read(&saved,1,&fault) && fault.value[0]==0xfeed);
 bl_record unnest{};unnest.kind=35;assert(bl_critical_kind(unnest.kind) && bl_critical_kind(36) && !bl_critical_kind(1) && !bl_critical_kind(5));
 assert(bl_latency_file("/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/frames.trace"));
 assert(bl_latency_file("/private/var/mobile/Library/Logs/CrashReporter/STExtractionService-now.ips"));
 assert(bl_latency_file("/private/var/mobile/Library/Logs/CrashReporter/CalendarWidgetExtension-now.ips"));
 assert(!bl_latency_file("/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/prime-fn"));
 assert(!bl_latency_file("/private/var/db/diagnostics/all.tracev3"));
 assert(!bl_latency_file("/private/var/mobile/Library/Logs/CrashReporter/Safari-now.ips"));
 char text[]="[wifi] unrelated\nprocess dasd: EXC_BAD_ACCESS\n[audio] unrelated\nmemorystatus: killed\n";
 unsigned n=bl_latency_messages(text,sizeof text-1);
 assert(std::string(text,n)=="process dasd: EXC_BAD_ACCESS\nmemorystatus: killed\n");
 static_assert(sizeof(bl_record)==72,"latency ABI");
}
