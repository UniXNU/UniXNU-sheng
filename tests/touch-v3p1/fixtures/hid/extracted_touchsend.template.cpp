// Synthetic regressions derived from the original public V3 fixture.
// Current V3.1 touchSend is extracted at test time; no archived source body is substituted.
#include "NativeHidTouchPlan.h"
#include <array>
#include <vector>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <atomic>
#include <algorithm>
using IOReturn=int;static constexpr int kIOReturnSuccess=0,kIOReturnNoMemory=-1,kIODirectionNone=0;
volatile uint32_t native_touch_seq=0,native_touch_count=0,native_touch_hid_reports=0,native_touch_ack_seq=0;
volatile uint64_t native_touch_time_ticks=0;
static uint64_t observed_ticks=0;
volatile int native_touch_xs[12]{},native_touch_ys[12]{},native_touch_ids[12]{};
namespace edge {volatile uint32_t native_touch_hid_multi[3]{};}
struct Event {std::array<unsigned char,NHT_REPORT_BYTES> bytes;int result;};
static std::vector<Event> events;static unsigned allocations,submits,failAllocation,failSubmit;static bool ambiguousSideEffect;static unsigned barriers,tornBarrier;
static void host_barrier(){std::atomic_thread_fence(std::memory_order_seq_cst);if(++barriers==tornBarrier)native_touch_seq=native_touch_seq+2;}
struct IOBufferMemoryDescriptor {
 std::array<unsigned char,NHT_REPORT_BYTES> data;
 static IOBufferMemoryDescriptor *withBytes(const void *bytes,unsigned n,int){assert(n==NHT_REPORT_BYTES);if(++allocations==failAllocation)return nullptr;auto *q=new IOBufferMemoryDescriptor;std::memcpy(q->data.data(),bytes,n);return q;}
};
#define OSSafeReleaseNULL(p) do{delete (p);(p)=nullptr;}while(0)
#define TOUCH_FINGERS 10
static void native_hid_input_observed(unsigned index,int){assert(index==6);}
static int submit(IOBufferMemoryDescriptor *b){int r=++submits==failSubmit?-2:0;events.push_back({b->data,r});return r;}
static void reset(){native_touch_seq=0;native_touch_count=0;native_touch_hid_reports=0;events.clear();allocations=submits=failAllocation=failSubmit=barriers=tornBarrier=0;ambiguousSideEffect=false;}
static void frame(std::initializer_list<int> ids,uint32_t sequence){native_touch_count=(uint32_t)ids.size();unsigned k=0;for(int id:ids){assert(k<12);native_touch_ids[k]=id;native_touch_xs[k]=1000+(int)k*100;native_touch_ys[k]=2000+(int)k*100;++k;}native_touch_seq=sequence;}
static std::set<int> pointset(const nht_state &s){std::set<int> r;for(unsigned i=0;i<s.count;++i)r.insert(s.point[i].id);return r;}
static std::set<int> eventset(const Event &e,bool down){std::set<int> r;unsigned count=e.bytes[NHT_REPORT_BYTES-1];assert(e.bytes[0]==3&&count<=10);for(unsigned i=0;i<count;++i){unsigned f=1+6*i;assert(e.bytes[f]==2||e.bytes[f]==3);if((e.bytes[f]==3)==down)r.insert(e.bytes[f+1]);}return r;}
static bool got_up(int id){for(auto &e:events)if(e.result==0&&eventset(e,false).count(id))return true;return false;}
struct OldDevice {
    uint32_t lastSeq=0;
    int prevIds[10]={-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},prevX[10]{},prevY[10]{};
    bool hasEventDriverCached(){return true;}
    IOReturn submitReport(IOBufferMemoryDescriptor *b){return submit(b);}
    static unsigned transformCoordinate(int value,int maximum){if(value<0)value=0;if(value>=maximum)value=maximum-1;return (unsigned)((long long)value*32767/(maximum-1));}
    void touchSend() {
        OldDevice *self=this;
        if(!self->hasEventDriverCached())return;
        uint32_t before=native_touch_seq;
        if((before&1) || before==self->lastSeq)return;
        host_barrier();
        unsigned count=native_touch_count;if(count>TOUCH_FINGERS)count=TOUCH_FINGERS;
        int ids[TOUCH_FINGERS],xs[TOUCH_FINGERS],ys[TOUCH_FINGERS];
        for(unsigned i=0;i<count;++i){ids[i]=native_touch_ids[i];xs[i]=native_touch_xs[i];ys[i]=native_touch_ys[i];}
        host_barrier();
        if(native_touch_seq!=before)return;      // torn read: retry on the next tick
        self->lastSeq=before;
        // Sizes follow the axis maxima of the touch core: 30480 long, 20320 short, mapped to the same 0..32767 range.
        unsigned char report[1+TOUCH_FINGERS*6+1]={3};
        unsigned slot=0;
        int nextIds[TOUCH_FINGERS],nextX[TOUCH_FINGERS],nextY[TOUCH_FINGERS];
        for(unsigned i=0;i<count && slot<TOUCH_FINGERS;++i,++slot) {
            unsigned x=transformCoordinate(xs[i],30480),y=transformCoordinate(ys[i],20320);
            unsigned char *f=report+1+slot*6;
            f[0]=3;f[1]=(unsigned char)(ids[i]&0xff);f[2]=(unsigned char)x;f[3]=(unsigned char)(x>>8);f[4]=(unsigned char)y;f[5]=(unsigned char)(y>>8);   // tip, valid
            nextIds[slot]=ids[i];nextX[slot]=(int)x;nextY[slot]=(int)y;
        }
        const unsigned downCount=slot;
        if(downCount>=2)edge::native_touch_hid_multi[0]=edge::native_touch_hid_multi[0]+1;
        if(downCount>edge::native_touch_hid_multi[1])edge::native_touch_hid_multi[1]=downCount;
        // Identifiers that were down and are gone: tip up at the last known position. They are matched by identifier, not by slot:
        // the producer packs its fingers, so when one lifts the others move down a slot and a slot match would lift a finger still
        // down and leave the lifted one stuck.
        for(unsigned j=0;j<TOUCH_FINGERS;++j) {
            const int id=self->prevIds[j];
            if(id<0)continue;
            bool kept=false;
            for(unsigned i=0;i<downCount && !kept;++i)kept=(ids[i]&0xff)==(id&0xff);
            if(kept || slot>=TOUCH_FINGERS)continue;
            unsigned char *f=report+1+slot*6;
            f[0]=2;f[1]=(unsigned char)(id&0xff);   // tip up, valid
            f[2]=(unsigned char)self->prevX[j];f[3]=(unsigned char)(self->prevX[j]>>8);
            f[4]=(unsigned char)self->prevY[j];f[5]=(unsigned char)(self->prevY[j]>>8);
            ++slot;edge::native_touch_hid_multi[2]=edge::native_touch_hid_multi[2]+1;
        }
        for(unsigned j=0;j<TOUCH_FINGERS;++j) {
            self->prevIds[j]=j<downCount?nextIds[j]:-1;
            if(j<downCount){self->prevX[j]=nextX[j];self->prevY[j]=nextY[j];}
        }
        if(!slot)return;
        report[1+TOUCH_FINGERS*6]=(unsigned char)slot;
        IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withBytes(report,sizeof(report),kIODirectionNone);
        IOReturn result=buffer?self->submitReport(buffer):kIOReturnNoMemory;
        OSSafeReleaseNULL(buffer);
        native_hid_input_observed(6,result);
        native_touch_hid_reports=native_touch_hid_reports+1;
    }
};

