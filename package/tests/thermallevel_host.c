// Host test of ThermalLevel.h: the level mapping with its hysteresis, unreadable and stale readings, the service's answers, the
// client counts and the boot log row.
// cc -std=c11 -Wall -Wextra -o thermallevel_host thermallevel_host.c && ./thermallevel_host
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../ThermalLevel.h"

static void groups_set(int32_t groups[THERMAL_GROUPS], int32_t d0, int32_t d1, int32_t d2, int32_t gpu) {
  for (unsigned g = 0; g < THERMAL_GROUPS; ++g) groups[g] = THERMAL_NONE;
  groups[THERMAL_GROUP_CPU_D0] = d0;
  groups[THERMAL_GROUP_CPU_D1] = d1;
  groups[THERMAL_GROUP_CPU_D2] = d2;
  groups[THERMAL_GROUP_GPU] = gpu;
}

// One step with a sample count that moves, as the probe's does.
static unsigned step(struct thermal_level_state *s, int32_t d0, int32_t d1, int32_t d2, int32_t gpu) {
  static uint32_t samples;
  int32_t groups[THERMAL_GROUPS];
  groups_set(groups, d0, d1, d2, gpu);
  return thermal_level_step(s, groups, 1, ++samples);
}

static void test_input(void) {
  const struct thermal_level_trip *cpu = thermal_level_cpu_trips, *gpu = thermal_level_gpu_trips;
  // rising: a trip is crossed at its temperature
  assert(thermal_level_input(0, 89900, cpu) == 0);
  assert(thermal_level_input(0, 90000, cpu) == 1);
  assert(thermal_level_input(0, 94900, cpu) == 1);
  assert(thermal_level_input(0, 95000, cpu) == 2);
  assert(thermal_level_input(0, 109900, cpu) == 2);
  assert(thermal_level_input(0, 110000, cpu) == 3);
  assert(thermal_level_input(0, 204700, cpu) == 3);              // the sensor field's maximum: never above trapping
  assert(thermal_level_input(0, 84900, gpu) == 0 && thermal_level_input(0, 85000, gpu) == 1);
  assert(thermal_level_input(0, 90000, gpu) == 2 && thermal_level_input(0, 110000, gpu) == 3);
  // falling: a crossed trip holds until the reading is below it by the hysteresis
  assert(thermal_level_input(1, 88000, cpu) == 1 && thermal_level_input(1, 87900, cpu) == 0);
  assert(thermal_level_input(2, 93000, cpu) == 2 && thermal_level_input(2, 92900, cpu) == 1);
  assert(thermal_level_input(2, 87900, cpu) == 0);               // through two trips at once
  assert(thermal_level_input(3, 109000, cpu) == 3 && thermal_level_input(3, 108900, cpu) == 2);
  assert(thermal_level_input(1, 84000, gpu) == 1 && thermal_level_input(1, 83900, gpu) == 0);
  assert(thermal_level_input(2, 89000, gpu) == 2 && thermal_level_input(2, 88900, gpu) == 1);
  // a held lower trip does not make a higher one easier
  assert(thermal_level_input(1, 94900, cpu) == 1 && thermal_level_input(1, 95000, cpu) == 2);
  // no reading, whatever was crossed
  for (unsigned crossed = 0; crossed <= 3; ++crossed) assert(thermal_level_input(crossed, THERMAL_NONE, cpu) == 0);
  assert(thermal_level_input(0, -204800, cpu) == 0);
}

