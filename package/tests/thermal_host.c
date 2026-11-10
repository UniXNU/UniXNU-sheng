// Host test of ThermalProbe.h: the status word arithmetic, the sensor table, the limit statistics and the boot log rows.
// cc -std=c11 -Wall -Wextra -o thermal_host thermal_host.c && ./thermal_host
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ThermalProbe.h"

static void test_status(void) {
  assert(thermal_status_mc(0) == 0);
  assert(thermal_status_mc(253) == 25300);
  assert(thermal_status_mc(950 | THERMAL_STATUS_VALID | 4u << THERMAL_STATUS_FLAGS_SHIFT) == 95000);   // other fields do not leak in
  assert(thermal_status_mc(0xfff) == -100);                                                             // -0.1 degree
  assert(thermal_status_mc(0x800) == -204800);
  assert(thermal_status_mc(0x7ff) == 204700);

  int32_t mc = 1;
  const uint32_t valid_first[3] = {300 | THERMAL_STATUS_VALID, 0, 0};
  assert(thermal_pick(valid_first, 1, &mc) && mc == 30000);
  const uint32_t valid_last[3] = {310, 320, 330 | THERMAL_STATUS_VALID};
  assert(thermal_pick(valid_last, 3, &mc) && mc == 33000);
  const uint32_t agree_first[3] = {410, 410, 420};                 // none valid: the first two agree
  assert(thermal_pick(agree_first, 3, &mc) && mc == 41000);
  const uint32_t agree_last[3] = {410, 420, 420};
  assert(thermal_pick(agree_last, 3, &mc) && mc == 42000);
  const uint32_t none[3] = {410, 420, 430};
  mc = 7;
  assert(!thermal_pick(none, 3, &mc) && mc == 7);
  assert(!thermal_pick(none, 1, &mc));                             // one read without the valid bit is no reading

  assert(!thermal_sensor_enabled(0, 0));
  assert(!thermal_sensor_enabled(0xfffffffe, 3));                  // controller off
  assert(thermal_sensor_enabled(THERMAL_CTRL_ENABLE | 1u << 3, 0) && !thermal_sensor_enabled(THERMAL_CTRL_ENABLE | 1u << 3, 1));
  assert(thermal_sensor_enabled(THERMAL_CTRL_ENABLE | 1u << 18, 15) && !thermal_sensor_enabled(THERMAL_CTRL_ENABLE | 1u << 19, 15));
}

static void test_table(void) {
  unsigned count[THERMAL_GROUPS] = {0};
  for (unsigned c = 0; c < THERMAL_CONTROLLERS; ++c)
    for (unsigned s = 0; s < THERMAL_SENSORS; ++s) {
      assert(thermal_group_of[c][s] < THERMAL_GROUPS);
      count[thermal_group_of[c][s]]++;
    }
  // sm8550.dtsi: cpu0..2 on the second controller, cpu3..6 with two sensors each and cpu7 with three on the first, cpuss0..3, gpuss-0..7,
  // modem0..3; aoss0..2, video, mem and camera0..1 are the rest with a zone.
  assert(count[THERMAL_GROUP_CPU_D0] == 3 && count[THERMAL_GROUP_CPU_D1] == 8 && count[THERMAL_GROUP_CPU_D2] == 3);
  assert(count[THERMAL_GROUP_CPUSS] == 4 && count[THERMAL_GROUP_GPU] == 8 && count[THERMAL_GROUP_MODEM] == 4);
  assert(count[THERMAL_GROUP_OTHER] == 7 && count[THERMAL_GROUP_UNNAMED] == 11);
  assert(thermal_group_of[1][1] == THERMAL_GROUP_CPU_D0 && thermal_group_of[1][3] == THERMAL_GROUP_CPU_D0);
  assert(thermal_group_of[0][5] == THERMAL_GROUP_CPU_D1 && thermal_group_of[0][12] == THERMAL_GROUP_CPU_D1);
  assert(thermal_group_of[0][13] == THERMAL_GROUP_CPU_D2 && thermal_group_of[0][15] == THERMAL_GROUP_CPU_D2);
  assert(thermal_group_of[2][1] == THERMAL_GROUP_GPU && thermal_group_of[2][8] == THERMAL_GROUP_GPU);
  assert(thermal_tm_base[2] == 0x0c273000 && thermal_srot_base[0] == 0x0c222000);

  int32_t mc[THERMAL_CONTROLLERS][THERMAL_SENSORS], out[THERMAL_GROUPS];
  for (unsigned c = 0; c < THERMAL_CONTROLLERS; ++c)
    for (unsigned s = 0; s < THERMAL_SENSORS; ++s) mc[c][s] = THERMAL_NONE;
  thermal_groups(mc, out);
  for (unsigned g = 0; g < THERMAL_GROUPS; ++g) assert(out[g] == THERMAL_NONE);
  mc[1][1] = 40000; mc[1][2] = 45500; mc[1][3] = 41000;          // cpu0..2
  mc[0][7] = 91200; mc[0][8] = 88000;                            // cpu4 top and bottom
  mc[0][14] = -500;                                              // a single negative reading still counts
  mc[2][3] = 60100;
  thermal_groups(mc, out);
  assert(out[THERMAL_GROUP_CPU_D0] == 45500 && out[THERMAL_GROUP_CPU_D1] == 91200 && out[THERMAL_GROUP_CPU_D2] == -500);
  assert(out[THERMAL_GROUP_GPU] == 60100 && out[THERMAL_GROUP_CPUSS] == THERMAL_NONE && out[THERMAL_GROUP_MODEM] == THERMAL_NONE);
}

