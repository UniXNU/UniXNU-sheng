#!/usr/bin/env python3
"""Host ASan/UBSan verification for FairPlayDiagExport.h and macOS built-in vmm-hide boot-args."""
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'package'


def main() -> None:
    watchdog_src = (PACKAGE / 'BootWatchdogPolicy.c').read_text()
    start = watchdog_src.index('char PE_builtin_boot_args[]')
    end = watchdog_src.index('static char policy')
    boot_args_decl = watchdog_src[start:end]

    with tempfile.TemporaryDirectory(prefix='sheng-fairplay-diag-') as tmp_s:
        tmp = Path(tmp_s)
        diag_c = tmp / 'fairplay_diag_test.c'
        diag_bin = tmp / 'fairplay_diag_test'
        diag_c.write_text(r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "FairPlayDiagExport.h"

static void check_bounded(const char *vmm_hide, size_t cap) {
    char *buf = (char *)malloc(cap);
    assert(buf != NULL);
    memset(buf, 0x5a, cap);
    size_t n = sheng_fairplay_diag_text(1, 7, 2, 1, 3, 2209, vmm_hide, buf, cap);
    if (cap < 2) {
        assert(n == 0);
        if (cap == 1) {
            assert(buf[0] == '\0');
        }
    } else {
        assert(n > 0 && n < cap);
        assert(strlen(buf) == n);
        assert(buf[n] == '\0');
        assert(buf[n - 1] == '\n');
    }
    free(buf);
}

int main(void) {
    char out[SHENG_FAIRPLAY_DIAG_TEXT];
    memset(out, 0x7f, sizeof(out));
    size_t n = sheng_fairplay_diag_text(
        1, 14, 3, 2, 0, 0,
        "adid,fairplayd,fairplaydeviceidentityd,akd",
        out, sizeof(out));
    const char *expected =
        "aes_registered=1 vmm_hide_hits=14 fairplay_0x899_uses=3 "
        "nonce_0x8a4_uses=2 unsupported_uses=0 last_unsupported_selector=0x0 "
        "vmm_hide=adid,fairplayd,fairplaydeviceidentityd,akd\n";
    assert(n == strlen(expected));
    assert(strcmp(out, expected) == 0);

    /* Empty and NULL vmm_hide fall back to "none"; 2209 formats as 0x8a1. */
    n = sheng_fairplay_diag_text(1, 0, 0, 4, 1, 2209, "", out, sizeof(out));
    assert(n == strlen(out));
    assert(strstr(out, "last_unsupported_selector=0x8a1 ") != NULL);
    assert(strstr(out, "vmm_hide=none\n") != NULL);

    n = sheng_fairplay_diag_text(0, 0, 0, 0, 0, 0, NULL, out, sizeof(out));
    assert(n == strlen(out));
    assert(strstr(out, "aes_registered=0 ") == out);
    assert(strstr(out, "vmm_hide=none\n") != NULL);

    /* 300-byte and 480-byte long strings across tight and full capacities. */
    char long300[301];
    memset(long300, 'a', 300);
    long300[300] = '\0';
    check_bounded(long300, 128);
    check_bounded(long300, SHENG_FAIRPLAY_DIAG_TEXT);

    char long480[481];
    memset(long480, 'b', 480);
    long480[480] = '\0';
    check_bounded(long480, SHENG_FAIRPLAY_DIAG_TEXT);
    check_bounded(long480, 64);
    check_bounded(long480, 2);
    check_bounded(long480, 1);
    assert(sheng_fairplay_diag_text(1, 1, 1, 1, 0, 0, "akd", NULL, 64) == 0);
    return 0;
}
''')
        subprocess.run(
            ['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
             '-fsanitize=address,undefined', '-I', str(PACKAGE),
             str(diag_c), '-o', str(diag_bin)],
            check=True, timeout=45,
        )
        subprocess.run([str(diag_bin)], check=True, timeout=15)

        for osx_flag in (1, 0):
            ba_c = tmp / f'boot_args_{osx_flag}.c'
            ba_bin = tmp / f'boot_args_{osx_flag}'
            ba_c.write_text(
                '#include <assert.h>\n#include <string.h>\n'
                + boot_args_decl + r'''
int main(void) {
#if XNU_TARGET_OS_OSX
    assert(strstr(PE_builtin_boot_args, " vmm-hide=adid,fairplayd,fairplaydeviceidentityd,akd") != NULL);
    assert(sizeof(PE_builtin_boot_args) < 512);
#else
    assert(strstr(PE_builtin_boot_args, "vmm-hide") == NULL);
#endif
    return 0;
}
''')
            subprocess.run(
                ['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                 '-fsanitize=address,undefined', f'-DXNU_TARGET_OS_OSX={osx_flag}',
                 str(ba_c), '-o', str(ba_bin)],
                check=True, timeout=45,
            )
            subprocess.run([str(ba_bin)], check=True, timeout=15)

    print('PASS: FairPlayDiagExport.h formatting/truncation bounds and macOS PE_builtin_boot_args vmm-hide policy')


if __name__ == '__main__':
    main()