static void test_levels(void) {
  struct thermal_level_state s;
  thermal_level_init(&s);
  assert(s.level == THERMAL_LEVEL_NOMINAL && s.mc[0] == THERMAL_NONE);
  assert(step(&s, 45000, 50000, 52000, 40000) == THERMAL_LEVEL_NOMINAL && s.changes == 0);
  // the hottest input decides
  assert(step(&s, 45000, 90000, 52000, 40000) == THERMAL_LEVEL_MODERATE && s.changes == 1);
  assert(step(&s, 45000, 89000, 52000, 40000) == THERMAL_LEVEL_MODERATE);       // inside the hysteresis
  assert(step(&s, 45000, 89000, 96000, 40000) == THERMAL_LEVEL_HEAVY);
  assert(step(&s, 45000, 87000, 93500, 40000) == THERMAL_LEVEL_HEAVY);          // cpu7 holds heavy, cpu3-6 has left moderate
  assert(s.crossed[1] == 0 && s.crossed[2] == 2);
  assert(step(&s, 45000, 87000, 92000, 40000) == THERMAL_LEVEL_MODERATE);
  assert(step(&s, 45000, 87000, 87000, 40000) == THERMAL_LEVEL_NOMINAL);
  // the GPU has its own trips: 85 is nominal for a core and moderate for the GPU
  assert(step(&s, 85000, 85000, 85000, 84900) == THERMAL_LEVEL_NOMINAL);
  assert(step(&s, 85000, 85000, 85000, 85000) == THERMAL_LEVEL_MODERATE);
  assert(step(&s, 85000, 85000, 85000, 90500) == THERMAL_LEVEL_HEAVY);
  assert(step(&s, 85000, 85000, 85000, 111000) == THERMAL_LEVEL_TRAPPING);
  assert(step(&s, 85000, 85000, 85000, 60000) == THERMAL_LEVEL_NOMINAL);
  assert(s.highest == THERMAL_LEVEL_TRAPPING && s.level == THERMAL_LEVEL_NOMINAL);
  // never sleeping, whatever the inputs
  assert(step(&s, 204700, 204700, 204700, 204700) == THERMAL_LEVEL_TRAPPING);
  // a sensor that stops reading while it holds a level lets go of it at once and does not come back holding it
  assert(step(&s, 45000, 45000, 96000, 40000) == THERMAL_LEVEL_HEAVY);
  assert(step(&s, 45000, 45000, THERMAL_NONE, 40000) == THERMAL_LEVEL_NOMINAL);
  assert(s.mc[2] == THERMAL_NONE && s.crossed[2] == 0);
  assert(step(&s, 45000, 45000, 93500, 40000) == THERMAL_LEVEL_MODERATE);       // 93.5 without history is one trip, not two
  // no readings at all
  assert(step(&s, THERMAL_NONE, THERMAL_NONE, THERMAL_NONE, THERMAL_NONE) == THERMAL_LEVEL_NOMINAL);
  // groups that are not inputs change nothing
  int32_t groups[THERMAL_GROUPS];
  groups_set(groups, 45000, 45000, 45000, 45000);
  groups[THERMAL_GROUP_CPUSS] = groups[THERMAL_GROUP_MODEM] = groups[THERMAL_GROUP_OTHER] = groups[THERMAL_GROUP_UNNAMED] = 120000;
  assert(thermal_level_step(&s, groups, 1, 100000) == THERMAL_LEVEL_NOMINAL);
}

static void test_stale(void) {
  struct thermal_level_state s;
  int32_t groups[THERMAL_GROUPS];
  thermal_level_init(&s);
  groups_set(groups, 96000, 50000, 50000, 40000);
  // the probe never sampled: its values are not used
  assert(thermal_level_step(&s, groups, 0, 0) == THERMAL_LEVEL_NOMINAL && s.stale == 1 && s.mc[0] == THERMAL_NONE);
  assert(thermal_level_step(&s, groups, 1, 1) == THERMAL_LEVEL_HEAVY);
  // the count stands still: two more rounds use the readings (another sampler may have been at work), then they are old
  assert(thermal_level_step(&s, groups, 1, 1) == THERMAL_LEVEL_HEAVY);
  assert(thermal_level_step(&s, groups, 1, 1) == THERMAL_LEVEL_HEAVY);
  assert(thermal_level_step(&s, groups, 1, 1) == THERMAL_LEVEL_NOMINAL && s.stale == 2);
  assert(thermal_level_step(&s, groups, 1, 1) == THERMAL_LEVEL_NOMINAL && s.stale == 3);
  assert(s.crossed[0] == 0 && s.mc[0] == THERMAL_NONE);
  // the count moves again
  groups_set(groups, 93500, 50000, 50000, 40000);
  assert(thermal_level_step(&s, groups, 1, 2) == THERMAL_LEVEL_MODERATE && s.mc[0] == 93500);
  assert(s.highest == THERMAL_LEVEL_HEAVY && s.changes == 3 && s.rounds == 7);
}

static void test_wire(void) {
  uint32_t keys[THERMAL_LEVEL_SLOTS], values[THERMAL_LEVEL_SLOTS];
  assert(sizeof keys == THERMAL_LEVEL_ARRAY_BYTES && THERMAL_LEVEL_ARRAY_BYTES == 0x3c);
  memset(keys, 0xff, sizeof keys);
  memset(values, 0xff, sizeof values);
  thermal_level_fill_keys(keys);
  thermal_level_fill_values(values, THERMAL_LEVEL_HEAVY);
  // the daemon makes the key's name from the code, top byte first, and takes the value at the code's index
  char name[5] = {(char)(keys[0] >> 24), (char)(keys[0] >> 16), (char)(keys[0] >> 8), (char)keys[0], 0};
  assert(!strcmp(name, "mTPL"));
  assert(values[0] == 2);
  for (unsigned i = THERMAL_LEVEL_KEYS; i < THERMAL_LEVEL_SLOTS; ++i) assert(keys[i] == 0 && values[i] == 0);
  // every level this file can report is one the daemon passes on: not its "no key" (-1) and not its "read failed" (-12700)
  for (unsigned level = 0; level <= THERMAL_LEVEL_TRAPPING; ++level) {
    thermal_level_fill_values(values, level);
    assert((int32_t)values[0] != -1 && (int32_t)values[0] != -12700 && values[0] <= THERMAL_LEVEL_SLEEPING);
  }
}

