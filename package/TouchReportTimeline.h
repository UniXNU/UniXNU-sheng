// SPDX-License-Identifier: BSD-2-Clause-Patent
// Finger report ordering for Astra algorithm V3. Integer-only, bounded, no allocation.
// Keep this object off a kernel thread's stack: caller owns persistent storage.
#ifndef SHENG_TOUCH_REPORT_TIMELINE_H
#define SHENG_TOUCH_REPORT_TIMELINE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
extern "C" {
#include "NvtTouchCore.h"
}

struct TouchReportTimeline {
    enum { Capacity=32 };
    typedef bool (*Callback)(void *,unsigned,const nvt_contact *,uint64_t);
    struct Point {
        nvt_contact contact;
        uint64_t generation,bornUs;
        unsigned pending;
    };
    struct Frame {
        uint64_t timeUs,timeTicks;
        unsigned count;
        Point point[NVT_SLOTS];
    };
    Frame frames[Capacity];
    Point emitted[NVT_SLOTS];
    unsigned head,used,emittedCount;
    uint64_t blockedGeneration[NVT_SLOTS];
    uint64_t lastInputUs,lastInputTicks,lastOutputTicks;
    unsigned clockValid,outputClockValid;
    uint64_t fed,submitted,pendingDropped,capacityFaults,clockFaults,submitFailures;

    // POD with explicit reset: valid after IOMalloc + reset(), no constructor.
    void reset() {memset(this,0,sizeof(*this));}
    static bool same(const Point &p,const nvt_slot &s) {
        return s.active && p.contact.tracking_id==s.tracking_id && p.generation==s.generation;
    }
    static uint64_t age(uint64_t now,uint64_t born) {return now>=born ? now-born : 0;}
    Frame &at(unsigned i) {return frames[(head+i)%Capacity];}
    const Frame &at(unsigned i) const {return frames[(head+i)%Capacity];}

