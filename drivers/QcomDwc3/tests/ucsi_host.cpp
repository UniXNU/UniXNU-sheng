// Host test of QcomUcsi.h against a model of the port manager. Build: clang++ -std=c++17 -I.. ucsi_host.cpp && ./a.out
#include "QcomUcsi.h"
#include <assert.h>
#include <stdio.h>
#include <vector>
struct Ppm {
    uint32_t buffer, cci = 0, status; uint64_t last = 0; std::vector<uint64_t> log; bool enabled = false; uint32_t pending = 0;
    void write(uint64_t c) {
        last = c; log.push_back(c);
        if (c == kUcsiReset) {cci = kUcsiCciResetComplete; pending = 0;}
        else if ((c & 0xff) == 0x05) {cci = kUcsiCciCommandComplete; enabled = c != kUcsiClearReset;}
        else if ((c & 0xff) == 0x12) cci = kUcsiCciCommandComplete;
        else if ((c & 0xff) == 0x04) {cci = kUcsiCciAckComplete; if (c & 1ull << 16) pending = 0;}
        cci |= pending;
    }
    uint32_t readAnswer(uint8_t *m) {
        memset(m, 0, 16 + buffer); ucsi_put32(m, QCOM_UCSI_OWNER); ucsi_put32(m + 4, 1); ucsi_put32(m + 8, kUcsiOpRead);
        ucsi_put32(m + 12 + kUcsiOffVersion, buffer == 48 ? 0x0110 : 0x0200);
        ucsi_put32(m + 12 + kUcsiOffCci, cci | pending);
        ucsi_put32(m + 12 + kUcsiOffMessageIn, status);
        return 16 + buffer;
    }
};
static void run(QcomUcsi &u, Ppm &p, uint32_t n, uint8_t *out) {
    uint8_t in[QCOM_UCSI_MAX_MESSAGE];
    for (unsigned guard = 0; n && guard < 200; ++guard) {
        if (ucsi_get32(out + 8) == kUcsiOpWrite) {
            uint64_t c = ucsi_get32(out + 12 + kUcsiOffControl) | (uint64_t)ucsi_get32(out + 12 + kUcsiOffControl + 4) << 32;
            assert(n == 16 + p.buffer);
            p.write(c);
            uint8_t ack[16] = {}; ucsi_put32(ack, QCOM_UCSI_OWNER); ucsi_put32(ack + 4, 1); ucsi_put32(ack + 8, kUcsiOpWrite);
            n = ucsi_receive(&u, ack, 16, out);
        } else {
            uint32_t m = p.readAnswer(in);
            n = ucsi_receive(&u, in, m, out);
        }
    }
}
int main() {
    for (uint32_t buffer : {48u, (uint32_t)QCOM_UCSI_V2_BUFFER}) for (uint32_t latched : {0u, (uint32_t)kUcsiCciResetComplete}) {
        Ppm p; p.buffer = buffer; p.cci = latched;
        p.status = 1u << 19 | 1u << 21 | (uint32_t)kUcsiPartnerDfp << 29;
        QcomUcsi u; uint8_t out[QCOM_UCSI_MAX_MESSAGE];
        run(u, p, ucsi_start(&u, out), out);
        assert(u.state == kUcsiReady); assert(u.status_valid && u.connected && u.usb && u.partner == kUcsiPartnerDfp);
        assert(u.version == (buffer == 48 ? 0x0110u : 0x0200u)); assert(p.enabled);
        assert(latched ? p.log[0] == kUcsiClearReset && p.log[1] == kUcsiReset : p.log[0] == kUcsiReset);
        const size_t commands = p.log.size();
        // A notification with nothing changed, as after every command: reads the CCI and writes nothing.
        uint8_t ind[12]; ucsi_put32(ind, QCOM_UCSI_OWNER); ucsi_put32(ind + 4, 2); ucsi_put32(ind + 8, kUcsiOpNotify);
        run(u, p, ucsi_receive(&u, ind, 12, out), out);
        assert(u.state == kUcsiReady); assert(p.log.size() == commands); assert(u.changes == 0);
        // A change: the charger goes away. Status, then its acknowledge, then the connector bits are gone.
        p.status = 0; p.pending = 1u << 1;
        run(u, p, ucsi_receive(&u, ind, 12, out), out);
        assert(u.state == kUcsiReady); assert(!u.connected); assert(u.changes == 1); assert(p.pending == 0);
        assert(p.log.back() == kUcsiAckChange);
        // A change during the status of another one is taken after its acknowledge.
        p.status = 1u << 19 | 1u << 21 | (uint32_t)kUcsiPartnerDfp << 29; p.pending = 1u << 1;
        uint32_t n = ucsi_receive(&u, ind, 12, out);
        assert(n == 12); uint8_t in[QCOM_UCSI_MAX_MESSAGE]; n = ucsi_receive(&u, in, p.readAnswer(in), out);
        assert(n && u.state == kUcsiAskStatus && u.change_read);
        p.write(kUcsiStatus); p.status = 1u << 19 | (uint32_t)kUcsiPartnerUfp << 29;   // it changes again after this read
        n = ucsi_receive(&u, in, p.readAnswer(in), out);
        p.write(kUcsiAckCommand); n = ucsi_read_request(out); n = ucsi_receive(&u, in, p.readAnswer(in), out);
        run(u, p, n, out);
        p.pending = 1u << 1;
        run(u, p, ucsi_receive(&u, ind, 12, out), out);
        assert(u.state == kUcsiReady); assert(u.connected && u.partner == kUcsiPartnerUfp && u.changes == 3); assert(p.pending == 0);
        // Busy hides the rest of the CCI.
        p.cci = kUcsiCciBusy | 1u << 1; const size_t before = p.log.size();
        ucsi_receive(&u, ind, 12, out); n = ucsi_receive(&u, in, p.readAnswer(in), out); assert(!n && p.log.size() == before);
        p.cci = 0;
    }
    // A manager that stops answering ends in failed, not in a loop.
    {Ppm p; p.buffer = 48; p.status = 0; QcomUcsi u; uint8_t out[QCOM_UCSI_MAX_MESSAGE], in[QCOM_UCSI_MAX_MESSAGE];
     ucsi_start(&u, out); uint32_t n = ucsi_receive(&u, in, p.readAnswer(in), out); assert(n && u.state == kUcsiResetting);
     p.cci = 0; ucsi_receive(&u, in, p.readAnswer(in), out); assert(u.polling);
     for (unsigned i = 0; i < kUcsiPollsPerCommand + 2 && u.state != kUcsiFailed; ++i) {u.polling = 1; ucsi_tick(&u, out);}
     assert(u.state == kUcsiFailed);}
    puts("PASS");
}
