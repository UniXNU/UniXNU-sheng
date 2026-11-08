extern "C" {
#include "NvtTouchCore.h"
}
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>

struct Feed {
    std::unique_ptr<nvt_touch_core> c{new nvt_touch_core};
    int16_t matrix[NVT_NODES];
    nvt_contact out[NVT_SLOTS];
    Feed(){nvt_touch_reset(c.get());flat();assert(nvt_touch_process_timed(c.get(),matrix,2,0,out,NVT_SLOTS)==-1);}
    void flat(){for(auto &x:matrix)x=5000;}
    void blob(int r,int col,int peak=1000){flat();add(r,col,peak);}
    void add(int r,int col,int peak=1000){
        matrix[r*NVT_COLUMNS+col]=(int16_t)(5000+peak);
        matrix[r*NVT_COLUMNS+col+1]=(int16_t)(5000+peak*3/4);
        matrix[(r+1)*NVT_COLUMNS+col]=(int16_t)(5000+peak*2/3);
        matrix[(r+1)*NVT_COLUMNS+col+1]=(int16_t)(5000+peak/2);
    }
    int at(uint64_t t){return nvt_touch_process_timed(c.get(),matrix,4,t,out,NVT_SLOTS);}
    int down(int row=20,int col=20){blob(row,col);assert(at(1000)==1);return out[0].tracking_id;}
    nvt_slot &slot(int id){for(auto &s:c->slots)if(s.active&&s.tracking_id==id)return s;assert(false);return c->slots[0];}
    nvt_contact pm(int id,int x,int y,int peak=280){return {-1,id,x,y,peak,4,800};}
    int partial(const nvt_contact *m,int n,uint64_t t){return nvt_touch_process_partial(c.get(),m,n,t,out,NVT_SLOTS);}
};
static unsigned passed;
static void ok(const char *name){++passed;std::printf("PASS %s\n",name);}
int main(){
    std::setvbuf(stdout,nullptr,_IONBF,0);
    {Feed f;int id=f.down();int x=f.out[0].x;f.blob(20,21,280);
     assert(f.at(9333)==1);assert(f.out[0].tracking_id==id&&f.out[0].x>x);
     auto &s=f.slot(id);assert(s.last_filter_us==9333&&s.last_observed_us==9333&&s.filter_raw_x==s.x);
     ok("qualified_weak_moves_and_updates_one_observation_clock");}
    {Feed f;f.flat();f.matrix[20*NVT_COLUMNS+20]=6000;assert(f.at(1000)==0&&f.at(9333)==1);
     const int id=f.out[0].tracking_id,x=f.slot(id).x;f.flat();f.matrix[20*NVT_COLUMNS+20]=5280;
     assert(f.at(17666)==1);assert(f.slot(id).last_observed_us==9333&&f.slot(id).last_filter_us==9333&&f.slot(id).x==x);
     assert(nvt_touch_expire(f.c.get(),34333,f.out,NVT_SLOTS)==0&&f.c->dropout_expired==1);
     ok("unqualified_weak_is_not_accepted_then_frozen_or_renewed");}
    {Feed f;f.flat();f.matrix[20*NVT_COLUMNS+20]=6000;assert(f.at(1000)==0);
     const auto origin=f.c->slots[0].x;const auto generation=f.c->slots[0].generation;
     f.flat();f.matrix[20*NVT_COLUMNS+21]=6000;assert(f.at(9333)==1);
     auto &s=f.slot(f.out[0].tracking_id);
     assert(s.origin_x==origin&&s.generation==generation&&s.born_us==1000&&f.out[0].x>origin);
     ok("two_sample_confirmation_preserves_origin_without_overwriting_latest");}
    {Feed f;int id=f.down();auto &s=f.slot(id);auto p=f.pm(id,s.x+1000,s.y);
     assert(f.partial(&p,1,9333)==1);const long v=s.vx;const int q=s.rx;
     assert(v>0&&q<s.x);assert(f.partial(&p,1,17666)==1);
     assert(s.vx>=0&&s.vx<v&&s.rx>q);
     ok("stationary_raw_has_zero_new_derivative_while_filter_catches_up");}
    {Feed f;int id=f.down();auto &s=f.slot(id);const uint64_t gen=s.generation;
     uint64_t t=1000;for(int i=0;i<150;++i){t+=8333;auto p=f.pm(id,s.x+2,s.y+1);
       assert(nvt_touch_expire(f.c.get(),t,f.out,NVT_SLOTS)==-1);
       assert(f.partial(&p,1,t)==1&&f.slot(id).generation==gen);}
     assert(t>1200000&&f.c->observation_cancelled==0&&s.last_observed_us==t);
     ok("qualified_PM_alone_continues_over_one_second_without_larger_timeout");}
    {Feed f;int id=f.down();auto p=f.pm(id,f.slot(id).x,f.slot(id).y);
     assert(nvt_touch_expire(f.c.get(),101000,f.out,NVT_SLOTS)==0);
     assert(f.partial(&p,1,102000)==-1);
     ok("expired_generation_cannot_be_revived_by_PM");}
    {Feed f;f.flat();f.add(10,10);f.add(25,40);assert(f.at(1000)==0&&f.at(9333)==2);
     const int a=f.out[0].tracking_id,b=f.out[1].tracking_id;auto &s=f.slot(a);
     const auto b_time=f.slot(b).last_observed_us,b_gen=f.slot(b).generation;auto p=f.pm(a,s.x+20,s.y);
     assert(f.partial(&p,1,18000)==2);assert(f.slot(b).last_observed_us==b_time&&f.slot(b).generation==b_gen);
     ok("partial_uncovered_other_finger_stays_unknown_without_UP_or_renewal");}
    {Feed f;int id=f.down();auto &s=f.slot(id);auto p=f.pm(id,s.x+20,s.y);nvt_contact a[2]={p,p};
     assert(f.partial(a,2,9333)==-1&&s.last_observed_us==1000);
     ok("duplicate_partial_ID_batch_rejected_before_any_update");}
    {Feed f;int id=f.down();auto p=f.pm(id+1,1000,1000);
     assert(f.partial(&p,1,9333)==-1&&f.c->next_generation==1);
     ok("partial_cannot_create_new_DOWN_or_consume_generation");}
    {Feed f;int id=f.down();auto &s=f.slot(id);auto p=f.pm(id,NVT_LONG_AXIS_MAX,s.y);
     assert(f.partial(&p,1,9333)==-1&&s.last_observed_us==1000);
     ok("partial_invalid_axis_rejected");}
    {Feed f;int id=f.down();auto &s=f.slot(id);const auto g=s.generation;auto p=f.pm(id,s.x+50,s.y);
     assert(f.partial(&p,1,9333)==1);f.blob(20,20,280);assert(f.at(17666)==1&&f.slot(id).generation==g);
     ok("M_PM_M_keeps_ID_filter_and_generation");}
    {Feed f;int id=f.down();auto &s=f.slot(id);auto p=f.pm(id,s.x+6000,s.y);
     assert(f.partial(&p,1,9333)==-1&&s.last_observed_us==1000);
     ok("prequalified_PM_still_obeys_spatial_identity_gate");}
    {Feed f;int id=f.down();auto &s=f.slot(id);auto p=f.pm(id,s.x,s.y);
     assert(f.partial(&p,1,500)==-1&&s.last_observed_us==1000);
     ok("rollback_PM_cannot_rewind_core_time_or_extend_track");}
    {Feed f;f.flat();f.matrix[20*NVT_COLUMNS+20]=6000;assert(f.at(1000)==0);
     uint64_t g=f.c->slots[0].generation;assert(nvt_touch_expire(f.c.get(),26000,f.out,NVT_SLOTS)==-1);
     f.blob(20,20);assert(f.at(27000)==1);assert(f.slot(f.out[0].tracking_id).generation>g);
     ok("new_contact_reinitializes_origin_and_generation");}
    {Feed f;int id=f.down();auto &s=f.slot(id);s.dx=6000;s.dy=0;
     f.blob(20,20);assert(f.at(25999)==1&&f.out[0].tracking_id==id);
     ok("strong_stop_uses_measured_h0_even_when_h1_predicts_far");}
    {Feed f;int id=f.down();auto &s=f.slot(id);s.dx=6000;s.dy=0;
     f.blob(20,20,280);assert(f.at(9333)==1&&f.out[0].tracking_id==id&&s.last_filter_us==9333);
     ok("weak_stop_uses_same_measured_h0_as_strong");}
    {Feed f;f.flat();f.add(10,10);f.add(25,40);assert(f.at(1000)==0&&f.at(9333)==2);
     int id0=f.out[0].tracking_id,id1=f.out[1].tracking_id;f.flat();f.add(10,11,280);f.add(25,39,280);
     assert(f.at(17666)==2&&f.out[0].tracking_id!=f.out[1].tracking_id);
     assert(f.slot(id0).last_filter_us==17666&&f.slot(id1).last_filter_us==17666);
     ok("global_one_to_one_matching_accepts_distinct_weak_measurements");}
    {Feed f;int id=f.down();auto &s=f.slot(id);auto p=f.pm(id,s.x+20,s.y);
     assert(f.partial(&p,1,9333)==1);f.flat();assert(f.at(17666)==0);
     ok("reliable_full_empty_M_still_releases_PM_continued_contact");}
    {Feed f;int id=f.down();auto &s=f.slot(id);auto p=f.pm(id,s.x,s.y);
     assert(f.partial(&p,1,9333)==1);assert(f.partial(&p,1,9333)==-1);
     assert(nvt_touch_wait_us(f.c.get(),9333)==100000);
     ok("same_timestamp_PM_does_not_renew_position_deadline_twice");}
    {Feed f;int id=f.down();auto &s=f.slot(id);s.vx=100000;s.vy=50000;
     auto p=f.pm(id,s.x,s.y);assert(f.partial(&p,1,40000)==1&&s.vx==0&&s.vy==0);
     ok("gap_recovery_discards_stale_velocity_memory");}
    {Feed f;int id=f.down();auto &s=f.slot(id);const auto gen=s.generation;
     assert(nvt_touch_note_presence(f.c.get(),id,gen,90000)==1);
     assert(s.last_presence_us==90000&&s.last_observed_us==1000);
     assert(nvt_touch_expire(f.c.get(),101000,f.out,NVT_SLOTS)==0);
     assert(nvt_touch_note_presence(f.c.get(),id,gen,102000)==0);
     ok("presence_does_not_extend_position_deadline_or_revive_expired_generation");}
    {Feed f;f.flat();f.add(10,10);f.add(25,40);assert(f.at(1000)==0&&f.at(9333)==2);
     const int a=f.out[0].tracking_id,b=f.out[1].tracking_id;const auto gen=f.slot(a).generation;
     assert(nvt_touch_covered_negative(f.c.get(),a,gen+1,18000,f.out,NVT_SLOTS)==-1);
     assert(nvt_touch_covered_negative(f.c.get(),a,gen,18000,f.out,NVT_SLOTS)==1);
     assert(f.out[0].tracking_id==b);
     ok("qualified_covered_negative_ends_only_exact_generation_and_returns_other_finger");}
    std::printf("{\"status\":\"PASS_ASTRA_CORE_HOST_ONLY\",\"tests\":%u,\"Core_bytes\":%zu,\"slot_bytes\":%zu}\n",passed,sizeof(nvt_touch_core),sizeof(nvt_slot));
}
