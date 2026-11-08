// Regional-observation host regression fixtures. Device behavior unverified.
extern "C" {
#include "NvtTouchCore.h"
}
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>

struct RegionFeed {
    std::unique_ptr<nvt_touch_core> core{new nvt_touch_core};
    int16_t matrix[NVT_NODES];
    nvt_contact out[NVT_SLOTS];
    nvt_region_candidates region;
    RegionFeed(){nvt_touch_reset(core.get());flat();assert(nvt_touch_process_timed(core.get(),matrix,2,0,out,NVT_SLOTS)==-1);}
    void flat(){for(auto &v:matrix)v=5000;}
    void blob(int row,int col,int amplitude=1000){flat();add(row,col,amplitude);}
    void add(int row,int col,int amplitude=1000) {
        matrix[row*NVT_COLUMNS+col]=(int16_t)(5000+amplitude);
        matrix[row*NVT_COLUMNS+col+1]=(int16_t)(5000+amplitude*3/4);
        matrix[(row+1)*NVT_COLUMNS+col]=(int16_t)(5000+amplitude*2/3);
        matrix[(row+1)*NVT_COLUMNS+col+1]=(int16_t)(5000+amplitude/2);
    }
    int extract(int row=10,int count=10){return nvt_touch_extract_region(core.get(),matrix+row*NVT_COLUMNS,row,count,&region);}
    int at(uint64_t t){return nvt_touch_process_region(core.get(),&region,t,out,NVT_SLOTS);}
    int full(uint64_t t){return nvt_touch_process_timed(core.get(),matrix,4,t,out,NVT_SLOTS);}
    nvt_slot &slot(int id){for(auto &s:core->slots)if(s.active&&s.tracking_id==id)return s;assert(false);return core->slots[0];}
    int down(){blob(15,20);assert(extract()==1&&at(1000)==0&&extract()==1&&at(9333)==1);return out[0].tracking_id;}
};
static unsigned checks;
static void ok(const char *name){++checks;std::printf("PASS %s\n",name);}
int main() {
    {RegionFeed full,regional;full.blob(15,20);regional.blob(15,20);
     assert(full.full(1000)==1&&regional.extract()==1);
     const auto &a=full.out[0];const auto &b=regional.region.contact[0];
     assert(a.x==b.x&&a.y==b.y&&a.peak==b.peak&&a.area==b.area&&a.weight==b.weight);
     ok("complete_PM_blob_uses_identical_full_M_segmentation_merge_centroid_mapping");}
    {RegionFeed f;f.blob(15,20);assert(f.extract()==1&&f.at(1000)==0);
     const int origin=f.core->slots[0].origin_x;
     const auto gen=f.core->slots[0].generation;
     f.blob(15,21);assert(f.extract()==1&&f.at(9333)==1);
     auto &s=f.slot(f.out[0].tracking_id);assert(s.origin_x==origin&&s.generation==gen&&f.out[0].x>origin);
     ok("new_full_PM_contact_needs_independent_confirmation_and_keeps_second_position");}
    {RegionFeed f;int id=f.down();int x=f.out[0].x;
     f.blob(15,21,280);assert(f.extract()==1&&f.region.quality_mask!=0&&f.at(17666)==1);
     assert(f.out[0].tracking_id==id&&f.out[0].x>x&&f.slot(id).last_filter_us==17666);
     ok("complete_weak_PM_observation_follows_same_accepted_position_path");}
    {RegionFeed f;int id=f.down();const auto time=f.slot(id).last_observed_us;f.flat();
     assert(f.extract()==0&&f.at(18000)==-1);
     assert(f.slot(id).last_observed_us==time&&f.slot(id).missed==0);
     assert(nvt_touch_expire(f.core.get(),time+100000,f.out,NVT_SLOTS)==0);
     ok("empty_region_is_UNKNOWN_not_fullscreen_UP_or_position_renewal");}
    {RegionFeed f;f.flat();f.add(15,20);f.add(30,40);assert(f.full(1000)==0&&f.full(9333)==2);
     const int a=f.out[0].tracking_id,b=f.out[1].tracking_id;
     int aid=f.slot(a).y<f.slot(b).y?a:b,bid=aid==a?b:a;
     const auto bt=f.slot(bid).last_observed_us,bg=f.slot(bid).generation;
     f.blob(15,21);assert(f.extract()==1&&f.at(17666)==2);
     assert(f.slot(aid).last_observed_us==17666&&f.slot(bid).last_observed_us==bt&&f.slot(bid).generation==bg);
     ok("uncovered_other_finger_keeps_identity_without_implicit_negative_evidence");}
    {RegionFeed f;f.blob(19,20);assert(f.extract()==0&&f.region.clipped_count>0);
     assert(f.at(1000)==-1&&f.core->next_generation==0);
     ok("boundary_cut_blob_is_excluded_from_centroid_path_and_cannot_create_DOWN");}
    {RegionFeed f;f.blob(17,20);assert(f.extract()==0&&f.region.clipped_count>0);
     ok("required_background_merge_ring_must_also_be_covered");}
    {RegionFeed full,regional;full.blob(0,20);regional.blob(0,20);
     assert(full.full(1000)==1&&regional.extract(0,10)==1);
     assert(full.out[0].x==regional.region.contact[0].x&&full.out[0].y==regional.region.contact[0].y);
     ok("physical_panel_border_is_not_an_unknown_missing_stripe");}
    {RegionFeed f;f.flat();f.add(16,20,1000);f.add(18,20,900);
     assert(f.extract()==0&&f.region.clipped_count>0);
     ok("merged_cluster_inheriting_clipped_support_is_not_a_complete_peak");}
    {RegionFeed f;for(auto &v:f.matrix)v=5002;
     const auto before=*f.core;assert(f.extract()==0);
     assert(std::memcmp(f.core->reference,before.reference,sizeof before.reference)==0);
     assert(f.core->reference_frames==before.reference_frames&&f.core->frames_by_type[0]==before.frames_by_type[0]&&f.core->frames_by_type[1]==before.frames_by_type[1]);
     ok("regional_extraction_does_not_update_baseline_or_ordinary_M_frame_counters");}
    {RegionFeed f;f.blob(15,20);assert(f.extract()==1&&f.at(1000)==0&&f.at(1000)==-1);
     assert(f.core->slots[0].age==1);assert(f.extract()==1&&f.at(9333)==1);
     ok("duplicate_regional_timestamp_cannot_confirm_pending_contact");}
    {RegionFeed f;f.flat();f.add(15,10);f.add(15,40);
     assert(f.extract()==2&&f.at(1000)==0&&f.extract()==2&&f.at(9333)==2);
     assert(f.out[0].tracking_id!=f.out[1].tracking_id);
     ok("multiple_complete_PM_peaks_use_global_one_to_one_identity_assignment");}
    {RegionFeed f;f.blob(15,20,280);assert(f.extract()==1&&f.at(1000)==-1&&f.core->next_generation==0);
     ok("weak_complete_PM_peak_can_continue_but_cannot_create_new_contact");}
    {RegionFeed f;f.blob(15,20);assert(f.extract()==1&&f.at(1000)==0);
     assert(nvt_touch_wait_us(f.core.get(),1000)==25000);
     assert(nvt_touch_expire(f.core.get(),26000,f.out,NVT_SLOTS)==-1&&f.core->pending_expired==1);
     ok("regional_pending_uses_same_25ms_deadline_even_without_more_packets");}
    {RegionFeed f;int id=f.down();uint64_t t=9333;
     for(int i=0;i<150;++i){t+=8333;assert(f.extract()==1&&f.at(t)==1&&f.out[0].tracking_id==id);}
     assert(f.core->observation_cancelled==0&&t>1200000);
     ok("complete_PM_regions_continue_existing_contact_without_Gaussian_or_larger_timeout");}
    {RegionFeed f;f.blob(15,20);assert(f.extract(0,NVT_ROWS)==1);
     RegionFeed full;full.blob(15,20);assert(full.full(1000)==1);
     assert(full.out[0].x==f.region.contact[0].x&&full.out[0].y==f.region.contact[0].y);
     ok("full_coverage_extraction_is_same_shared_candidate_body_as_M");}
    {RegionFeed f;f.down();const auto old_generation=f.core->next_generation;
     assert(nvt_touch_expire(f.core.get(),100,f.out,NVT_SLOTS)==0&&f.core->clock_rebases==1);
     assert(f.core->region_clock_valid==0);
     assert(f.extract()==1&&f.at(200)==0&&f.extract()==1&&f.at(8533)==1);
     assert(f.slot(f.out[0].tracking_id).generation>old_generation);
     ok("clock_rebase_cancels_old_generation_and_resets_regional_freshness_clock");}
    {RegionFeed f;f.blob(15,20);assert(f.extract()==1&&f.at(1000)==0);
     const auto generation=f.core->next_generation;
     assert(nvt_touch_expire(f.core.get(),26000,f.out,NVT_SLOTS)==-1);
     assert(f.extract()==1&&f.at(34332)==0&&f.core->next_generation>generation);
     ok("documented_limit_33ms_stripe_revisit_cannot_confirm_25ms_pending_contact");}
    std::printf("{\"status\":\"PASS_ASTRA_REGION_HOST_ONLY\",\"tests\":%u}\n",checks);
}
