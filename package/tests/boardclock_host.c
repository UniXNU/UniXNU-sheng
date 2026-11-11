// Host test of BoardClock.h against a software model of the SPMI PMIC arbiter (version 7 layout): the model runs each transfer at
// the moment its command word is written, which BoardClock.h lets a test take over through BC_COMMAND.
// cc -std=c11 -Wall -Wextra -o boardclock_host boardclock_host.c && ./boardclock_host
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void model(volatile uint32_t *channel, uint32_t word);
#define BC_COMMAND(channel, word) model((channel), (word))
#include "../BoardClock.h"

enum {APIDS = 40, RTC_APID = 11, SDAM_APID = 7, SDAM_DECOY = 5};
static volatile uint32_t core[0x3000 / 4], cnfg[0x4000 / 4];
static volatile uint32_t obs[BR_OBS_APID_STRIDE * APIDS / 4];   // execution environment 0's observer channels
static volatile uint32_t chn[BR_CHNL_APID_STRIDE / 4];          // the SDAM's write channel
static uint8_t rtc[256], sdam[256];
static unsigned reads, writes, last_write_bytes;
static int deny_writes, drop_writes, fail_reads, silent;
// The counter values successive four-byte counter reads see; when the script is used up the register bytes are read.
static uint32_t script[8];
static unsigned script_length, script_at;

static void put32(uint8_t *at, uint32_t value) {for (unsigned i = 0; i < 4; ++i) at[i] = (uint8_t)(value >> (8 * i));}
static uint32_t get32(const uint8_t *at) {return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;}

static void model(volatile uint32_t *channel, uint32_t word) {
  const uint8_t op = (uint8_t)(word >> 27), addr = (uint8_t)(word >> 4);
  const unsigned bytes = (word & 7) + 1;
  channel[BR_CMD / 4] = word;
  channel[BR_STATUS / 4] = 0;
  if (silent) return;                                           // the arbiter never answers
  if (channel == chn) {
    assert(op == BR_OP_EXT_WRITEL && bytes <= 4);
    ++writes;
    last_write_bytes = bytes;
    if (deny_writes) {channel[BR_STATUS / 4] = BR_STATUS_DONE | 1u << 2; return;}
    if (!drop_writes)
      for (unsigned i = 0; i < bytes; ++i) sdam[addr + i] = (uint8_t)(channel[BR_WDATA0 / 4] >> (8 * i));
    channel[BR_STATUS / 4] = BR_STATUS_DONE;
    return;
  }
  const unsigned apid = (unsigned)((channel - obs) * 4 / BR_OBS_APID_STRIDE);
  assert(channel >= obs && apid < APIDS && op == BR_OP_EXT_READL && bytes <= 4);
  ++reads;
  if (fail_reads) {channel[BR_STATUS / 4] = BR_STATUS_DONE | 1u << 1; return;}
  const uint8_t *memory = apid == RTC_APID ? rtc : sdam;
  assert(apid == RTC_APID || apid == SDAM_APID);
  uint32_t data = 0;
  if (apid == RTC_APID && addr == BC_RTC_COUNTER && bytes == 4 && script_at < script_length) data = script[script_at++];
  else for (unsigned i = 0; i < bytes; ++i) data |= (uint32_t)memory[addr + i] << (8 * i);
  channel[BR_RDATA0 / 4] = data;
  channel[BR_STATUS / 4] = BR_STATUS_DONE;
}

static struct br_arbiter arbiter = {core, cnfg, 0};

