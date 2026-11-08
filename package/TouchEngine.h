// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SHENG_EXPERIMENTAL_TOUCH_ENGINE_H
#define SHENG_EXPERIMENTAL_TOUCH_ENGINE_H
#include <stdint.h>
extern "C" {
#include "NvtProtocol.h"
#include "NvtTouchCore.h"
#include "NvtPartialObservation.h"
#include "NvtCoverage.h"
}
// One owner, no allocation or device I/O. Frame freshness is validated by caller.
// Only typed mutual observations create/update fingers; pen planes stay separate.
struct TouchEngine {
    enum { SpecialP=-2, Rejected=-3 };
    nvt_touch_core *core;
    nvt_partial_state *partial;
    uint64_t streamUs, mutualUs, previousUs;
    unsigned partialFrames, partialPositions, partialRejected, transportCancelled;
    bool isPartial;
    struct CoverageFacts {
        nvt_coverage_policy policy;
        nvt_coverage_contact contact;
        uint64_t observed_epoch;
        unsigned epoch_trusted,baseline_trusted,mode_trusted;
    };
    // Owner-supplied verified facts only. No inferred human speed, contact
    // support radius or controller epoch is silently invented by the engine.
    typedef bool (*CoverageProvider)(void *,const nvt_slot &,uint64_t,CoverageFacts *);
    CoverageProvider coverageProvider;
    void *coverageContext;
    uint64_t sampleToken;
    explicit TouchEngine(nvt_touch_core *value,nvt_partial_state *pm=nullptr):
        core(value),partial(pm),streamUs(0),mutualUs(0),previousUs(0),
        partialFrames(0),partialPositions(0),partialRejected(0),transportCancelled(0),isPartial(false),
        coverageProvider(nullptr),coverageContext(nullptr),sampleToken(0) {}
    // The owner supplied this configuration explicitly. Reset clears templates
    // and packet age while preserving that same reviewed numeric profile.
    void resetPartial() {
        if(!partial)return;
        const bool configured=partial->config_valid!=0;
        const nvt_partial_config saved=partial->config;
        nvt_partial_reset(partial);
        if(configured)(void)nvt_partial_configure(partial,&saved);
    }
    void setCoverageProvider(CoverageProvider provider,void *context) {
        coverageProvider=provider;coverageContext=context;
    }
    int coveredNegative(const uint8_t *raw,uint64_t nowUs,nvt_contact *out,int max) {
        if(!coverageProvider || !core->have_reference)return -1; // UNKNOWN
        int changed=-1;
        const unsigned block=raw[332];
        if(block<1 || block>4)return -1;
        for(int i=0;i<NVT_SLOTS;++i) {
            const nvt_slot slot=core->slots[i];
            if(!slot.active || slot.age<2 || slot.last_observed_us>=nowUs)continue;
            CoverageFacts facts={};
            if(!coverageProvider(coverageContext,slot,nowUs,&facts) ||
               facts.contact.position_us!=slot.last_observed_us)continue;
            nvt_coverage_observation observation={};
            observation.epoch=facts.observed_epoch;observation.sample_token=sampleToken;
            observation.time_us=nowUs;observation.row0=(block-1)*10;observation.row_count=10;
            observation.values_le=raw+4217;observation.value_bytes=1200;
            observation.baseline=core->reference;observation.fresh=1;
            observation.epoch_trusted=facts.epoch_trusted;
            observation.baseline_trusted=facts.baseline_trusted;
            observation.mode_trusted=facts.mode_trusted;
            nvt_coverage_result result={};
            if(nvt_coverage_negative(&facts.policy,&facts.contact,&observation,&result)==
               NVT_COVERAGE_COVERED_NEGATIVE) {
                const int n=nvt_touch_covered_negative(core,slot.tracking_id,slot.generation,nowUs,out,max);
                if(n>=0)changed=n;
            }
        }
        return changed;
    }
    void clock(uint64_t nowUs) {
        if(previousUs && nowUs<previousUs) {
            resetPartial();
            streamUs=mutualUs=0;
        }
        previousUs=nowUs;
    }
    int frame(const uint8_t *raw,size_t rawLength,uint64_t nowUs,int16_t *matrix,nvt_contact *out,int max) {
        isPartial=false;
        if(!core || !raw || rawLength!=NVT_THP_FRAME_LEN || !matrix || !out || !nvt_frame_header_valid(raw))return Rejected;
        const unsigned type=nvt_frame_type(raw);
        if(type!=2 && type!=4)return Rejected;
        clock(nowUs);streamUs=nowUs;++sampleToken;
        if(nvt_frame_is_stylus(raw)) {
            isPartial=true;++partialFrames;
            if(!partial)return SpecialP;
            const unsigned block=raw[332];
            if(block<1 || block>4){++partialRejected;return SpecialP;}
            // Complete supported peaks use the exact ordinary-M candidate path.
            // A clipped peak is excluded here, and may be localized by K below.
            int16_t rows[600];
            for(unsigned j=0;j<600;++j) {
                const unsigned v=(unsigned)raw[4217+2*j]|((unsigned)raw[4218+2*j]<<8);
                rows[j]=static_cast<int16_t>((v&0x8000u)?(int)v-65536:(int)v);
            }
            nvt_region_candidates region={};
            int result=-1;
            if(nvt_touch_extract_region(core,rows,static_cast<int>((block-1)*10),10,&region)>=0)
                result=nvt_touch_process_region(core,&region,nowUs,out,max);
            nvt_contact measured[NVT_SLOTS];
            const int count=nvt_partial_measure(partial,core,raw,rawLength,nowUs,measured,NVT_SLOTS);
            if(count>=0)for(int i=0;i<NVT_SLOTS;++i) {
                const nvt_slot &slot=core->slots[i];
                if(slot.active && slot.age>=2 && slot.tracking_id>=0 && slot.tracking_id<32 &&
                   (partial->presence_ids&(1u<<slot.tracking_id)))
                    nvt_touch_note_presence(core,slot.tracking_id,slot.generation,nowUs);
            }
            if(count>0) {
                const int fitted=nvt_touch_process_partial(core,measured,count,nowUs,out,max);
                if(fitted>=0){result=fitted;partialPositions+=static_cast<unsigned>(count);}
            }
            const int negative=coveredNegative(raw,nowUs,out,max);
            if(negative>=0)result=negative;
            if(result<0)++partialRejected;
            return result>=0?result:SpecialP;
        }
        mutualUs=nowUs;
        nvt_frame_matrix(raw,matrix);
        const int result=nvt_touch_process_timed(core,matrix,type,nowUs,out,max);
        if(partial)nvt_partial_seed(partial,core,matrix,nowUs);
        return result;
    }
    int expire(uint64_t nowUs,nvt_contact *out,int max) {
        clock(nowUs);
        const unsigned long before=core?core->observation_cancelled:0;
        const int result=nvt_touch_expire(core,nowUs,out,max);
        if(core && streamUs && nowUs>=streamUs && nowUs-streamUs>=NVT_OBSERVATION_MAX_US)
            transportCancelled+=static_cast<unsigned>(core->observation_cancelled-before);
        return result;
    }
    uint64_t waitUs(uint64_t nowUs)const {return nvt_touch_wait_us(core,nowUs);}
    unsigned waitMs(uint64_t nowUs,bool scriptedDown)const {
        const uint64_t us=waitUs(nowUs);
        if(!us)return 0;
        if(scriptedDown && us==1)return 100;
        return static_cast<unsigned>((us+999)/1000);
    }
};
#endif
