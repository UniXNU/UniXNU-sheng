// SPDX-License-Identifier: BSD-2-Clause-Patent
// Prepared, NOT executed in the V3.1 source-only session.
// Valid synthetic math-API inputs, not a claim these values occurred on hardware.
// Jx is exactly an affine function of K, so nuisance projection must remove all
// X information even though the pre-projection X energy is enormous.
extern "C" {
#include "NvtPartialObservation.h"
}
#include "TouchAstraProfile.h"
#include <cassert>
#include <cstdio>
#include <cstdint>
int main() {
    nvt_partial_math_sample samples[10]={};
    const int32_t large=INT32_C(1073741824);
    for(int i=0;i<10;++i) {
        samples[i].residual_q16=0;
        samples[i].k_q20=i<3 ? 524288 : 524289;
        samples[i].jx_q16=i<3 ? -large : large;
        samples[i].jy_q16=i==0 ? large : i==1 ? -large : 0;
    }
    // Independent algebraic certificate: Jx = 2^31*K + one constant.
    const int64_t intercept=(int64_t)samples[0].jx_q16-
        INT64_C(2147483648)*samples[0].k_q20;
    for(const auto &sample:samples)
        assert((int64_t)sample.jx_q16-INT64_C(2147483648)*sample.k_q20==intercept);
    const nvt_partial_config config=touchAstraReportExperimentProfile();
    assert(config.sigma_q16==2u*65536u);
    nvt_partial_math_result result={};
    assert(nvt_partial_math_evaluate(samples,10,&config,&result)==1);
    // Output Schur matrix is Q16 in ADC²/node². Y is orthogonal to K and 1:
    // sum_j (Jy_j/65536)^2 = 2*(2^14)^2 = 536870912.
    const int64_t schur_yy_q16=INT64_C(536870912)*65536;
    assert(result.schur_xx_q16==0 && result.schur_xy_q16==0);
    assert(result.schur_yy_q16==schur_yy_q16);
    // Current result contract names the eigenvalues of unwhitened Schur H.
    // The report's information I=H/sigma² would have major eigenvalue
    // 134217728 at sigma=2; admission must use that explicit noise scale.
    assert(result.lambda_min_q16==0);
    assert(result.lambda_max_q16==(uint64_t)schur_yy_q16);
    assert(!result.admitted);
    std::puts("PASS nearly_constant_K_affine_Jx_has_no_2D_information");
    std::puts("{\"tests\":1,\"status\":\"PREPARED_SYNTHETIC_NUISANCE_COUNTEREXAMPLE\"}");
}
