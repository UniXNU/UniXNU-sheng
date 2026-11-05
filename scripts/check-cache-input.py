#!/usr/bin/env python3
"""Exercise the production parser-input snapshot with simulated copyin."""
from pathlib import Path
import hashlib,json,subprocess
root=Path(__file__).resolve().parents[1]
source=(root/'package/BootDiagExport.c').read_text()
body='static void *cache_input;'+source.split('static void *cache_input;',1)[1].split('static char *diag_note;',1)[0]
out=root/'build/cache-input';out.mkdir(exist_ok=True)
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint64_t user_addr_t;
#define PAGE_SIZE 4096
static unsigned allocations,frees,copies;
static uint32_t enabled;
static void *valid;static size_t valid_size;
static int PE_parse_boot_argn(const char *name,void *value,size_t size) {
 assert(!strcmp(name,"sheng-cache-log") && size==4);*(uint32_t *)value=enabled;return 1;
}
static void *IOMallocAligned(size_t size,size_t align) {assert(align==4096);allocations++;return malloc(size);}
static void IOFreeAligned(void *p,size_t size) {(void)size;frees++;free(p);}
static int copyin(user_addr_t address,void *to,size_t size) {
 copies++;if(address!=(uintptr_t)valid || size!=valid_size)return EFAULT;
 memcpy(to,valid,size);return 0;
}
'''
tests=r'''
int main(void) {
 (void)cache_keep;
 unsigned char bytes[8193];for(unsigned i=0;i<sizeof bytes;i++)bytes[i]=(unsigned char)(i*19);
 valid=bytes;valid_size=sizeof bytes;
 boot_cache_input_capture((uintptr_t)valid,valid_size);assert(!allocations && !copies && !boot_cache_input_ready());
 enabled=1;
 boot_cache_input_capture((uintptr_t)valid,31);boot_cache_input_capture((uintptr_t)valid,4*1024*1024+1);
 assert(!allocations && !copies);
 assert(!cache_path("/System/Library/xpc/launchd.plist"));
 boot_cache_input_capture(1,valid_size);assert(allocations==1 && frees==1 && !boot_cache_input_ready());
 boot_cache_input_capture((uintptr_t)valid,valid_size);
 assert(allocations==2 && boot_cache_input_ready()==sizeof bytes && !memcmp(cache_input,bytes,sizeof bytes));
 assert(cache_path("/System/Library/xpc/launchd.plist") && cache_path("/private/var/ngpu/xpc/launchd.plist"));
 assert(!cache_path("/private/var/mobile/Library/Preferences/test.plist"));
 bytes[0]^=255;assert(((unsigned char *)cache_input)[0]!=bytes[0]);
 unsigned before=copies;boot_cache_input_capture((uintptr_t)valid,valid_size);
 assert(copies==before && allocations==2);
 free(cache_input);
 puts("PASS: opt-in snapshot, 4 MiB bound, failed copy teardown, immutable byte copy, one-shot publication and fixed cache paths");
}
'''
(out/'test.c').write_text(prefix+body+tests)
subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',str(out/'test.c'),'-o',str(out/'test')],check=True)
r=subprocess.run([out/'test'],capture_output=True,text=True);(out/'output.txt').write_text(r.stdout+r.stderr)
if r.returncode:raise SystemExit(r.stdout+r.stderr)
(out/'report.json').write_text(json.dumps(dict(passed=True,copyin_simulated=True,source_sha256=hashlib.sha256(source.encode()).hexdigest(),output=r.stdout),indent=2)+'\n')
print(r.stdout,end='')
