// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../KeyboardService.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>

static unsigned checks;
#define CHECK(x) do {if (!(x)) {std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1);} ++checks;} while (0)
struct Mock {
    uint64_t clock = 1;
    uint32_t edges = 0, reads = 0, powers = 0, applies = 0, wakes = 0, releases = 0, reloads = 0, consumedCount = 0;
    bool on = true, readOK = true, powerOK = true, applyOK = true, recoverOK = true, flood = false;
    KeyboardService *service = nullptr;
    uint64_t now() const {return clock;}
    uint32_t irqs() const {return edges;}
    bool attached() const {return on;}
    bool read() {++reads; ++clock; if (flood) ++edges; return readOK;}
    void consumed(uint32_t seen) {consumedCount = seen;}
    bool power() {++powers; ++clock; return powerOK;}
    bool apply() {++applies; ++clock; return applyOK;}
    void wake() {++wakes; clock += 35;}
    void release() {++releases;}
    bool recover() {++reloads; clock += 600; if (recoverOK) service->seen = edges; return recoverOK;}
};
int main() {
    KeyboardService s;
    Mock io; io.service = &s;
    s.keepalive = 12000;
    // A failed read does not consume its only edge; retry without any new interrupt succeeds.
    io.edges = 1; io.readOK = false;
    s.step(io);
    CHECK(!s.seen && io.reads == 1 && s.retryDue > io.clock && !s.recovering);
    CHECK(s.deadline(io.clock, io.edges) == s.retryDue);
    s.step(io); CHECK(io.reads == 1);
    io.clock = s.retryDue; io.readOK = true; s.step(io);
    CHECK(s.seen == 1 && io.reads == 2 && !s.retryDue && !s.readFailures);
    CHECK(s.deadline(io.clock, io.edges) > io.clock); // no expired retry deadline spinning the worker

    // An input flood yields after a bounded batch, and the due power command still runs.
    io.flood = true; ++io.edges; io.clock = s.keepalive;
    const unsigned before = io.reads; s.step(io);
    CHECK(io.reads == before + KeyboardService::drainBudget && io.powers == 1);
    CHECK(s.deadline(io.clock, io.edges) == io.clock);
    io.flood = false; s.step(io); CHECK(s.seen == io.edges);

    // A quiet but healthy controller is never reset, even across many idle intervals.
    for (unsigned i = 0; i < 100; ++i) {io.clock = s.keepalive; s.step(io); CHECK(!s.recovering && !io.reloads);}
    CHECK(io.powers == 101 && io.reads >= 100);

    // Failed keepalive: GPIO wake, bounded retries, then one reload after a delay.
    io.powerOK = false; io.clock = s.keepalive;
    s.step(io); CHECK(io.wakes == 1 && s.controlFailures == 1 && !s.recovering);
    io.clock = s.retryDue; s.step(io); CHECK(s.controlFailures == 2);
    io.clock = s.retryDue; s.step(io); CHECK(s.recovering && io.reloads == 0);
    const uint64_t firstRecovery = s.recoveryDue;
    s.step(io); CHECK(!io.reloads);
    io.clock = firstRecovery; io.recoverOK = false; s.step(io);
    CHECK(io.reloads == 1 && s.recoveryDue == io.clock + 2000 && s.recovering);
    for (unsigned i = 0; i < 6; ++i) {io.clock = s.recoveryDue; s.step(io);}
    CHECK(s.recoveryDue == io.clock + 32000); // bounded retry frequency during a persistent failure
    io.clock = s.recoveryDue; io.recoverOK = io.powerOK = true; s.step(io);
    CHECK(!s.recovering && !s.retryDue && !s.controlFailures && !s.readFailures && io.applies == 1);

    // Repeated read errors also reload, and do not pretend that the pending frame was read.
    io.readOK = false; ++io.edges;
    const uint32_t seen = s.seen;
    s.step(io); io.clock = s.retryDue; s.step(io); io.clock = s.retryDue; s.step(io);
    CHECK(s.recovering && s.seen == seen && s.readFailures == 3);
    io.readOK = true; io.clock = s.recoveryDue; s.step(io);
    CHECK(!s.recovering && s.seen == io.edges);

    // A failed state update remains due and recovers rather than silently dropping touchpad/LED configuration.
    s.statePending = true; io.applyOK = false;
    s.step(io); CHECK(s.statePending && s.controlFailures == 1);
    io.clock = s.retryDue; s.step(io); io.clock = s.retryDue; s.step(io);
    CHECK(s.recovering && s.statePending);
    io.applyOK = true; io.clock = s.recoveryDue; s.step(io); CHECK(!s.statePending && !s.recovering);

    // Detached covers still get a periodic frame poll, allowing a lost attach edge to be observed without power writes.
    io.on = false; const uint32_t power = io.powers, reads = io.reads;
    io.clock = s.keepalive; s.step(io);
    CHECK(io.powers == power && io.reads == reads + 1);

    // Counter wrap remains a single pending event.
    s.seen = UINT32_MAX; io.edges = 0; s.step(io); CHECK(!s.seen && !io.consumedCount);
    std::printf("PASS keyboard service: %u checks\n", checks);
}