struct NewDevice {
    uint32_t lastSeq=0;
    int prevIds[10]={-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},prevX[10]{},prevY[10]{};
    bool hasEventDriverCached(){return true;}
    IOReturn submitTouchReport(IOBufferMemoryDescriptor *b,uint64_t ticks){observed_ticks=ticks;return submit(b);}
    static unsigned transformCoordinate(int value,int maximum){if(value<0)value=0;if(value>=maximum)value=maximum-1;return (unsigned)((long long)value*32767/(maximum-1));}
/* ACTUAL_V3P1_TOUCHSEND_BODY */
};

static unsigned tests;static void pass(const char*s){++tests;std::printf("PASS %s\n",s);}
template<class D>static bool empty_failure(bool allocation){reset();D d;frame({1},2);d.touchSend();assert(d.prevIds[0]==1);frame({},4);if(allocation)failAllocation=allocations+1;else failSubmit=submits+1;d.touchSend();failAllocation=failSubmit=0;frame({},6);d.touchSend();return got_up(1);}
template<class D>static bool full_replace(bool descriptor_only){reset();D d;frame({1,2,3,4,5,6,7,8,9,10},2);d.touchSend();if(descriptor_only)frame({11,12,13,14,15,16,17,18,19,20},4);else frame({3,4,5,6,7,8,9,10,11,12},4);d.touchSend();return got_up(1)&&got_up(2);}
int main(){std::setvbuf(stdout,nullptr,_IONBF,0);
 assert(!empty_failure<OldDevice>(true)&&empty_failure<NewDevice>(true));pass("actual_touchSend_old_loses_empty_UP_on_allocation_failure_new_preserves_for_next_notify");
 assert(!empty_failure<OldDevice>(false)&&empty_failure<NewDevice>(false));pass("actual_touchSend_old_loses_empty_UP_on_submit_failure_new_preserves_successful_ACK_state");
 assert(!full_replace<OldDevice>(false)&&full_replace<NewDevice>(false));pass("Core_valid_old1to10_then_new3to12_old_omits_two_UP_new_sends_both_before_new_DOWN");
 assert(!full_replace<OldDevice>(true)&&full_replace<NewDevice>(true));pass("descriptor_valid_disjoint10replacement_fits_two_packets_explicitly_not_Core12_range_claim");
 {reset();NewDevice d;frame({1,2,3,4,5,6,7,8,9,10},2);d.touchSend();frame({3,4,5,6,7,8,9,10,11,12},4);failSubmit=3;d.touchSend();assert(d.lastSeq==2);std::set<int> retained;for(int id:d.prevIds)if(id>=0)retained.insert(id);assert(retained==std::set<int>({3,4,5,6,7,8,9,10})&&got_up(1)&&got_up(2));failSubmit=0;d.touchSend();assert(d.lastSeq==4&&d.prevIds[9]==12&&events.size()==4);pass("phase2_failure_retains_phase1_survivor_ACK_and_same_snapshot_can_finish_later");}
 {reset();NewDevice d;frame({1,2,3,4,5,6,7,8,9,10},2);d.touchSend();frame({3,4,5,6,7,8,9,10,11,12},4);failAllocation=allocations+1;d.touchSend();assert(d.lastSeq==2&&d.prevIds[0]==1&&d.prevIds[9]==10&&events.size()==1);failAllocation=0;d.touchSend();assert(d.lastSeq==4&&got_up(1)&&got_up(2));pass("phase1_allocation_failure_does_not_ack_or_admit_new_contact_set");}
 {reset();NewDevice d;frame({1,2,3,4,5,6,7,8,9,10},2);d.touchSend();frame({3,4,5,6,7,8,9,10,11,12},4);failSubmit=submits+1;d.touchSend();assert(d.lastSeq==2&&d.prevIds[0]==1&&!got_up(1));failSubmit=0;frame({4,5,6},6);d.touchSend();assert(d.lastSeq==6&&got_up(1)&&got_up(2)&&got_up(3)&&got_up(7)&&got_up(10));pass("failed_phase1_then_newer_notification_rebuilds_releases_from_ACK_not_failed_desired_state");}
 {reset();NewDevice d;frame({1},2);failSubmit=1;d.touchSend();assert(d.lastSeq==0&&d.prevIds[0]==-1);failSubmit=0;d.touchSend();assert(d.lastSeq==2&&d.prevIds[0]==1&&events.size()==2);pass("failed_DOWN_does_not_commit_sequence_same_snapshot_remains_retryable_without_adding_timer");}
 {reset();NewDevice d;frame({1},2);tornBarrier=2;d.touchSend();assert(d.lastSeq==0&&d.prevIds[0]==-1&&events.empty());pass("actual_extracted_seqlock_torn_snapshot_still_never_submits_or_mutates_ACK");}
 {reset();NewDevice d;frame({},2);d.touchSend();assert(d.lastSeq==2&&events.empty()&&allocations==0);frame({1},4);d.touchSend();frame({},6);d.touchSend();assert(events.size()==2&&got_up(1)&&d.lastSeq==6&&d.prevIds[0]==-1);pass("empty_noop_and_single_packet_real_UP_keep_existing62byte_descriptor_wire_format");}
 {nht_state a{};a.sequence=2;a.count=1;a.point[0]={1,1000,2000};nht_point current[10]={{2,3000,4000}};nht_plan p;assert(nht_build(&a,current,1,4,&p)==1);nht_state before=a;assert(!nht_commit(&a,&p,1,1)&&!nht_commit(&a,&p,0,0)&&std::memcmp(&a,&before,sizeof a)==0);assert(nht_commit(&a,&p,0,1)&&a.sequence==4&&a.point[0].id==2);pass("pure_commit_requires_success_and_in_order_packet_index");}
 {nht_state a{};nht_plan p;nht_point now[11]{};now[0]={1,100,100};now[1]={1,200,200};assert(nht_build(&a,now,2,2,&p)==-1);assert(nht_build(&a,now,11,2,&p)==-1);assert(nht_build(&a,now,1,3,&p)==-1);now[0].id=256;assert(nht_build(&a,now,1,2,&p)==-1);now[0]={1,-1,0};assert(nht_build(&a,now,1,2,&p)==-1);pass("invalid_duplicate_range_capacity_or_odd_snapshot_rejected_without_ACK_mutation");}
 {unsigned seed=0x12345;unsigned runs=0;for(unsigned k=0;k<10000;++k){seed=seed*1664525u+1013904223u;unsigned old=seed&4095;seed=seed*1664525u+1013904223u;unsigned fresh=seed&4095;if(__builtin_popcount(old)>10||__builtin_popcount(fresh)>10)continue;nht_state a{};a.sequence=2;nht_point now[10];unsigned n=0;std::set<int> expected,removed,seenups;for(unsigned i=0;i<12;++i){if(old&(1u<<i))a.point[a.count++]={(int)i+1,1000+(int)i,2000};if(fresh&(1u<<i))now[n++]={(int)i+1,3000+(int)i,4000},expected.insert((int)i+1);if((old&(1u<<i))&&!(fresh&(1u<<i)))removed.insert((int)i+1);}nht_plan p;assert(nht_build(&a,now,n,4,&p)>=0);for(unsigned j=0;j<p.count;++j){Event e{};std::memcpy(e.bytes.data(),p.packet[j].bytes,62);auto ups=eventset(e,false);seenups.insert(ups.begin(),ups.end());assert(p.packet[j].entries<=10&&nht_commit(&a,&p,j,1));}if(!p.count)a.sequence=4;assert(pointset(a)==expected&&seenups==removed&&a.sequence==4);++runs;}std::printf("{\"kind\":\"Core12_set_property\",\"cases\":%u}\n",runs);pass("all_generated_Core12_sets_ack_to_desired_and_release_every_vanished_ID_without_overflow");}
 {reset();NewDevice d;frame({1},2);failSubmit=1;ambiguousSideEffect=true;d.touchSend();assert(d.lastSeq==0);frame({},4);failSubmit=0;d.touchSend();assert(!got_up(1));pass("explicit_limit_ambiguous_failed_unacknowledged_DOWN_not_claimed_safe_or_delivered");}
 {reset();NewDevice d;native_touch_time_ticks=123456;frame({1},2);d.touchSend();assert(observed_ticks==123456&&native_touch_ack_seq==2);pass("actual_V3p1_sender_preserves_original_tick_and_successful_ACK");}
 std::printf("{\"status\":\"PASS_PURE_PLANNER_AND_EXTRACTED_TOUCHSEND_HOST_ONLY\",\"tests\":%u,\"report_bytes_including_ID\":%d,\"no_new_timer\":true}\n",tests,NHT_REPORT_BYTES);
}
