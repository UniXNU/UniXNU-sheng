#include "TouchEngine.h"
#include "TouchAstraProfile.h"
#include "TouchReportTimeline.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
static unsigned passed;
static void ok(const char*n){++passed;std::printf("PASS %s\n",n);}
struct Feed {
 std::unique_ptr<nvt_touch_core> c{new nvt_touch_core{}};
 std::unique_ptr<nvt_partial_state> pm{new nvt_partial_state{}};
 std::unique_ptr<TouchReportTimeline> line{new TouchReportTimeline};
 TouchEngine e{c.get(),pm.get()};
 std::array<int16_t,NVT_NODES> m{},decoded{};
 std::array<uint8_t,NVT_THP_FRAME_LEN> raw{};
 nvt_contact out[NVT_SLOTS]{};uint64_t t=0;unsigned seq=0;
 struct Event{unsigned count;uint64_t ticks;int x;};std::vector<Event> events;
 static bool sink(void*ctx,unsigned n,const nvt_contact*c,uint64_t ticks){auto*f=static_cast<Feed*>(ctx);f->events.push_back({n,ticks,n?c[0].x:0});return true;}
 Feed(){nvt_touch_reset(c.get());nvt_partial_reset(pm.get());const auto cfg=touchAstraReportExperimentProfile();assert(nvt_partial_configure(pm.get(),&cfg));line->reset();flat();packet(2,0);assert(e.frame(raw.data(),raw.size(),0,decoded.data(),out,NVT_SLOTS)==-1);}
 void flat(){m.fill(10000);}
 void blob(double x,double y,int amp=1000){for(int r=0;r<40;++r)for(int c=0;c<60;++c)m[r*60+c]=(int16_t)(10000+std::lround(amp*std::exp(-.5*((c-x)*(c-x)+(r-y)*(r-y)/(.85*.85)))));}
 void packet(unsigned type,int block){raw.fill(0);auto*h=raw.data()+NVT_THP_HEADER_LEN;h[2]=(uint8_t)++seq;h[3]=(uint8_t)(seq>>8);h[12]=h[13]=255;for(int i=16;i<20;++i)h[i]=255;h[24]=(uint8_t)type;if(block){raw[313]=0x1d;raw[332]=(uint8_t)block;for(int i=357;i<4197;i+=2){raw[i]=255;raw[i+1]=63;}for(int i=0;i<600;++i){auto v=(uint16_t)m[(block-1)*600+i];raw[4217+2*i]=(uint8_t)v;raw[4218+2*i]=(uint8_t)(v>>8);}}else std::memcpy(raw.data()+321,m.data(),m.size()*2);}
 int at(uint64_t now,int block=0){t=now;(void)e.expire(t,out,NVT_SLOTS);packet(4,block);int n=e.frame(raw.data(),raw.size(),t,decoded.data(),out,NVT_SLOTS);if(n>=0 || !block){assert(line->feed(t,t*24,*c));line->flush(t,sink,this);}return n;}
 int down(){blob(25.2,19.5);(void)at(1000);int n=at(9333);assert(n==1);return out[0].tracking_id;}
 nvt_slot&slot(int id){for(auto&s:c->slots)if(s.active&&s.tracking_id==id)return s;assert(false);return c->slots[0];}
};
int main(){
 {Feed f;int id=f.down();assert(f.pm->finger[0].valid);auto seed=f.slot(id).last_observed_us;unsigned hits=0;for(int k=1;k<=144;++k){f.blob(25.2+.012*k,19.5+.008*k);int n=f.at(9333+(uint64_t)k*8333,1+(k-1)%4);if(n>=0)++hits;assert(f.slot(id).tracking_id==id);}assert(hits>20&&f.slot(id).last_observed_us>seed+1000000);assert(f.c->observation_cancelled==0);ok("actual_M_template_to_PM_engine_keeps_ID_for_1_2s");assert(f.events.size()>20);for(size_t i=1;i<f.events.size();++i)assert(f.events[i].ticks>=f.events[i-1].ticks);ok("accepted_PM_positions_reach_original_time_report_timeline");}
 {Feed f;int id=f.down();auto observed=f.slot(id).last_observed_us;f.flat();for(int k=1;k<=11;++k)assert(f.at(9333+(uint64_t)k*8333,2)==TouchEngine::SpecialP);assert(f.slot(id).last_observed_us==observed);assert(f.e.expire(observed+100000,f.out,NVT_SLOTS)==0);ok("P_packets_with_no_mutual_signal_do_not_extend_finger_lease");}
 {Feed f;f.down();auto before=f.c->reference_frames;f.blob(26,20);(void)f.at(17666,3);assert(f.c->reference_frames==before);ok("PM_does_not_refresh_uncovered_fullscreen_reference");}
 {Feed f;int id=f.down();auto seen=f.slot(id).last_observed_us;f.blob(25.2,19.5);f.packet(4,2);f.raw[332]=0;assert(f.e.frame(f.raw.data(),f.raw.size(),17666,f.decoded.data(),f.out,NVT_SLOTS)==TouchEngine::SpecialP);assert(f.slot(id).last_observed_us==seen);ok("invalid_PM_block_never_renews_position");}
 {Feed f;f.down();f.blob(25.2,19.5);f.packet(4,3);f.raw[NVT_THP_HEADER_LEN+12]^=1;assert(f.e.frame(f.raw.data(),f.raw.size(),17666,f.decoded.data(),f.out,NVT_SLOTS)==TouchEngine::Rejected);ok("bad_header_rejected_before_partial_or_full_matrix_processing");}
 {Feed f;f.down();assert(f.e.expire(100,f.out,NVT_SLOTS)==0);bool valid=false;for(auto&t:f.pm->finger)valid|=t.valid;assert(!valid);ok("clock_epoch_change_invalidates_PM_templates");}
 std::printf("{\"status\":\"PASS_ASTRA_ENGINE_HOST_ONLY\",\"tests\":%u}\n",passed);
}
