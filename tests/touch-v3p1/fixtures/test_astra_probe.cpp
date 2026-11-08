// SPDX-License-Identifier: BSD-2-Clause-Patent
// Actual Probe helper extraction, platform stubs, actual Core/Engine/Timeline.
#include "TouchEngine.h"
#include "TouchAstraProfile.h"
#include "TouchReportTimeline.h"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
volatile uint32_t native_touch_seq,native_touch_count,native_touch_hid_reports,native_touch_ack_seq;
volatile uint64_t native_touch_time_ticks;
volatile int native_touch_xs[NVT_SLOTS],native_touch_ys[NVT_SLOTS],native_touch_ids[NVT_SLOTS];
volatile int32_t native_touch_extent[4],native_touch_strokes[16][7];
volatile uint32_t native_touch_stroke_count,native_touch_aggressive_stats[8],native_touch_astra_stats[8];
volatile uint32_t native_touch_stream_stats[5],native_touch_contacts;
static void (*gTouchSink)(void *);
static void *gTouchSinkContext;
volatile bool gScriptedDown;
static void host_barrier() {std::atomic_thread_fence(std::memory_order_seq_cst);}
static void absolutetime_to_nanoseconds(uint64_t ticks,uint64_t *ns) {*ns=ticks*1000;} // host tick=1us
struct touch_trace_row {uint32_t counter;uint64_t report_us;int count;uint16_t peer_distance;uint8_t miss_reason;};
struct {bool lock;} pathTrace;
static unsigned pathRecords;
static void recordPaths(const nvt_touch_core &,touch_trace_row) {++pathRecords;}
#include "probe_extraction.inc"
struct Event {uint64_t ticks;std::vector<nvt_contact> points;};
struct Sink {
    unsigned failures=0;
    std::vector<Event> events;
    static void submit(void *context) {
        auto &s=*static_cast<Sink *>(context);
        assert(!(native_touch_seq&1));
        if(s.failures) {--s.failures;return;}
        Event e={native_touch_time_ticks,{}};
        for(unsigned i=0;i<native_touch_count && i<NVT_SLOTS;++i) {
            nvt_contact p={};p.tracking_id=native_touch_ids[i];p.x=native_touch_xs[i];p.y=native_touch_ys[i];
            e.points.push_back(p);
        }
        s.events.push_back(e);
        native_touch_ack_seq=native_touch_seq;
    }
};
static nvt_touch_core core;
static nvt_partial_state partial;
static TouchReportTimeline timeline;
static void init(Sink &s) {
    nvt_touch_reset(&core);nvt_partial_reset(&partial);timeline.reset();
    const auto profile=touchAstraReportExperimentProfile();
    assert(nvt_partial_configure(&partial,&profile));
    native_touch_seq=native_touch_count=native_touch_ack_seq=0;native_touch_time_ticks=0;
    native_touch_stroke_count=0;strokeCount=0;
    for(unsigned i=0;i<4;++i)native_touch_extent[i]=(i%2)?-1:40000;
    for(unsigned i=0;i<8;++i)native_touch_aggressive_stats[i]=native_touch_astra_stats[i]=0;
    for(unsigned i=0;i<5;++i)native_touch_stream_stats[i]=0;
    timelineResetPending=false;gScriptedDown=false;pathTrace.lock=false;pathRecords=0;
    gTouchSink=Sink::submit;gTouchSinkContext=&s;
}
static void seed(int age,int x=100,int y=200,uint64_t born=1000) {
    nvt_slot &p=core.slots[0];p={};p.active=1;p.age=age;p.tracking_id=1;p.generation=1;
    p.x=p.rx=p.origin_x=x;p.y=p.ry=p.origin_y=y;
    p.born_us=p.last_observed_us=p.last_filter_us=born;
}
static void pass(const char *s) {std::printf("PASS %s\n",s);}
int main() {
    unsigned tests=0;
    {
        Sink s;init(s);nvt_contact p={};p.tracking_id=3;p.x=123;p.y=200;
        s.failures=1;assert(!publishAt(1,&p,100000));
        assert(strokeCount==0 && native_touch_stroke_count==0 && native_touch_time_ticks==100000);
        assert(publishAt(1,&p,100000));assert(strokeCount==1 && strokes[0].frames==1);
        assert(s.events.back().ticks==100000 && s.events.back().points[0].y==20119);
        assert(publishAt(0,nullptr,120000));assert(native_touch_stroke_count==1);
        assert(native_touch_strokes[0][0]==100 && native_touch_strokes[0][1]==20 && native_touch_strokes[0][6]==1);
        pass("actual_publish_ACK_gates_originaltime_stroke_stats");++tests;
    }
    {
        Sink s;init(s);TouchEngine engine(&core,&partial);seed(1);
        assert(captureTimeline(engine,timeline,1000));assert(s.events.empty() && timeline.used==1);
        core.slots[0].age=2;core.slots[0].rx=300;core.slots[0].ry=400;
        assert(captureTimeline(engine,timeline,9000));assert(s.events.size()==2 && timeline.used==0);
        assert(s.events[0].ticks==1000 && s.events[1].ticks==9000);
        assert(s.events[0].points[0].x==100 && s.events[1].points[0].x==300);
        assert(s.events[0].points[0].y==20119 && s.events[1].points[0].y==19919);
        assert(strokes[0].frames==2 && strokes[0].t0==1);
        pass("actual_capture_shortswipe_two_distinct_receive_times");++tests;
    }
    {
        Sink s;init(s);TouchEngine engine(&core,&partial);seed(1);
        captureTimeline(engine,timeline,1000);
        assert(inputWaitMs(engine,timeline,1000)==25);
        expireTouchStream(engine,timeline,25999,7);assert(s.events.empty() && core.slots[0].active);
        expireTouchStream(engine,timeline,26000,7);
        assert(!core.slots[0].active && core.pending_expired==1 && timeline.used==0);
        assert(s.events.size()==1 && s.events.back().points.empty());
        assert(inputWaitMs(engine,timeline,26000)==0);
        pass("actual_expiry_no_new_M_resolves_pending25ms");++tests;
    }
    {
        Sink s;init(s);TouchEngine engine(&core,&partial);seed(2);captureTimeline(engine,timeline,1000);
        expireTouchStream(engine,timeline,100999,8);assert(core.slots[0].active && s.events.size()==1);
        expireTouchStream(engine,timeline,101000,8);
        assert(!core.slots[0].active && core.observation_cancelled==1);
        assert(s.events.size()==2 && s.events.back().points.empty() && s.events.back().ticks==101000);
        assert(native_touch_contacts==0 && native_touch_stream_stats[0]==1);
        pass("actual_expiry_missing_position_captures_complete_UP100ms");++tests;
    }
    {
        Sink s;init(s);TouchEngine engine(&core,&partial);seed(2);s.failures=1;
        captureTimeline(engine,timeline,1000);assert(timeline.used==1 && s.events.empty() && strokeCount==0);
        assert(inputWaitMs(engine,timeline,1000)==1);
        expireTouchStream(engine,timeline,1500,9);
        assert(timeline.used==0 && s.events.size()==1 && s.events.back().ticks==1000);
        assert(strokes[0].frames==1 && strokes[0].t0==1);
        pass("actual_timer_retries_failed_ACK_with_original_tick");++tests;
    }
    {
        Sink s;init(s);TouchEngine engine(&core,&partial);seed(1);
        for(unsigned k=0;k<TouchReportTimeline::Capacity;++k)captureTimeline(engine,timeline,1000+k*10);
        assert(timeline.used==TouchReportTimeline::Capacity && s.events.empty());
        assert(!captureTimeline(engine,timeline,1400));
        assert(native_touch_astra_stats[4]==1 && timeline.used==0 && !timelineResetPending);
        assert(!core.slots[0].active && s.events.size()==1 && s.events[0].points.empty());
        pass("actual_overflow_ACKed_empty_precedes_Core_queue_reset");++tests;
    }
    {
        Sink s;init(s);TouchEngine engine(&core,&partial);seed(1);
        for(unsigned k=0;k<TouchReportTimeline::Capacity;++k)captureTimeline(engine,timeline,1000+k*10);
        s.failures=1;captureTimeline(engine,timeline,1400);
        assert(timelineResetPending && core.slots[0].active && timeline.used==TouchReportTimeline::Capacity);
        assert(inputWaitMs(engine,timeline,1400)==1);
        expireTouchStream(engine,timeline,2000,10);
        assert(!timelineResetPending && timeline.used==0 && !core.slots[0].active);
        assert(s.events.size()==1 && s.events[0].points.empty());
        pass("actual_overflow_failed_release_waits_for_ACK_before_reset");++tests;
    }
    {
        Sink s;init(s);nvt_contact p[3]={};
        p[0].tracking_id=1;p[0].x=123;p[0].y=-7;
        p[1].tracking_id=2;p[1].x=456;p[1].y=20319;
        p[2].tracking_id=3;p[2].x=789;p[2].y=30000;
        assert(publishAt(3,p,1000));
        assert(s.events[0].points[0].x==123 && s.events[0].points[0].y==-7);
        assert(s.events[0].points[1].y==0 && s.events[0].points[2].y==0);
        pass("actual_publish_preserves_panel_axis_mapping_and_clamp");++tests;
    }
    std::printf("{\"tests\":%u,\"status\":\"PASS_ACTUAL_PROBE_HELPERS_PLATFORM_STUB_NOT_HARDWARE\"}\n",tests);
}
