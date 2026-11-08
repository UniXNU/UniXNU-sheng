// V3.1 host regression fixtures for actual integer PM processing.
// SPDX-License-Identifier: Apache-2.0
#include "NvtPartialObservation.h"
#include "NvtProtocol.h"
#include "TouchAstraProfile.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
static unsigned tests=0;
static void require(bool condition,const char *name) {
    if(!condition){std::fprintf(stderr,"FAIL %s\n",name);std::exit(1);}
    std::printf("PASS %s\n",name);++tests;
}
static int axis(double value,bool row) {
    int k=(int)std::floor(value);double f=value-k;
    int a=row?nvt_touch_row_center(k):nvt_touch_column_center(k);
    int b=row?nvt_touch_row_center(k+1):nvt_touch_column_center(k+1);
    int v=(int)std::lround((a+(b-a)*f)*10);
    return nvt_touch_map_edge_axis(v,row?NVT_SHORT_AXIS_MAX:NVT_LONG_AXIS_MAX,row);
}
struct Fixture {
    std::unique_ptr<nvt_touch_core> core;
    std::unique_ptr<nvt_partial_state> state;
    std::array<int16_t,NVT_NODES> matrix;
    std::array<uint8_t,NVT_THP_FRAME_LEN> raw;
    uint64_t time=1000;
    Fixture(double x=25.2,double y=19.5) : core(new nvt_touch_core{}),state(new nvt_partial_state{}) {
        nvt_touch_reset(core.get());nvt_partial_reset(state.get());
        const auto profile=touchAstraReportExperimentProfile();
        if(!nvt_partial_configure(state.get(),&profile)){std::fprintf(stderr,"FAIL explicit report profile\n");std::exit(1);}
        core->have_reference=1;core->timed_enabled=1;
        for(auto &v:core->reference)v=10000;
        auto &slot=core->slots[0];
        slot.active=1;slot.age=2;slot.tracking_id=1;slot.born_us=1000;
        slot.generation=1;slot.x=axis(x,false);slot.y=axis(y,true);
        slot.rx=slot.x;slot.ry=slot.y;slot.last_observed_us=time;
        slot.last_filter_us=time;slot.fx=slot.x*16;slot.fy=slot.y*16;
        slot.filter_raw_x=slot.x;slot.filter_raw_y=slot.y;
        make_matrix(x,y);nvt_partial_seed(state.get(),core.get(),matrix.data(),time);
    }
    void make_matrix(double x,double y,int a=1000,bool noise=false) {
        for(int r=0;r<NVT_ROWS;++r)for(int c=0;c<NVT_COLUMNS;++c) {
            double d=a*std::exp(-.5*((c-x)*(c-x)+(r-y)*(r-y)/(.85*.85)));
            int n=noise?((r*37+c*19)%5)-2:0;
            matrix[r*NVT_COLUMNS+c]=(int16_t)(10000+(int)std::lround(d)+n);
        }
    }
    void pack(int b) {
        raw.fill(0);raw[NVT_THP_HEADER_LEN+24]=4;raw[313]=0x1d;raw[332]=(uint8_t)b;
        raw[NVT_THP_HEADER_LEN+12]=255;raw[NVT_THP_HEADER_LEN+13]=255;
        for(int k=16;k<20;++k)raw[NVT_THP_HEADER_LEN+k]=255;
        for(unsigned off=357;off<4197;off+=2){raw[off]=255;raw[off+1]=63;}
        for(int k=0;k<600;++k) {
            int16_t v=matrix[(b-1)*600+k];
            raw[4217+2*k]=(uint8_t)v;raw[4218+2*k]=(uint8_t)((uint16_t)v>>8);
        }
    }
    int measure(int b,nvt_contact *out,int count=NVT_SLOTS) {
        pack(b);time+=8333;
        return nvt_partial_measure(state.get(),core.get(),raw.data(),raw.size(),time,out,count);
    }
};
int main() {
    nvt_contact out[NVT_SLOTS];
    Fixture f;require(f.state->finger[0].valid,"template_learned_from_actual_full_M_delta");
    for(int block:{2,3}) {
        const int n=f.measure(block,out);
        if(n!=1)std::fprintf(stderr,"boundary block%d reason%d mseQ8%d info%d\n",block,f.state->last_reason,f.state->last_rms_q8,f.state->last_information);
        require(n==1 && std::abs(out[0].x-axis(25.2,false))<30 &&
                std::abs(out[0].y-axis(19.5,true))<30,
                block==2?"upper_half_boundary_fit":"lower_half_boundary_fit");
    }
    auto valid=f.raw;auto t=f.time;
    require(nvt_partial_measure(f.state.get(),f.core.get(),valid.data(),valid.size(),t,out,NVT_SLOTS)==-1,
            "same_timestamp_duplicate_does_not_reobserve");
    f.raw[332]=0;
    require(nvt_partial_measure(f.state.get(),f.core.get(),f.raw.data(),f.raw.size(),t+1,out,NVT_SLOTS)==-1,
            "invalid_block_rejected");
    f.raw=valid;
    require(nvt_partial_measure(f.state.get(),f.core.get(),f.raw.data(),f.raw.size()-1,t+1,out,NVT_SLOTS)==-1,
            "short_packet_rejected");
    f.raw[313]=0;
    require(nvt_partial_measure(f.state.get(),f.core.get(),f.raw.data(),f.raw.size(),t+1,out,NVT_SLOTS)==-1,
            "ordinary_M_not_reinterpreted_as_PM");
    Fixture quiet;quiet.matrix.fill(10000);
    require(quiet.measure(2,out)==0,"large_pen_planes_with_quiet_mutual_do_not_make_fingers");
    quiet.matrix[19*NVT_COLUMNS+25]=11000;
    require(quiet.measure(2,out)==0,"isolated_mutual_spike_not_position");
    Fixture wrong;wrong.core->slots[0].generation=2;
    require(wrong.measure(2,out)==0 && !wrong.state->finger[0].valid,
            "reused_ID_generation_cannot_inherit_template");
    Fixture overlap;
    overlap.core->slots[1]=overlap.core->slots[0];
    overlap.core->slots[1].tracking_id=2;overlap.core->slots[1].generation=2;
    overlap.core->slots[1].x+=500;
    require(overlap.measure(2,out)==0,"overlapping_ID_templates_cannot_explain_same_peak");
    Fixture second;
    second.make_matrix(24.0,19.5);
    for(int r=0;r<NVT_ROWS;++r)for(int c=0;c<NVT_COLUMNS;++c)
        second.matrix[r*NVT_COLUMNS+c]+=(int16_t)std::lround(1000*std::exp(-.5*((c-27.)*(c-27.)+(r-19.5)*(r-19.5)/(.85*.85))));
    require(second.measure(2,out)==0,"unresolved_two_peak_shape_rejected");
    for(int route=0;route<3;++route) {
        const double y0=route==0?15.0:route==1?19.5:17.0;
        Fixture motion(18.0,y0);
        uint64_t accepted=0,last=motion.time,maxgap=0;
        int worst=0;
        for(int k=1;k<=144;++k) {
            const double x=18.0+.025*k,y=y0+(route==2?.035*k:0);
            motion.make_matrix(x,y,1000,true);
            int n=motion.measure(1+(k-1)%4,out);
            if(n>0) {
                int e=std::abs(out[0].x-axis(x,false))+std::abs(out[0].y-axis(y,true));
                worst=std::max(worst,e);
                maxgap=std::max(maxgap,motion.time-last);last=motion.time;++accepted;
                nvt_contact visible[NVT_SLOTS];
                if(nvt_touch_process_partial(motion.core.get(),out,n,motion.time,visible,NVT_SLOTS)<=0){
                    std::fprintf(stderr,"FAIL qualified_PM_updates_existing_core_ID\n");return 1;}
            }
            if(motion.time-last>=NVT_OBSERVATION_MAX_US)break;
        }
        std::printf("TRACE route%d accepted%llu gap%llu worst_coordinate_units%d reason%d mseQ8%d info%d\n",
                    route,(unsigned long long)accepted,(unsigned long long)maxgap,worst,
                    motion.state->last_reason,motion.state->last_rms_q8,motion.state->last_information);
        require(motion.time>=1000+144*8333 && accepted>=25 && maxgap<100000 && worst<100,
                route==0?"interior_PM_only_1p2s":route==1?"boundary_parallel_PM_only_1p2s":"cross_boundary_PM_only_1p2s");
    }
    Fixture rotated;
    const double vx=1.44,vy=.81,cov=.8,det=vx*vy-cov*cov;
    for(int r=0;r<NVT_ROWS;++r)for(int c=0;c<NVT_COLUMNS;++c) {
        const double dx=c-25.2,dy=r-19.5;
        const double d=1000*std::exp(-.5*(vy*dx*dx-2*cov*dx*dy+vx*dy*dy)/det);
        rotated.matrix[r*NVT_COLUMNS+c]=(int16_t)(10000+std::lround(d));
    }
    nvt_partial_reset(rotated.state.get());
    const auto rotated_profile=touchAstraReportExperimentProfile();
    if(!nvt_partial_configure(rotated.state.get(),&rotated_profile)){std::fprintf(stderr,"FAIL rotated report profile\n");return 1;}
    nvt_partial_seed(rotated.state.get(),rotated.core.get(),rotated.matrix.data(),rotated.time);
    int rn=rotated.measure(2,out);
    if(rn!=1)std::fprintf(stderr,"rotated reason%d mseQ8%d info%d seed%d\n",rotated.state->last_reason,rotated.state->last_rms_q8,rotated.state->last_information,rotated.state->finger[0].valid);
    require(rn==1 && std::abs(out[0].x-axis(25.2,false))<30 &&
            std::abs(out[0].y-axis(19.5,true))<30,
            "correlated_rotated_M_learned_PM_fit");
    Fixture stale;
    stale.core->slots[0].active=0;
    require(stale.measure(2,out)==0,"retired_contact_not_resurrected");
    std::printf("{\"tests\":%u,\"status\":\"PASS_INTEGER_EMPIRICAL_PM_HOST_NOT_HARDWARE\"}\n",tests);
}
