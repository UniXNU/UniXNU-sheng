// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of kern.unixnu_wifi_log's text (package/WifiLogExport.h): the WiFi module's ring of lines, oldest first.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../WifiLogExport.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

enum {Lines = 8};
static char ring[Lines][SHENG_WIFI_LOG_LINE];

static void write_lines(uint32_t from, uint32_t to)
{
    for (uint32_t i = from; i < to; ++i) snprintf(ring[i % Lines], sizeof ring[0], "%u 6 line %u", i, i);
}

static void empty_ring_gives_nothing(void)
{
    char out[256];
    CHECK(sheng_wifi_log_export(ring, Lines, 0, out, sizeof out) == 0);
}

static void before_the_ring_fills(void)
{
    char out[256];
    write_lines(0, 3);
    const size_t n = sheng_wifi_log_export(ring, Lines, 3, out, sizeof out);
    CHECK(n == strlen("0 6 line 0\n1 6 line 1\n2 6 line 2\n"));
    CHECK(!memcmp(out, "0 6 line 0\n1 6 line 1\n2 6 line 2\n", n));
}

static void after_it_wrapped_oldest_first(void)
{
    char out[512];
    write_lines(0, 13);   // lines 5..12 are held, 12 the newest
    const size_t n = sheng_wifi_log_export(ring, Lines, 13, out, sizeof out);
    out[n] = 0;
    CHECK(!strncmp(out, "5 6 line 5\n", 11));
    CHECK(strstr(out, "12 6 line 12\n") && out[n - 1] == '\n');
    CHECK(!strstr(out, "line 4\n"));
    unsigned newlines = 0;
    for (size_t i = 0; i < n; ++i) newlines += out[i] == '\n';
    CHECK(newlines == Lines);
}

static void an_unterminated_line_is_cut(void)
{
    char out[512];
    memset(ring[0], 'x', sizeof ring[0]);   // a line being written, without its terminator
    const size_t n = sheng_wifi_log_export(ring, Lines, 1, out, sizeof out);
    CHECK(n == SHENG_WIFI_LOG_LINE);   // SHENG_WIFI_LOG_LINE - 1 bytes and the newline
    CHECK(out[n - 1] == '\n' && out[0] == 'x');
}

static void the_buffer_bounds_the_output(void)
{
    char out[25];
    write_lines(0, 3);
    const size_t n = sheng_wifi_log_export(ring, Lines, 3, out, sizeof out);
    CHECK(n <= sizeof out);
    CHECK(n == strlen("0 6 line 0\n1 6 line 1\n"));   // whole lines only
}

int main(void)
{
    empty_ring_gives_nothing();
    before_the_ring_fills();
    after_it_wrapped_oldest_first();
    an_unterminated_line_is_cut();
    the_buffer_bounds_the_output();
    printf("checks %u\n", checks);
    return 0;
}
