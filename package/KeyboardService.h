// SPDX-License-Identifier: BSD-2-Clause-Patent
// Scheduling policy for the WN8030 worker. Millisecond deadlines and injected I/O keep error recovery testable without hardware.
#ifndef SHENG_KEYBOARD_SERVICE_H
#define SHENG_KEYBOARD_SERVICE_H
#include <stdint.h>

struct KeyboardService {
    static constexpr uint32_t keepaliveMs = 12000, retryMs = 100, drainBudget = 16;
    uint32_t seen = 0, readFailures = 0, controlFailures = 0, recoveryTries = 0;
    uint64_t keepalive = 0, retryDue = 0, recoveryDue = 0;
    bool statePending = false, recovering = false;

    template <typename IO> void step(IO &io) {
        if (recovering) {
            if (io.now() < recoveryDue) return;
            if (!io.recover()) {
                if (recoveryTries < 5) ++recoveryTries;
                recoveryDue = io.now() + (1000u << recoveryTries);
                return;
            }
            recovering = false;
            recoveryTries = readFailures = controlFailures = 0;
            retryDue = 0;
            statePending = io.attached();
            keepalive = io.now();
        }
        if (retryDue && io.now() < retryDue) return;
        retryDue = 0;
        // A failed read keeps its edge pending. A bounded batch lets the caller service hall and LED changes under load.
        for (unsigned n = 0; n < drainBudget && seen != io.irqs(); ++n) {
            if (!io.read()) {++readFailures; retryDue = io.now() + retryMs; break;}
            io.consumed(++seen);
            readFailures = 0;
        }
        if (!retryDue && io.now() >= keepalive) {
            if (!io.attached() || io.power()) {
                controlFailures = 0;
                keepalive = io.now() + keepaliveMs;
                // Poll once per health interval as insurance against a missed rising edge, including an attach at boot.
                if (seen == io.irqs()) {
                    if (!io.read()) {++readFailures; retryDue = keepalive = io.now() + retryMs;}
                    else readFailures = 0;
                }
            } else {
                if (++controlFailures == 1) {io.release(); io.wake(); statePending = io.attached();}
                retryDue = keepalive = io.now() + retryMs;
            }
        }
        if (!retryDue && statePending) {
            if (io.apply()) {statePending = false; controlFailures = 0;}
            else {++controlFailures; retryDue = io.now() + retryMs;}
        }
        if (readFailures >= 3 || controlFailures >= 3) {
            io.release();
            recovering = true;
            recoveryDue = io.now() + 1000;
        }
    }

    uint64_t deadline(uint64_t now, uint32_t irqs) const {
        if (recovering) return recoveryDue;
        if (retryDue) return retryDue;
        if (seen != irqs || statePending) return now;
        return keepalive;
    }
};
#endif
