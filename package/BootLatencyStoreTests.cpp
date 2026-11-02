// SPDX-License-Identifier: MIT
#include "BootLatencyStore.h"
#include <cassert>
#include <thread>
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
 static_assert(sizeof(bl_record)==72,"latency ABI");
}