    void discardPending(unsigned slot,uint64_t generation) {
        if(slot<NVT_SLOTS)blockedGeneration[slot]=generation;
        bool removed=false;
        for(unsigned f=0;f<used;++f) {
            Frame &frame=at(f);unsigned n=0;
            for(unsigned j=0;j<frame.count;++j) {
                const Point &p=frame.point[j];
                if(p.pending && p.contact.slot==(int)slot && p.generation==generation) {removed=true;continue;}
                frame.point[n++]=p;
            }
            frame.count=n;
        }
        if(removed)++pendingDropped;
    }
    void expirePending(uint64_t nowUs) {
        // Restart each scan after removal; at most 12 independent pending IDs.
        for(;;) {
            bool again=false;
            for(unsigned f=0;f<used && !again;++f) {
                const Frame &frame=at(f);
                for(unsigned j=0;j<frame.count;++j) {
                    const Point &p=frame.point[j];
                    if(p.pending && age(nowUs,p.bornUs)>=NVT_PENDING_MAX_US) {
                        const unsigned slot=(unsigned)p.contact.slot;const uint64_t gen=p.generation;
                        discardPending(slot,gen);again=true;break;
                    }
                }
            }
            if(!again)break;
        }
    }
    // Capture every ordinary result, including an age1 hidden candidate. PM
    // updates use the same path only when an actual accepted measurement exists.
    // false preserves all queued history. Caller must handle overflow/clock
    // faults by an explicit input release + Core/timeline reset, never overwrite.
    bool feed(uint64_t nowUs,uint64_t nowTicks,const nvt_touch_core &core) {
        if(clockValid && (nowUs<lastInputUs || nowTicks<lastInputTicks)) {++clockFaults;return false;}
        if(used==Capacity) {++capacityFaults;return false;}
        // Reconcile historical pending points against this generation's status.
        // Confirmations modify only the pending bit, never historical positions.
        for(unsigned s=0;s<NVT_SLOTS;++s) {
            bool have=false;uint64_t gen=0,born=0;
            for(unsigned f=0;f<used && !have;++f) {
                const Frame &frame=at(f);
                for(unsigned j=0;j<frame.count;++j)if(frame.point[j].pending && frame.point[j].contact.slot==(int)s) {
                    gen=frame.point[j].generation;born=frame.point[j].bornUs;have=true;break;
                }
            }
            if(!have)continue;
            const nvt_slot &slot=core.slots[s];
            const bool current=slot.active && slot.generation==gen;
            if(!current || age(nowUs,born)>=NVT_PENDING_MAX_US) {discardPending(s,gen);continue;}
            if(slot.age>=2) {
                for(unsigned f=0;f<used;++f) {
                    Frame &frame=at(f);
                    for(unsigned j=0;j<frame.count;++j)if(frame.point[j].pending && frame.point[j].contact.slot==(int)s && frame.point[j].generation==gen)
                        frame.point[j].pending=0;
                }
            }
        }
        Frame &frame=at(used);frame.timeUs=nowUs;frame.timeTicks=nowTicks;frame.count=0;
        for(unsigned s=0;s<NVT_SLOTS;++s) {
            const nvt_slot &slot=core.slots[s];
            if(!slot.active || !slot.generation || blockedGeneration[s]==slot.generation)continue;
            if(slot.age<2 && age(nowUs,slot.born_us)>=NVT_PENDING_MAX_US) {
                blockedGeneration[s]=slot.generation;continue;
            }
            Point &p=frame.point[frame.count++];
            memset(&p,0,sizeof(p));
            p.contact.slot=(int)s;p.contact.tracking_id=slot.tracking_id;
            p.contact.x=slot.age<2 ? slot.origin_x : slot.rx;
            p.contact.y=slot.age<2 ? slot.origin_y : slot.ry;
            p.generation=slot.generation;p.bornUs=slot.born_us;p.pending=slot.age<2;
        }
        ++used;++fed;clockValid=1;lastInputUs=nowUs;lastInputTicks=nowTicks;
        return true;
    }
    bool deliver(Callback callback,void *context,const Point *points,unsigned count,uint64_t ticks) {
        if(outputClockValid && ticks<lastOutputTicks) {++clockFaults;return false;}
        nvt_contact contacts[NVT_SLOTS];
        for(unsigned j=0;j<count;++j)contacts[j]=points[j].contact;
        if(!callback || !callback(context,count,contacts,ticks)) {++submitFailures;return false;}
        emittedCount=count;
        for(unsigned j=0;j<count;++j)emitted[j]=points[j];
        outputClockValid=1;lastOutputTicks=ticks;++submitted;return true;
    }
    // The producer's existing timer must call flush even when no new M arrives.
    // A later candidate can block only frames containing it, not older history.
    unsigned flush(uint64_t nowUs,Callback callback,void *context) {
        expirePending(nowUs);unsigned released=0;
        while(used) {
            const Frame &frame=at(0);
            if(nowUs<frame.timeUs)break;
            bool pending=false;
            for(unsigned j=0;j<frame.count;++j)if(frame.point[j].pending) {pending=true;break;}
            if(pending)break;
            // HID transmits IDs, not generations. If Core reuses an ID without
            // a captured empty frame, force a tip-up snapshot before its new
            // generation, preserving other IDs at their last submitted values.
            Point kept[NVT_SLOTS];unsigned keep=0;bool reused=false;
            for(unsigned j=0;j<emittedCount;++j) {
                bool retire=false;
                for(unsigned k=0;k<frame.count;++k)
                    if(emitted[j].contact.tracking_id==frame.point[k].contact.tracking_id &&
                       emitted[j].generation!=frame.point[k].generation) {retire=true;break;}
                if(retire)reused=true;else kept[keep++]=emitted[j];
            }
            if(reused && !deliver(callback,context,kept,keep,frame.timeTicks))break;
            if(!deliver(callback,context,frame.point,frame.count,frame.timeTicks))break;
            head=(head+1)%Capacity;--used;++released;
        }
        return released;
    }
    // Microseconds until the oldest unresolved deadline; 1 means ready/overdue.
    uint64_t waitUs(uint64_t nowUs) const {
        if(!used)return 0;
        const Frame &frame=at(0);uint64_t wait=0;
        for(unsigned j=0;j<frame.count;++j)if(frame.point[j].pending) {
            const uint64_t elapsed=age(nowUs,frame.point[j].bornUs);
            const uint64_t left=elapsed>=NVT_PENDING_MAX_US ? 1 : NVT_PENDING_MAX_US-elapsed;
            if(!wait || left<wait)wait=left;
        }
        return wait ? wait : 1;
    }
};
static_assert(sizeof(TouchReportTimeline)<32768,"Timeline must stay bounded and off-stack");
#endif