static void setup(void) {
  memset((void *)core, 0, sizeof core); memset((void *)cnfg, 0, sizeof cnfg); memset((void *)obs, 0, sizeof obs); memset((void *)chn, 0, sizeof chn);
  core[0] = 0x70000001u; core[BR_FEATURES / 4] = APIDS;
  // The SDAM twice: APID 5 owned by execution environment 1, APID 7 by this one. The clock once, owned by another one (reads only).
  core[(BR_APID_MAP + 4 * SDAM_DECOY) / 4] = BC_SDAM_PPID << 8 | 1; cnfg[SDAM_DECOY] = 1;
  core[(BR_APID_MAP + 4 * SDAM_APID) / 4] = BC_SDAM_PPID << 8 | 1; cnfg[SDAM_APID] = 0;
  core[(BR_APID_MAP + 4 * RTC_APID) / 4] = BC_RTC_PPID << 8 | 1; cnfg[RTC_APID] = 4;
  memset(rtc, 0, sizeof rtc); memset(sdam, 0, sizeof sdam);
  rtc[BC_RTC_CONTROL] = 0x80; put32(rtc + BC_RTC_COUNTER, 8147);
  sdam[0x48] = 0x04; sdam[BC_OFFSET_CELL - 1] = 0xaa; sdam[BC_OFFSET_CELL + 4] = 0xbb;
  reads = writes = last_write_bytes = 0; deny_writes = drop_writes = fail_reads = silent = 0;
  script_length = script_at = 0;
}
static void counter_script(const uint32_t *values, unsigned count) {
  for (unsigned i = 0; i < count; ++i) script[i] = values[i];
  script_length = count; script_at = 0;
}

static void test_find_and_read(void) {
  setup();
  int owner = -1;
  assert(bc_find(&arbiter, APIDS, BC_SDAM_PPID, &owner) == SDAM_APID && owner == 0);       // this side's mapping, not the first
  assert(bc_find(&arbiter, APIDS, BC_RTC_PPID, &owner) == RTC_APID && owner == 4);
  owner = 9;
  assert(bc_find(&arbiter, APIDS, 0x0ba, &owner) == -1 && owner == 9);
  assert(bc_find(&arbiter, SDAM_APID, BC_SDAM_PPID, &owner) == SDAM_DECOY && owner == 1);  // only the other side's is in range

  uint32_t value = 0;
  assert(bc_read(obs, &arbiter, SDAM_APID, 0x48, 1, &value) == BC_OK && value == 0x04);
  put32(sdam + BC_OFFSET_CELL, 0x6a1b2c3d);
  assert(bc_read(obs, &arbiter, SDAM_APID, BC_OFFSET_CELL, 4, &value) == BC_OK && value == 0x6a1b2c3d);  // first byte lowest
  assert(bc_read(obs, &arbiter, SDAM_APID, BC_OFFSET_CELL, 2, &value) == BC_OK && value == 0x2c3d);
  assert(bc_read_stable(obs, &arbiter, SDAM_APID, BC_OFFSET_CELL, 4, &value) == BC_OK && value == 0x6a1b2c3d);
  fail_reads = 1; value = 7;
  assert(bc_read(obs, &arbiter, SDAM_APID, 0x48, 1, &value) == 2 && value == 7);
  assert(bc_read_stable(obs, &arbiter, SDAM_APID, 0x48, 1, &value) == 2 && value == 7);
  fail_reads = 0; silent = 1;
  assert(bc_read(obs, &arbiter, SDAM_APID, 0x48, 1, &value) == 1 && value == 7);
}

static void test_counter(void) {
  uint32_t seconds = 0;
  setup();
  assert(bc_counter(obs, &arbiter, RTC_APID, &seconds) == BC_OK && seconds == 8147);
  rtc[BC_RTC_CONTROL] = 0x00; seconds = 1;
  assert(bc_counter(obs, &arbiter, RTC_APID, &seconds) == BC_STOPPED && seconds == 1);
  rtc[BC_RTC_CONTROL] = 0x80;

  const uint32_t tick[] = {0x0000ffff, 0x00010000};                        // a second passes between the two reads
  counter_script(tick, 2);
  assert(bc_counter(obs, &arbiter, RTC_APID, &seconds) == BC_OK && seconds == 0x00010000);
  const uint32_t torn[] = {0x0001ffff, 0x00010000, 0x00010000};            // a read across the carry, then the true value twice
  counter_script(torn, 3);
  assert(bc_counter(obs, &arbiter, RTC_APID, &seconds) == BC_OK && seconds == 0x00010000 && script_at == 3);
  const uint32_t foreign[] = {8147, 0x80, 8147, 8148};                     // another user's transfer answered the second read
  counter_script(foreign, 4);
  assert(bc_counter(obs, &arbiter, RTC_APID, &seconds) == BC_OK && seconds == 8148 && script_at == 4);
  const uint32_t never[] = {10, 500, 9000, 70000, 70002, 70004};
  counter_script(never, 6); seconds = 3;
  assert(bc_counter(obs, &arbiter, RTC_APID, &seconds) == BC_UNSTABLE && seconds == 3 && script_at == BC_READ_TRIES);
  const uint32_t backwards[] = {8148, 8147, 8147};                         // one second back is not "one apart"
  counter_script(backwards, 3);
  assert(bc_counter(obs, &arbiter, RTC_APID, &seconds) == BC_OK && seconds == 8147 && script_at == 3);
  fail_reads = 1;
  assert(bc_counter(obs, &arbiter, RTC_APID, &seconds) == 2);
}