static void test_limit(void) {
  struct thermal_limit l;
  thermal_limit_init(&l);
  assert(thermal_limit_limited_mean(&l) == THERMAL_NONE && thermal_limit_free_mean(&l) == THERMAL_NONE);
  assert(!thermal_limit_add(&l, 899, 1000000, 2803200, 90000) && l.busy == 0);        // not busy enough
  assert(!thermal_limit_add(&l, 950, 0, 2803200, 90000) && l.busy == 0);              // cluster off
  assert(!thermal_limit_add(&l, 950, 1000000, 0, 90000) && l.busy == 0);              // no vote
  assert(!thermal_limit_add(&l, 950, 1000000, 2803200, THERMAL_NONE) && l.busy == 0); // no temperature
  assert(!thermal_limit_add(&l, 900, 2803200, 2803200, 70000) && l.busy == 1 && l.limited == 0);
  assert(!thermal_limit_add(&l, 950, 2242560, 2803200, 80000));                        // exactly 80 % of the vote is not limited
  assert(thermal_limit_add(&l, 950, 2242559, 2803200, 93000));
  assert(thermal_limit_add(&l, 1000, 1171200, 2803200, 96000));
  assert(thermal_limit_add(&l, 1000, 1497600, 2803200, 94500));
  assert(l.busy == 5 && l.limited == 3);
  assert(l.limited_min == 93000 && l.limited_max == 96000 && thermal_limit_limited_mean(&l) == 94500);
  assert(l.free_max == 80000 && thermal_limit_free_mean(&l) == 75000);
  assert(l.lowest_khz == 1171200);

  assert(thermal_lut_khz(0) == 300000);                       // source 0: GPLL0 / 2
  assert(thermal_lut_khz(1u << 30 | 146) == 2803200);         // 146 x 19.2 MHz
  assert(thermal_lut_khz(2u << 30 | 1u << 16 | 16) == 307200);
}

static void test_rows(void) {
  char row[400];
  int32_t now[THERMAL_GROUPS] = {45500, 91200, -500, THERMAL_NONE, 60100, 38000, 41900, THERMAL_NONE};
  int32_t peak[THERMAL_GROUPS] = {47000, 95300, 2000, THERMAL_NONE, 71000, 38000, 43000, THERMAL_NONE};
  unsigned n = thermal_row_groups(row, sizeof row, 12, now, peak);
  assert(n == strlen(row));
  assert(!strcmp(row, "[thermal] n=12 cpu0-2 45.5/47.0 cpu3-6 91.2/95.3 cpu7 -0.5/2.0 cpuss -/- gpu 60.1/71.0 modem 38.0/38.0 other 41.9/43.0 unnamed -/-"));

  const uint32_t lmh[3][8] = {{2016000, 2016000, 1, 1, 0, 0, 0, 0}, {1171200, 2803200, 1, 1, 0x10, 0x4, 0x4, 1}, {0, 0, 0, 0, 0, 0, 0, 2}};
  n = thermal_row_lmh(row, sizeof row, lmh);
  assert(n == strlen(row));
  assert(!strcmp(row, "[thermal lmh] d0 2016/2016 MHz en1 ctl1 irq0 0/0/0 | d1 1171/2803 MHz en1 ctl1 irq1 10/4/4 | d2 0/0 MHz en0 ctl0 irq2 0/0/0"));

  const int32_t limit[3][8] = {{100, 0, THERMAL_NONE, THERMAL_NONE, THERMAL_NONE, 61000, 70500, 0},
                               {220, 31, 93000, 94500, 96000, 75000, 92900, 1171200},
                               {0, 0, THERMAL_NONE, THERMAL_NONE, THERMAL_NONE, THERMAL_NONE, THERMAL_NONE, 0}};
  n = thermal_row_limit(row, sizeof row, limit);
  assert(n == strlen(row));
  assert(!strcmp(row, "[thermal limit] d0 100/0 at -/-/- free 61.0/70.5 low 0 MHz | d1 220/31 at 93.0/94.5/96.0 free 75.0/92.9 low 1171 MHz |"
                      " d2 0/0 at -/-/- free -/- low 0 MHz"));

  int32_t mc[THERMAL_SENSORS];
  for (unsigned s = 0; s < THERMAL_SENSORS; ++s) mc[s] = 30000 + 100 * (int32_t)s;
  mc[4] = THERMAL_NONE; mc[15] = -1500;
  n = thermal_row_raw(row, sizeof row, 1, 0x20040000, 0x7fff9, 0x10020, mc);
  assert(n == strlen(row));
  assert(!strcmp(row, "[thermal raw] t1 v=20040000 ctl=7fff9 over=10020: 300 301 302 303 - 305 306 307 308 309 310 311 312 313 314 -15"));

  // A buffer that is too small is filled and terminated, never overrun.
  char small[24];
  memset(small, 'x', sizeof small);
  n = thermal_row_groups(small, sizeof small - 4, 12, now, peak);
  assert(n == sizeof small - 5 && small[n] == 0 && small[sizeof small - 4] == 'x');
  assert(!strncmp(small, "[thermal] n=12 cpu0", n));
}

int main(void) {
  test_status();
  test_table();
  test_limit();
  test_rows();
  printf("thermal_host: ok\n");
  return 0;
}
