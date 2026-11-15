/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "../BluetoothHciTrace.h"
#include <assert.h>
#include <stdlib.h>

int main(void) {
    struct bt_hci_trace *trace = calloc(1, sizeof(*trace));
    assert(trace);
    char out[BtHciTraceReserve], retry[BtHciTraceReserve];
    uint8_t inquiry[] = {1, 4, 5, 0x33, 0x8b, 0x9e, 10, 255};
    bt_hci_record(trace, 123, 1, 1, inquiry, sizeof(inquiry));
    uint64_t cursor = 0;
    assert(!bt_hci_dump(trace, out, 64, &cursor) && !cursor);
    size_t n = bt_hci_dump(trace, out, sizeof(out), &cursor);
    assert(n && n == strlen(out) && cursor == 1);
    assert(strstr(out, "seq=1 ns=123 TX_UART h4=01 bytes=8 data=010405338b9e0aff\n"));
    /* A failed disk write retries from the last committed cursor. */
    uint64_t committed = 0;
    assert(bt_hci_dump(trace, retry, sizeof(retry), &committed) == n);
    assert(!strcmp(out, retry) && committed == cursor);
    uint8_t maximum[BtHciTraceBytes]; memset(maximum, 0xab, sizeof(maximum));
    bt_hci_record(trace, 124, 0, 4, maximum, sizeof(maximum));
    assert(bt_hci_dump(trace, out, sizeof(out), &cursor) && cursor == 2);
    assert(strstr(out, "bytes=258 data=") && strlen(out) < sizeof(out));
    for (unsigned i = 0; i < BtHciTraceEntries + 4; ++i)
        bt_hci_record(trace, 200 + i, 0, 4, inquiry, sizeof(inquiry));
    cursor = 0;
    assert(bt_hci_dump(trace, out, sizeof(out), &cursor));
    assert(strstr(out, "missed=6 dropped=0") && cursor < trace->head);
    while (cursor < trace->head) assert(bt_hci_dump(trace, out, sizeof(out), &cursor));
    uint64_t panic = UINT64_MAX;
    assert(bt_hci_dump(trace, out, sizeof(out), &panic) && panic == trace->head);
    unsigned rows = 0;
    for (char *p = out; (p = strstr(p, "seq=")); ++p) rows++;
    assert(rows == 16);
    trace->lock = 1;
    bt_hci_record(trace, 0, 0, 4, inquiry, sizeof(inquiry));
    assert(!bt_hci_dump(trace, out, sizeof(out), &cursor));
    trace->lock = 0;
    assert(bt_hci_dump(trace, out, sizeof(out), &cursor));
    assert(strstr(out, "dropped=1"));
    trace->head = UINT64_MAX - 1;
    bt_hci_record(trace, 0, 0, 4, inquiry, sizeof(inquiry));
    assert(trace->head == UINT64_MAX - 1 && trace->dropped == 2);
    free(trace);
    puts("PASS - HCI ring, full packets, retry cursor, overwrite accounting and nonblocking panic snapshot");
    return 0;
}
