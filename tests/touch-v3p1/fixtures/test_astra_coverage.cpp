// SPDX-License-Identifier: Apache-2.0
// V3.1 source-only fixtures: prepared, not compiled or executed in this session.
extern "C" {
#include "NvtCoverage.h"
}
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
static int16_t baseline[NVT_NODES];
static uint8_t values[NVT_NODES*2];
static nvt_coverage_policy policy;
static nvt_coverage_contact contact;
static nvt_coverage_observation observation;
static nvt_coverage_result result;
static void put(unsigned row,unsigned col,int value) {
    assert(row>=observation.row0 && row<observation.row0+observation.row_count);
    const size_t index=((size_t)(row-observation.row0)*NVT_COLUMNS+col)*2;
    const uint16_t raw=(uint16_t)value;values[index]=(uint8_t)raw;values[index+1]=(uint8_t)(raw>>8);
}
static void init(unsigned row0=10,unsigned count=10) {
    policy={};policy.motion_bounds_verified=1;
    contact={};contact.epoch=7;contact.position_us=1000;
    contact.row_q16=15*65536;contact.col_q16=30*65536;
    contact.support_radius_q16=65536;contact.position_support_bounds_verified=1;
    observation={};observation.epoch=7;observation.sample_token=77;observation.time_us=2000;
    observation.row0=row0;observation.row_count=count;observation.values_le=values;
    observation.value_bytes=(size_t)count*NVT_COLUMNS*2;observation.baseline=baseline;
    observation.fresh=observation.epoch_trusted=observation.baseline_trusted=observation.mode_trusted=1;
    for(auto &b:baseline)b=10000;
    for(unsigned r=row0;r<row0+count;++r)for(unsigned c=0;c<NVT_COLUMNS;++c)put(r,c,10000);
    memset(&result,0,sizeof(result));
}
static int run() {return nvt_coverage_negative(&policy,&contact,&observation,&result);}
static void pass(const char *name) {std::printf("PASS %s\n",name);}
int main() {
    unsigned tests=0;
    init();assert(nvt_coverage_negative(nullptr,&contact,&observation,&result)==NVT_COVERAGE_UNKNOWN);
    assert(result.reason==NVT_COVERAGE_NO_POLICY);pass("NULL_policy_never_invents_motion_bound");++tests;
    init();assert(run()==NVT_COVERAGE_COVERED_NEGATIVE);
    assert(result.first_row==14 && result.last_row==16 && result.first_col==29 && result.last_col==31);
    pass("explicit_stationary_bound_complete_quiet_support");++tests;
    init();contact.row_q16=19*65536;policy.vmax_q16_per_s=10*65536;
    observation.time_us=51000;assert(run()==NVT_COVERAGE_UNKNOWN);
    assert(result.last_row==21 && result.reason==NVT_COVERAGE_INSUFFICIENT);
    pass("motion_growth_prevents_old_position_false_UP");++tests;
    init();policy.motion_bounds_verified=0;assert(run()==NVT_COVERAGE_UNKNOWN);
    assert(result.first_row==0 && result.last_row==39);
    pass("unverified_motion_U_is_whole_panel_stripe_unknown");++tests;
    init(0,40);policy.motion_bounds_verified=0;assert(run()==NVT_COVERAGE_COVERED_NEGATIVE);
    assert(result.first_col==0 && result.last_col==59);
    pass("explicit_whole_panel_policy_needs_actual_whole_panel");++tests;
    init();put(15,30,10000+NVT_NOISE_THRESHOLD);assert(run()==NVT_COVERAGE_SIGNAL_PRESENT);
    pass("existing_positive_noise_floor_blocks_negative");++tests;
    init();put(15,30,10000-NVT_NOISE_THRESHOLD);assert(run()==NVT_COVERAGE_SIGNAL_PRESENT);
    pass("negative_interference_is_not_quiet_background");++tests;
    init();put(10,0,11000);assert(run()==NVT_COVERAGE_COVERED_NEGATIVE);
    pass("other_region_signal_does_not_end_or_rescue_this_ID");++tests;
    init();observation.epoch=8;assert(run()==NVT_COVERAGE_UNKNOWN);
    assert(result.reason==NVT_COVERAGE_EPOCH_MISMATCH);pass("different_epoch_cannot_prove_UP");++tests;
    init();observation.time_us=999;assert(run()==NVT_COVERAGE_UNKNOWN);
    assert(result.reason==NVT_COVERAGE_STALE);pass("older_observation_cannot_prove_UP");++tests;
    init();observation.time_us=101000;assert(run()==NVT_COVERAGE_UNKNOWN);
    assert(result.reason==NVT_COVERAGE_DEADLINE);pass("100ms_deadline_belongs_to_Core_cancel");++tests;
    init();observation.fresh=0;assert(run()==NVT_COVERAGE_UNKNOWN);
    pass("cached_or_duplicate_packet_not_negative_evidence");++tests;
    init();observation.baseline_trusted=0;assert(run()==NVT_COVERAGE_UNKNOWN);
    pass("untrusted_baseline_blocks_negative");++tests;
    init();observation.mode_trusted=0;assert(run()==NVT_COVERAGE_UNKNOWN);
    pass("unverified_mode_blocks_negative");++tests;
    init();observation.epoch_trusted=0;assert(run()==NVT_COVERAGE_UNKNOWN);
    pass("unknown_epoch_blocks_negative");++tests;
    init();--observation.value_bytes;assert(run()==NVT_COVERAGE_UNKNOWN);
    assert(result.reason==NVT_COVERAGE_INVALID_INPUT);pass("truncated_actual_rows_not_completed_with_cache");++tests;
    init();policy.vmax_q16_per_s=UINT32_MAX;policy.amax_q16_per_s2=UINT32_MAX;
    contact.uncertainty_radius_q16=UINT32_MAX;contact.support_radius_q16=UINT32_MAX;
    observation.time_us=100999;assert(run()==NVT_COVERAGE_UNKNOWN);
    assert(result.first_row==0 && result.last_row==39);
    pass("explicit_uint32_extrema_do_not_overflow_radius");++tests;
    init();policy.vmax_q16_per_s=10*65536;policy.amax_q16_per_s2=100*65536;
    contact.uncertainty_radius_q16=65536/4;observation.time_us=51000;
    assert(run()==NVT_COVERAGE_COVERED_NEGATIVE);
    // .25 initial + 1 support + 10*.05 + .5*100*.05² = 1.875node.
    assert(result.radius_q16==122880 && result.first_row==13 && result.last_row==17);
    pass("velocity_acceleration_and_support_use_seconds_consistently");++tests;
    init();policy.amax_q16_per_s2=1;observation.time_us=1001;
    assert(run()==NVT_COVERAGE_COVERED_NEGATIVE && result.radius_q16==65537);
    pass("sub_LSB_acceleration_rounds_outward_not_inward");++tests;
    init(0,10);contact.row_q16=contact.col_q16=0;
    assert(run()==NVT_COVERAGE_COVERED_NEGATIVE && result.first_row==0 && result.last_row==1);
    pass("physical_grid_edges_clipped_without_unsigned_wrap");++tests;
    init();contact.position_support_bounds_verified=0;
    assert(run()==NVT_COVERAGE_UNKNOWN && result.last_row==39);
    pass("statistical_or_unspecified_E_support_not_physical_guarantee");++tests;
    init();for(auto &b:baseline)b=-32000;
    for(unsigned r=10;r<20;++r)for(unsigned c=0;c<NVT_COLUMNS;++c)put(r,c,-32000);
    put(15,30,-31900);assert(run()==NVT_COVERAGE_SIGNAL_PRESENT);
    pass("signed_LE_ADC_decoding_and_baseline_difference");++tests;
    std::printf("{\"tests\":%u,\"status\":\"SYNTHETIC_COVERAGE_CONTRACT_NOT_HARDWARE\"}\n",tests);
}
