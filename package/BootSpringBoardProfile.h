/* SPDX-License-Identifier: MIT */
#ifndef SHENG_SPRINGBOARD_PROFILE_H
#define SHENG_SPRINGBOARD_PROFILE_H

/* Used only in an opt-in exact sample of SpringBoard's main thread. The first
 * access on a CPU configures free counters 0 and 1 for EL0 INST_RETIRED and
 * CPU_CYCLES. No overflow interrupt, periodic timer or scheduler hook. */
enum { SbPmuReady = 1, SbPmuAbsent, SbPmuTrap, SbPmuBusy, SbPmuEvents, SbPmuLost };
extern unsigned sheng_pmu_access(unsigned setup, uint32_t values[2]);
extern const char sheng_pmu_begin[], sheng_pmu_end[], sheng_pmu_trapped[];
extern bool (*sheng_pmu_fixup)(arm_saved_state_t *) __attribute__((weak_import));

__asm__(
".text\n.p2align 2\n.globl _sheng_pmu_access\n_sheng_pmu_access:\n"
"  bti c\n"
"_sheng_pmu_begin:\n"
"  cbz w0, 1f\n"
"  mrs x2, ID_AA64DFR0_EL1\n  ubfx x2, x2, #8, #4\n"
"  cbz x2, 2f\n  cmp x2, #15\n  b.eq 2f\n"
"  mrs x3, PMCR_EL0\n  ubfx x2, x3, #11, #5\n  cmp x2, #2\n  b.lo 5f\n"
"  mrs x2, PMCEID0_EL0\n  tbz x2, #8, 5f\n  tbz x2, #17, 5f\n"
"  mrs x2, PMCNTENSET_EL0\n  cbnz x2, 4f\n"
"  mrs x2, PMINTENSET_EL1\n  tst x2, #3\n  b.ne 4f\n"
"  mov x4, #0x80000000\n  orr x5, x4, #8\n  msr PMEVTYPER0_EL0, x5\n"
"  add x5, x4, #17\n  msr PMEVTYPER1_EL0, x5\n  isb\n"
"  mov x2, #3\n  msr PMCNTENSET_EL0, x2\n"
"  bic x3, x3, #6\n  orr x3, x3, #1\n  msr PMCR_EL0, x3\n  isb\n"
"1: mrs x2, PMCR_EL0\n  tbz x2, #0, 6f\n"
"  mrs x2, PMCNTENSET_EL0\n  and x2, x2, #3\n  cmp x2, #3\n  b.ne 6f\n"
"  mov x4, #0x80000000\n  add x5, x4, #8\n  mrs x2, PMEVTYPER0_EL0\n  cmp x2, x5\n  b.ne 6f\n"
"  add x5, x4, #17\n  mrs x2, PMEVTYPER1_EL0\n  cmp x2, x5\n  b.ne 6f\n"
"  mrs x2, PMEVCNTR0_EL0\n  mrs x3, PMEVCNTR1_EL0\n"
"_sheng_pmu_end:\n  stp w2, w3, [x1]\n  mov w0, #1\n  ret\n"
"2: mov w0, #2\n  ret\n"
"_sheng_pmu_trapped:\n  mov w0, #3\n  ret\n"
"4: mov w0, #4\n  ret\n"
"5: mov w0, #5\n  ret\n"
"6: mov w0, #6\n  ret\n");

static bool sb_pmu_fixup(arm_saved_state_t *state) {
    uintptr_t pc = (uintptr_t)get_saved_state_pc(state);
    if (pc < (uintptr_t)sheng_pmu_begin || pc >= (uintptr_t)sheng_pmu_end)
        return false;
    set_saved_state_pc(state, (register_t)(uintptr_t)sheng_pmu_trapped);
    return true;
}

static unsigned sb_pmu_read(unsigned cpu, uint32_t values[2]) {
    /* Only the owning CPU touches the entry, with interrupts masked. */
    static unsigned status[8];
    if (cpu >= 8) return SbPmuAbsent;
    unsigned prior = status[cpu];
    if (!prior || prior == SbPmuReady || prior == SbPmuLost)
        status[cpu] = sheng_pmu_access(prior != SbPmuReady, values);
    return status[cpu];
}
#endif
