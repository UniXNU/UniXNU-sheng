// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SHENG_TOUCH_ASTRA_PROFILE_H
#define SHENG_TOUCH_ASTRA_PROFILE_H
extern "C" {
#include "NvtPartialObservation.h"
}
// Explicit reproduction profile from Astra's mathematical experiment. These
// are experimental inputs, not a measured NT36532 handset calibration. Keep
// them outside the generic observer so reset/other callers acquire no defaults.
static inline nvt_partial_config touchAstraReportExperimentProfile() {
    nvt_partial_config p={};
    p.sigma_q16=2u*65536u;
    p.huber_delta_q16=6u*65536u;
    p.rms_limit_q16=6u*65536u;
    p.budget_num=2;p.budget_den=25; // exactly 0.08 node
    p.amplitude_min_q16=0;p.amplitude_max_q16=3000*65536;
    p.background_min_q16=-100*65536;p.background_max_q16=100*65536;
    p.search_radius_q8=4u*256u;p.max_evaluations=90;
    // The generic observer also accepts an immutable caller-provided K. This
    // board experiment explicitly selects the optional Gaussian realization;
    // it is not imposed on the generic mathematical contract.
    p.allow_gaussian_learning=1;
    return p;
}
#endif
