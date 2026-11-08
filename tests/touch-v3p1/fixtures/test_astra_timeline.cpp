// Define ASTRA_TIME_API_COMPILE_PROBE for syntax-only compilation against the
// locked target IOHIDFamily source headers on tomorrow's macOS build runner.
#if defined(ASTRA_TIME_API_COMPILE_PROBE)
#include <IOKit/hid/IOHIDDevice.h>
#include <stdint.h>
class AstraTimestampApiProbe : public IOHIDDevice {
    IOReturn check(IOMemoryDescriptor *report,uint64_t ticks) {
        return handleReportWithTime(static_cast<AbsoluteTime>(ticks),report,kIOHIDReportTypeInput,0);
    }
};
#else
// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "TouchReportTimeline.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <type_traits>
struct Event {uint64_t ticks;std::vector<nvt_contact> contacts;};
struct Sink {
    std::vector<Event> events;
    unsigned failures=0;
    static bool submit(void *context,unsigned n,const nvt_contact *p,uint64_t ticks) {
        Sink &self=*static_cast<Sink *>(context);
        if(self.failures) {--self.failures;return false;}
        self.events.push_back({ticks,std::vector<nvt_contact>(p,p+n)});return true;
    }
};
static nvt_touch_core core;
static TouchReportTimeline timeline;
static void init() {memset(&core,0,sizeof(core));timeline.reset();}
static void slot(unsigned s,int id,uint64_t gen,int age,int x,int y,uint64_t born=0) {
    nvt_slot &p=core.slots[s];memset(&p,0,sizeof(p));
    p.active=1;p.tracking_id=id;p.generation=gen;p.age=age;p.origin_x=x;p.origin_y=y;p.x=x;p.y=y;p.rx=x;p.ry=y;p.born_us=born;
}
static void feed(uint64_t us) {assert(timeline.feed(us,us*24+7,core));}
static void flush(uint64_t us,Sink &s) {timeline.flush(us,Sink::submit,&s);}
static const nvt_contact *find(const Event &e,int id) {
    for(const auto &p:e.contacts)if(p.tracking_id==id)return &p;
    return nullptr;
}
static void pass(const char *name) {std::printf("PASS %s\n",name);}
int main() {
    static_assert(std::is_trivial<TouchReportTimeline>::value,"IOMalloc+reset requires trivial storage");
    static_assert(std::is_standard_layout<TouchReportTimeline>::value,"Fixed storage ABI");
    unsigned tests=0;
    init(); {
        Sink s;slot(0,1,1,1,100,200);feed(1000);flush(1000,s);assert(s.events.empty());
        core.slots[0].age=2;core.slots[0].rx=300;core.slots[0].ry=400;feed(9000);flush(9000,s);
        assert(s.events.size()==2);assert(find(s.events[0],1)->x==100);assert(find(s.events[1],1)->x==300);
        assert(s.events[0].ticks==24007 && s.events[1].ticks==216007);
        core.slots[0].active=0;feed(17000);flush(17000,s);assert(s.events.size()==3 && s.events.back().contacts.empty());
        pass("two_M_short_swipe_origin_latest_UP_original_time");++tests;
    }
    init(); {
        Sink s;slot(0,1,11,1,100,100);slot(1,2,22,2,20,30);feed(1000);
        core.slots[1].rx=40;feed(4000);
        core.slots[0].age=2;core.slots[0].rx=200;core.slots[1].rx=80;feed(8000);flush(8000,s);
        assert(s.events.size()==3);
        assert(find(s.events[0],2)->x==20 && find(s.events[1],2)->x==40 && find(s.events[2],2)->x==80);
        assert(find(s.events[0],1)->x==100 && find(s.events[2],1)->x==200);
        pass("multifinger_full_historical_snapshots_no_future_coordinate");++tests;
    }
    init(); {
        Sink s;slot(0,1,1,1,100,100);slot(1,2,2,2,20,30);feed(1000);
        core.slots[1].rx=40;feed(4000);
        core.slots[0].active=0;core.slots[1].rx=80;feed(8000);flush(8000,s);
        assert(s.events.size()==3);
        for(const auto &e:s.events)assert(!find(e,1));
        assert(find(s.events[0],2)->x==20 && find(s.events[1],2)->x==40 && find(s.events[2],2)->x==80);
        assert(timeline.pendingDropped==1);
        pass("failed_pending_removed_from_all_unsent_history");++tests;
    }
    init(); {
        Sink s;slot(0,1,1,1,10,10);feed(0);
        assert(timeline.waitUs(0)==25000);flush(24999,s);assert(s.events.empty());
        flush(25000,s);assert(s.events.size()==1 && s.events[0].contacts.empty());
        assert(timeline.waitUs(25000)==0 && timeline.pendingDropped==1);
        pass("timer_without_new_M_settles_at_25ms");++tests;
    }
    init(); {
        Sink s;slot(0,1,1,1,10,10);feed(0);
        slot(1,2,2,1,20,20,20000);feed(20000);flush(25000,s);
        assert(s.events.size()==1 && s.events[0].contacts.empty());
        assert(timeline.used==1 && timeline.waitUs(25000)==20000);
        flush(45000,s);assert(s.events.size()==2 && s.events.back().contacts.empty());
        pass("later_candidate_cannot_extend_older_deadline");++tests;
    }
    init(); {
        Sink s;slot(0,1,1,1,10,10);feed(0);flush(25000,s);
        core.slots[0].age=2;core.slots[0].rx=200;feed(26000);flush(26000,s);
        assert(s.events.back().contacts.empty());
        slot(0,1,2,2,300,300,27000);feed(27000);flush(27000,s);
        assert(find(s.events.back(),1)->x==300);
        pass("expired_pending_generation_cannot_resurrect");++tests;
    }
    init(); {
        Sink s;slot(0,1,1,2,10,10);feed(1000);s.failures=1;flush(1000,s);
        assert(timeline.used==1 && timeline.submitted==0 && timeline.emittedCount==0);
        flush(1000,s);assert(timeline.used==0 && s.events.size()==1 && timeline.submitted==1);
        pass("submission_failure_retains_original_frame_and_time");++tests;
    }
    init(); {
        Sink s;slot(0,1,10,2,10,10);feed(1000);flush(1000,s);
        slot(0,1,20,2,200,200,2000);feed(2000);flush(2000,s);
        assert(s.events.size()==3 && s.events[1].contacts.empty());
        assert(find(s.events[2],1)->x==200);
        assert(s.events[1].ticks==48007 && s.events[2].ticks==48007);
        pass("same_HID_ID_new_generation_gets_explicit_UP_boundary");++tests;
    }
    init(); {
        Sink s;slot(0,1,10,2,10,10);slot(1,2,30,2,50,50);feed(1000);flush(1000,s);
        slot(0,1,20,2,200,200,2000);core.slots[1].rx=80;feed(2000);flush(2000,s);
        assert(s.events.size()==3 && !find(s.events[1],1));
        assert(find(s.events[1],2)->x==50 && find(s.events[2],2)->x==80);
        pass("ID_reuse_boundary_preserves_other_fingers");++tests;
    }
    init(); {
        Sink s;slot(0,1,1,1,10,10);for(unsigned k=0;k<TouchReportTimeline::Capacity;++k)feed(k*100);
        assert(!timeline.feed(3300,79207,core));assert(timeline.used==TouchReportTimeline::Capacity && timeline.capacityFaults==1);
        flush(25000,s);assert(timeline.used==0 && s.events.size()==TouchReportTimeline::Capacity);
        for(const auto &e:s.events)assert(e.contacts.empty());
        pass("capacity_fault_preserves_existing_history_no_overwrite");++tests;
    }
    init(); {
        Sink s;slot(0,1,1,2,10,10);feed(1000);flush(1000,s);
        assert(!timeline.feed(999,24007,core));assert(timeline.clockFaults==1 && timeline.used==0);
        assert(!timeline.feed(1001,24006,core));assert(timeline.clockFaults==2 && timeline.used==0);
        pass("timestamp_rollback_explicit_fault_no_reordering");++tests;
    }
    init(); {
        Sink s;slot(0,1,1,2,10,10,1000);feed(1000);flush(1000,s);
        assert(s.events.size()==1 && find(s.events[0],1)->x==10);
        assert(timeline.waitUs(1000)==0);
        pass("high_confidence_immediate_entry_no_pending_wait");++tests;
    }
    init(); {
        Sink s;slot(0,1,1,1,10,10);feed(1000);core.slots[0].age=2;
        core.slots[0].origin_x=999;core.slots[0].rx=20;feed(9000);flush(9000,s);
        assert(find(s.events[0],1)->x==10 && find(s.events[1],1)->x==20);
        pass("historical_origin_not_reconstructed_from_latest_slot");++tests;
    }
    init(); {
        Sink s;slot(0,1,1,1,10,10);feed(1000);core.slots[0].age=2;feed(9000);
        s.failures=1;flush(9000,s);assert(timeline.used==2);
        flush(9000,s);assert(s.events.size()==2 && s.events[0].ticks==24007 && s.events[1].ticks==216007);
        pass("backpressure_keeps_whole_timeline_order");++tests;
    }
    init(); {
        Sink s;slot(0,1,1,1,10,10);feed(1000);timeline.reset();
        assert(timeline.used==0 && timeline.emittedCount==0 && timeline.waitUs(1000)==0);
        pass("explicit_reset_clears_all_generation_and_clock_state");++tests;
    }
    std::printf("{\"tests\":%u,\"status\":\"PASS_HOST_TIMELINE_NOT_HARDWARE\",\"bytes\":%zu}\n",tests,sizeof(TouchReportTimeline));
}

#endif
