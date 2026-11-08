// Independent mathematical/configuration host regression fixtures.
// This is a host test source; no target driver floating point is introduced.
#include "NvtPartialObservation.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

static constexpr int32_t Q16=65536,Q20=1048576;
static nvt_partial_config experiment_profile() {
    nvt_partial_config c={};
    c.sigma_q16=2*Q16;c.huber_delta_q16=6*Q16;c.rms_limit_q16=6*Q16;
    c.budget_num=2;c.budget_den=25;
    c.amplitude_min_q16=0;c.amplitude_max_q16=3000*Q16;
    c.background_min_q16=-100*Q16;c.background_max_q16=100*Q16;
    c.search_radius_q8=4*256;c.max_evaluations=90;c.allow_gaussian_learning=0;
    return c;
}
static void orthogonal(nvt_partial_math_sample *p,int n,int32_t ax,int32_t ay,int32_t residual=0) {
    assert(n==8 || n==16);
    for(int i=0;i<n;++i) {
        p[i].residual_q16=residual;
        p[i].k_q20=i<n/2?0:Q20;
        p[i].jx_q16=(i&1)?-ax:ax;
        p[i].jy_q16=(i&2)?-ay:ay;
    }
}
static nvt_partial_math_result evaluate(nvt_partial_math_sample *p,int n,const nvt_partial_config &c) {
    nvt_partial_math_result r={};
    // Public evaluator success is valid numerical evaluation, not admission.
    assert(nvt_partial_math_evaluate(p,n,&c,&r)==1);
    return r;
}
static void close_q16(int64_t got,double expected,double tolerance=.01) {
    assert(std::fabs(double(got)/Q16-expected)<tolerance);
}
static unsigned prepared_checks;
static void checked(const char *name){++prepared_checks;std::printf("PASS %s\n",name);}
static int generic_kernel(const void *,int col,int row,int32_t *k,int32_t *dx,int32_t *dy) {
    // A caller-owned model used only for binding identity/configuration checks.
    // Constant K is deliberately unobservable; it is not a finger detector.
    (void)col;(void)row;*k=Q20/2;*dx=0;*dy=0;return 1;
}
int main() {
    const auto profile=experiment_profile();
    nvt_partial_math_sample samples[16];
    {
        auto state=std::unique_ptr<nvt_partial_state>(new nvt_partial_state);
        nvt_partial_reset(state.get());assert(state->config_valid==0);
        assert(nvt_partial_configure(state.get(),&profile)==1&&state->config_valid==1);
        assert(state->config.allow_gaussian_learning==0);
        nvt_partial_reset(state.get());assert(state->config_valid==0);
        checked("reset_does_not_silently_select_experiment_or_gaussian_profile");
    }
    {
        nvt_partial_config bad[10];for(auto &c:bad)c=profile;
        bad[0].sigma_q16=0;bad[1].huber_delta_q16=0;bad[2].rms_limit_q16=0;
        bad[3].budget_num=0;bad[4].budget_den=0;bad[5].search_radius_q8=0;
        bad[6].max_evaluations=0;
        bad[7].amplitude_min_q16=bad[7].amplitude_max_q16;
        bad[8].background_min_q16=bad[8].background_max_q16;
        bad[9].amplitude_min_q16=bad[9].amplitude_max_q16+1;
        for(const auto &c:bad) {
            auto state=std::unique_ptr<nvt_partial_state>(new nvt_partial_state);
            nvt_partial_reset(state.get());assert(nvt_partial_configure(state.get(),&c)==0);
            assert(state->config_valid==0);
        }
        checked("invalid_explicit_profile_cannot_enable_PM");
    }
    {
        orthogonal(samples,8,100*Q16,100*Q16);
        auto r=evaluate(samples,8,profile);
        assert(r.numerical_status==0&&r.admitted==1);
        close_q16(r.schur_xx_q16,80000);close_q16(r.schur_yy_q16,80000);
        close_q16(r.schur_xy_q16,0);close_q16(r.lambda_min_q16,80000);
        close_q16(r.lambda_max_q16,80000);
        assert(r.mse_q32==0&&r.cost2_hi==0&&r.cost2_lo==0);
        checked("orthogonal_position_information_is_exact_after_nuisance_projection");
    }
    {
        // Jn=[K,1] has rank1 when K is constant; pinv removes only the
        // constant/background direction. It must not erase real position rank.
        orthogonal(samples,8,100*Q16,100*Q16);
        for(int i=0;i<8;++i)samples[i].k_q20=Q20/2;
        auto r=evaluate(samples,8,profile);
        assert(r.numerical_status==0&&r.admitted);
        close_q16(r.schur_xx_q16,80000);close_q16(r.schur_yy_q16,80000);
        close_q16(r.schur_xy_q16,0);close_q16(r.lambda_min_q16,80000);
        close_q16(r.lambda_max_q16,80000);
        checked("rank_one_nuisance_pseudoinverse_preserves_two_dimensional_position_information");
    }
    {
        orthogonal(samples,8,100*Q16,100*Q16);
        for(int i=0;i<8;++i){samples[i].jx_q16=samples[i].k_q20?100*Q16:0;samples[i].jy_q16=100*Q16;}
        nvt_partial_math_result r={};(void)nvt_partial_math_evaluate(samples,8,&profile,&r);
        assert(!r.admitted);close_q16(r.schur_xx_q16,0);close_q16(r.schur_yy_q16,0);
        checked("amplitude_and_background_only_gradients_do_not_become_position_information");
    }
    {
        // Near-constant K must keep exact nuisance cancellation. Rounding its
        // centered denominator first used to create false positive curvature.
        // At all7nodes Jx=C*(K-.5)/one_Q20_LSB, exactly in span{K,1}.
        auto c=profile;c.budget_num=2;c.budget_den=1;
        for(int i=0;i<7;++i) {
            samples[i]={};samples[i].k_q20=Q20/2;
            samples[i].jy_q16=i<6?((i&1)?-100*Q16:100*Q16):0;
        }
        samples[6].k_q20=Q20/2+1;samples[6].jx_q16=INT32_C(1073741824);
        nvt_partial_math_result r={};(void)nvt_partial_math_evaluate(samples,7,&c,&r);
        assert(!r.admitted);close_q16(r.schur_xx_q16,0,.000001);
        close_q16(r.schur_yy_q16,60000);close_q16(r.schur_xy_q16,0,.000001);
        checked("near_constant_K_keeps_rational_nuisance_moments_until_projection");
    }
    {
        orthogonal(samples,8,100*Q16,100*Q16);
        for(int i=0;i<8;++i)samples[i].jy_q16+=samples[i].jx_q16/2;
        auto r=evaluate(samples,8,profile);
        close_q16(r.schur_xx_q16,80000);close_q16(r.schur_xy_q16,40000);
        close_q16(r.schur_yy_q16,100000);
        close_q16(r.lambda_min_q16,48768.9437438234);
        close_q16(r.lambda_max_q16,131231.0562561766);
        assert(r.admitted);
        checked("correlated_information_uses_actual_eigenvalues_not_det_trace_substitution");
    }
    {
        auto c=profile;c.rms_limit_q16=20*Q16;
        orthogonal(samples,8,100*Q16,100*Q16,12*Q16);
        auto r=evaluate(samples,8,c);
        assert(r.admitted&&r.cost2_hi==0&&r.cost2_lo==(UINT64_C(864)<<32));
        assert(r.mse_q32==(UINT64_C(144)<<32));
        close_q16(r.lambda_min_q16,40000);
        checked("Huber_twice_loss_and_half_weights_at_residual_two_delta");
    }
    {
        auto c=profile;c.rms_limit_q16=20*Q16;
        orthogonal(samples,8,100*Q16,100*Q16,9*Q16);
        auto r=evaluate(samples,8,c);
        assert(r.cost2_hi==0&&r.cost2_lo==(UINT64_C(576)<<32));
        assert(r.mse_q32==(UINT64_C(81)<<32));
        close_q16(r.lambda_min_q16,160000.0/3.0);
        checked("Huber_two_thirds_weight_is_not_coarse_one_sixteenth_quantization");
    }
    {
        // N=16,A=25,sigma=2,budget3/50 gives exact boundary:
        // lambda=10000 and 9*sigma^2/budget^2=10000. Admission is strict <.
        auto c=profile;c.budget_num=3;c.budget_den=50;
        for(int offset=-1;offset<=1;++offset) {
            orthogonal(samples,16,25*Q16+offset,25*Q16+offset);
            auto r=evaluate(samples,16,c);
            assert(r.admitted==(offset>0));
        }
        checked("three_sigma_budget_boundary_rejects_equality_and_preserves_adjacent_cases");
    }
    {
        auto state=std::unique_ptr<nvt_partial_state>(new nvt_partial_state);
        auto core=std::unique_ptr<nvt_touch_core>(new nvt_touch_core);
        nvt_partial_reset(state.get());nvt_touch_reset(core.get());
        assert(nvt_partial_configure(state.get(),&profile)==1);
        core->have_reference=1;auto &slot=core->slots[0];slot.active=1;slot.age=2;
        slot.tracking_id=1;slot.generation=7;slot.born_us=1000;slot.last_observed_us=1000;
        slot.x=nvt_touch_map_edge_axis(nvt_touch_column_center(20)*NVT_SUPER_RESOLUTION,NVT_LONG_AXIS_MAX,0);
        slot.y=nvt_touch_map_edge_axis(nvt_touch_row_center(20)*NVT_SUPER_RESOLUTION,NVT_SHORT_AXIS_MAX,1);
        nvt_partial_template_binding b={generic_kernel,nullptr,20*256,20*256};
        assert(nvt_partial_bind_template(state.get(),core.get(),0,&b,1000)==1);
        assert(state->finger[0].evaluate==generic_kernel&&state->finger[0].generation==7);
        assert(!state->config.allow_gaussian_learning);
        b.evaluate=nullptr;assert(nvt_partial_bind_template(state.get(),core.get(),0,&b,1000)==0);
        checked("caller_supplied_K_binding_is_separate_from_optional_gaussian_learning");
    }
    std::printf("{\"status\":\"PASS_ASTRA_FORMULA_CONFIG_HOST_ONLY\",\"tests\":%u}\n",prepared_checks);
}