static void test_use(void) {
  struct thermal_level_use u;
  memset(&u, 0, sizeof u);
  for (unsigned i = 0; i < 3; ++i) thermal_level_use_read(&u, 70);
  thermal_level_use_read(&u, 412);
  thermal_level_use_read(&u, 70);
  assert(u.client[0].pid == 70 && u.client[0].reads == 4 && u.client[1].pid == 412 && u.client[1].reads == 1);
  for (int pid = 500; pid < 504; ++pid) thermal_level_use_read(&u, pid);
  assert(u.client[3].pid == 501 && u.other_reads == 2);

  uint8_t request[THERMAL_LEVEL_WRITE_BYTES];
  memset(request, 0, sizeof request);
  request[0] = 2;
  memcpy(request + 8, "z3eT", 4);             // 'Te3z' as a 32-bit code in memory
  memcpy(request + 24, "z5sT", 4);
  assert(thermal_level_use_write(&u, request) == 1);
  assert(u.writes == 1 && u.write_keys == 2 && u.written[0] == 0x5473357a && u.written[1] == 0x5465337a);
  assert(thermal_level_use_write(&u, request) == 1 && u.write_keys == 4 && u.written[2] == 0);   // the same keys are listed once
  request[0] = THERMAL_LEVEL_WRITE_PAIRS + 1;
  assert(thermal_level_use_write(&u, request) == 0 && u.bad_writes == 1 && u.writes == 3 && u.write_keys == 4);
  request[0] = 0;
  assert(thermal_level_use_write(&u, request) == 1 && u.write_keys == 4);
}

static void test_row(void) {
  struct thermal_level_state s;
  struct thermal_level_use u;
  char row[400];
  thermal_level_init(&s);
  memset(&u, 0, sizeof u);
  unsigned n = thermal_level_row(row, sizeof row, -1, &s, &u);
  assert(n == strlen(row));
  assert(!strcmp(row, "[thermal level] st=-1 level=0 nominal from cpu0-2 - cpu3-6 - cpu7 - gpu - | n=0 high=0 chg=0 stale=0 | open 0/0/0 | "
                      "reads +0 | count 0 list 0 | write 0/0/0 | other 0/0"));
  step(&s, 61300, 96000, 84000, 66500);
  step(&s, 61300, 93100, 84000, 66500);
  u.opens = 1;
  u.refused = 2;
  u.counts = u.lists = 1;
  for (unsigned i = 0; i < 412; ++i) thermal_level_use_read(&u, 70);
  uint8_t request[THERMAL_LEVEL_WRITE_BYTES] = {1};
  memcpy(request + 8, "z3eT", 4);
  thermal_level_use_write(&u, request);
  u.unsupported = 3;
  u.last_unsupported = 5;
  n = thermal_level_row(row, sizeof row, 1, &s, &u);
  assert(n == strlen(row));
  assert(!strcmp(row, "[thermal level] st=1 level=2 heavy from cpu0-2 61.3 cpu3-6 93.1 cpu7 84.0 gpu 66.5 | n=2 high=2 chg=1 stale=0 | "
                      "open 1/2/0 | reads 70:412 +0 | count 1 list 1 | write 1/1/0 Te3z | other 3/5"));
  // a full table and four written keys fit the boot log's row
  for (int pid = 100000; pid < 100003; ++pid) for (unsigned i = 0; i < 100000; ++i) thermal_level_use_read(&u, pid);
  for (unsigned i = 0; i < THERMAL_LEVEL_WRITTEN; ++i) u.written[i] = 0x54703078 + i;
  s.mc[0] = s.mc[1] = s.mc[2] = s.mc[3] = -204800;
  n = thermal_level_row(row, sizeof row, -12, &s, &u);
  assert(n == strlen(row) && n < sizeof row - 1);
  // a code that does not print
  char key[8];
  n = thermal_level_put_key(key, 0, sizeof key, 0x6d54000a);
  assert(n == 4 && !strcmp(key, "mT??"));
  // a short buffer is terminated inside its size
  char small[24];
  memset(small, 'x', sizeof small);
  n = thermal_level_row(small, sizeof small - 4, 1, &s, &u);
  assert(n == sizeof small - 5 && small[n] == 0 && small[sizeof small - 4] == 'x');
}

int main(void) {
  test_input();
  test_levels();
  test_stale();
  test_wire();
  test_use();
  test_row();
  printf("thermallevel_host: ok\n");
  return 0;
}
