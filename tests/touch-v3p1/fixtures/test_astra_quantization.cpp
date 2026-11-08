// SPDX-License-Identifier: Apache-2.0
// Prepared source fixtures only. V3.1 has not compiled or executed this file.
#include "NvtPartialObservation.h"
#include "TouchAstraProfile.h"
#include <cassert>
#include <cstdio>
#include <climits>
static void balanced(nvt_partial_math_sample *s,int n,int amplitude) {
    for(int i=0;i<n;++i) {
        s[i]={};s[i].k_q20=(i/4)&1?1048576:0;
        s[i].jx_q16=(i&1)?-amplitude:amplitude;
        s[i].jy_q16=(i&2)?-amplitude:amplitude;
    }
}
int main() {
    auto c=touchAstraReportExperimentProfile();
    nvt_partial_math_sample s[16];nvt_partial_math_result r={};
    c.budget_num=3;c.budget_den=50;
    balanced(s,16,25*65536);
    assert(nvt_partial_math_evaluate(s,16,&c,&r));
    assert(!r.admitted&&r.numerical_status==3); // interval overlaps strict equality
    balanced(s,16,25*65536+1);
    assert(nvt_partial_math_evaluate(s,16,&c,&r));assert(r.admitted&&r.numerical_status==0);
    balanced(s,16,25*65536-1);
    assert(nvt_partial_math_evaluate(s,16,&c,&r));assert(!r.admitted&&r.numerical_status==0);
    c=touchAstraReportExperimentProfile();balanced(s,16,100*65536);
    for(auto &v:s)v.residual_q16=6*65536;
    assert(nvt_partial_math_evaluate(s,16,&c,&r));assert(!r.admitted&&r.numerical_status==0);
    assert(r.mse_q32==(UINT64_C(36)<<32)); // exact RMS equality is rejected
    balanced(s,16,100*65536);for(auto &v:s)v.jy_q16=0;
    assert(nvt_partial_math_evaluate(s,16,&c,&r));assert(!r.admitted&&r.one_axis);
    c.huber_delta_q16=1;c.rms_limit_q16=536870912;
    balanced(s,16,100*65536);s[0].residual_q16=INT_MAX;
    assert(nvt_partial_math_evaluate(s,16,&c,&r));
    assert(!r.admitted&&r.numerical_status==3); // weight rounds to0; no false precision
    std::puts("PASS prepared_quantization_boundary_fixtures_if_executed");
}