static void test_arithmetic(void) {
  uint64_t seconds = 5;
  uint32_t offset = 5;
  assert(bc_time(8147, 0, &seconds) == BC_EMPTY && seconds == 5);
  assert(bc_time(8147, 1794298877u, &seconds) == BC_OK && seconds == 1794307024u);
  assert(bc_time(0xfffffff0u, 0x20, &seconds) == BC_RANGE && seconds == 1794307024u);
  assert(bc_time(0xfffffff0u, 0x0f, &seconds) == BC_OK && seconds == 0xffffffffu);

  assert(bc_offset(1794307024u, 8147, &offset) == BC_OK && offset == 1794298877u);
  assert(bc_offset(8147, 8147, &offset) == BC_RANGE && offset == 1794298877u);      // would be 0, which means empty
  assert(bc_offset(8146, 8147, &offset) == BC_RANGE);
  assert(bc_offset(0x100000000ull, 8147, &offset) == BC_RANGE);
  assert(bc_offset(8148, 8147, &offset) == BC_OK && offset == 1);

  // The cell: empty, an offset, or something else.
  const uint64_t now = 1794307024u;                                                 // 2026-11-10
  assert(bc_cell(8147, 0, UINT32_MAX) == BC_EMPTY);
  assert(bc_cell(8147, 1794298877u, UINT32_MAX) == BC_OK);
  assert(bc_cell(8147, 1794298877u, now + BC_SET_BACK) == BC_OK);
  assert(bc_cell(100, 1794298877u, now + BC_SET_BACK) == BC_OK);                    // the counter restarted: earlier, still a time
  assert(bc_cell(8147, 1, UINT32_MAX) == BC_FOREIGN);                               // a flag or a small count
  assert(bc_cell(8147, 0xffffffffu, UINT32_MAX) == BC_FOREIGN);                     // erased bytes
  assert(bc_cell(8147, BC_EARLIEST - 8147, UINT32_MAX) == BC_OK && bc_cell(8147, BC_EARLIEST - 8148, UINT32_MAX) == BC_FOREIGN);
  assert(bc_cell(8147, 1794298877u + BC_SET_BACK, now + BC_SET_BACK) == BC_OK);     // up to a day after the time being set
  assert(bc_cell(8147, 1794298877u + BC_SET_BACK + 1, now + BC_SET_BACK) == BC_FOREIGN);
  assert(bc_cell(8147, 3000000000u, now + BC_SET_BACK) == BC_FOREIGN);              // a time decades ahead
}

static void test_store(void) {
  uint32_t back = 0;
  setup();
  assert(bc_store(obs, chn, &arbiter, SDAM_DECOY, 1, 1794298877u, &back) == BC_NOT_OWNER && writes == 0 && reads == 0);
  assert(bc_store(obs, chn, &arbiter, SDAM_APID, 0, 0, &back) == BC_EMPTY && writes == 0 && reads == 0);

  assert(bc_store(obs, chn, &arbiter, SDAM_APID, 0, 1794298877u, &back) == BC_OK && back == 1794298877u);
  assert(writes == 1 && last_write_bytes == 4);                                     // the cell changes in one transfer
  assert(get32(sdam + BC_OFFSET_CELL) == 1794298877u);
  assert(sdam[BC_OFFSET_CELL] == 0xfd && sdam[BC_OFFSET_CELL + 3] == 0x6a);         // 0x6af2d3fd, little endian
  assert(sdam[BC_OFFSET_CELL - 1] == 0xaa && sdam[BC_OFFSET_CELL + 4] == 0xbb && sdam[0x48] == 0x04);  // the neighbours stay

  deny_writes = 1; back = 0;
  assert(bc_store(obs, chn, &arbiter, SDAM_APID, 0, 42, &back) == 2 && back == 0 && get32(sdam + BC_OFFSET_CELL) == 1794298877u);
  deny_writes = 0; drop_writes = 1;
  assert(bc_store(obs, chn, &arbiter, SDAM_APID, 0, 42, &back) == BC_MISMATCH && back == 1794298877u);
  drop_writes = 0; silent = 1;
  assert(bc_store(obs, chn, &arbiter, SDAM_APID, 0, 42, &back) == 1);
}

// The system sets the time, the offset is stored, the tablet restarts (the counter runs on, or starts again), the next boot reads it.
static void test_round_trip(void) {
  setup();
  uint32_t counter = 0, offset = 0, cell = 0, back = 0;
  uint64_t seconds = 0;
  int owner = -1;
  const int apid = bc_find(&arbiter, APIDS, BC_SDAM_PPID, &owner);
  const uint64_t set = 1794307024u;
  assert(bc_counter(obs, &arbiter, RTC_APID, &counter) == BC_OK);
  assert(bc_read_stable(obs, &arbiter, apid, BC_OFFSET_CELL, 4, &cell) == BC_OK && bc_cell(counter, cell, set + BC_SET_BACK) == BC_EMPTY);
  assert(bc_offset(set, counter, &offset) == BC_OK);
  assert(bc_store(obs, chn, &arbiter, apid, owner, offset, &back) == BC_OK);

  put32(rtc + BC_RTC_COUNTER, 8147 + 3600);                                         // an hour later, after a warm restart
  assert(bc_counter(obs, &arbiter, RTC_APID, &counter) == BC_OK);
  assert(bc_read_stable(obs, &arbiter, apid, BC_OFFSET_CELL, 4, &cell) == BC_OK && bc_cell(counter, cell, UINT32_MAX) == BC_OK);
  assert(bc_time(counter, cell, &seconds) == BC_OK && seconds == set + 3600);

  put32(rtc + BC_RTC_COUNTER, 20);                                                  // the counter started again: early, never late
  assert(bc_counter(obs, &arbiter, RTC_APID, &counter) == BC_OK);
  assert(bc_cell(counter, cell, UINT32_MAX) == BC_OK && bc_time(counter, cell, &seconds) == BC_OK && seconds == set - 8147 + 20);
  // the next time setting replaces the stale offset
  assert(bc_cell(counter, cell, set + 7200 + BC_SET_BACK) == BC_OK && bc_offset(set + 7200, counter, &offset) == BC_OK);
  assert(bc_store(obs, chn, &arbiter, apid, owner, offset, &back) == BC_OK && bc_time(counter, back, &seconds) == BC_OK && seconds == set + 7200);
}

static void test_row(void) {
  char row[400];
  const uint32_t values[16] = {0xf, 7, 600 | 4u << 16, 565, BC_EMPTY, 8147, 0, 0, 2, 1794307024u, 8200, 1794298824u, BC_OK, 1794298824u, 1,
                               (uint32_t)-53};
  const unsigned n = bc_row(row, sizeof row, values);
  assert(n == strlen(row));
  assert(!strcmp(row, "[board clock] mode=7 st=f rtc=600/4 sdam=565/0 boot rc=4 counter=8147 cell=0 time=0 | set n=2 secs=1794307024 counter=8200 "
                      "offset=1794298824 err=-53 | store rc=0 cell=1794298824 n=1"));
  char small[16];
  memset(small, 'x', sizeof small);
  assert(bc_row(small, 12, values) == 11 && small[11] == 0 && small[12] == 'x' && !strncmp(small, "[board cloc", 11));
}

int main(void) {
  test_find_and_read();
  test_counter();
  test_arithmetic();
  test_store();
  test_round_trip();
  test_row();
  printf("boardclock_host: ok\n");
  return 0;
}
