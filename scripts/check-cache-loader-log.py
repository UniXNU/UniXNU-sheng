#!/usr/bin/env python3
"""Check bounded production cache-log capture with explicit VFS stand-ins."""
from pathlib import Path
import hashlib
import json
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / 'package/BootDiagExport.c').read_text()
body = 'void boot_cache_loader_log_capture(' + source.split('void boot_cache_loader_log_capture(', 1)[1].split('\nenum {DiagMaxFiles', 1)[0]
out = root / 'build/cache-loader-log'
out.mkdir(exist_ok=True)
(out / 'report.json').unlink(missing_ok=True)
prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/types.h>
#include <time.h>
typedef void *vfs_context_t; typedef void *vnode_t;
#define NULLVP NULL
#define FREAD 1
#define UIO_READ 0
#define UIO_SYSSPACE 0
#define IO_NOAUTH 0
struct vnode_attr { uint64_t va_data_size; struct timespec va_modify_time; };
#define VATTR_INIT(x) memset(x, 0, sizeof(*(x)))
#define VATTR_WANTED(x, y) ((void)0)
static unsigned opens, closes, reads, allocations, frees;
static int opt_in = 1, present, fail_read; static unsigned modified = 1;
static char file[10000], logs[100000]; static size_t size = 9000, logged;
static int PE_parse_boot_argn(const char *name, void *value, size_t bytes) {
 assert(!strcmp(name,"sheng-cache-log") && bytes==4); *(uint32_t *)value=opt_in; return 1;
}
static void *IOMallocData(size_t n) { assert(n==8192); allocations++; return malloc(n); }
static void IOFreeData(void *p,size_t n) { assert(p && n==8192); frees++; free(p); }
static vfs_context_t vfs_context_kernel(void) {return (void *)1;}
static void *vfs_context_ucred(void *c) {return c;}
static void *vfs_context_proc(void *c) {return c;}
static int vnode_open(const char *path,int flags,int a,int b,vnode_t *vp,void *ctx) {
 assert(!strcmp(path,"/private/var/log/com.apple.xpc.launchd/launchd_cache_loader.log"));
 assert(flags==FREAD); opens++; if(!present)return ENOENT; *vp=(void *)1; return 0;
}
static int vnode_getattr(vnode_t vp,struct vnode_attr *va,void *ctx) {va->va_data_size=size;va->va_modify_time.tv_sec=modified;return 0;}
static void vnode_close(vnode_t vp,int flags,void *ctx) {assert(vp);closes++;}
static int vn_rdwr(int op,vnode_t vp,char *buffer,int length,off_t offset,int space,
 int flags,void *cred,int *resid,void *proc) {
 assert(op==UIO_READ && length<=8192 && offset>=0 && (uint64_t)offset+length==size);
 reads++; if(fail_read)return EIO; memcpy(buffer,file+offset,length);*resid=0;return 0;
}
static void native_wifi_logf(int level,const char *format,...) {
 va_list ap;va_start(ap,format);int n=vsnprintf(logs+logged,sizeof(logs)-logged,format,ap);va_end(ap);
 assert(n>=0 && (size_t)n+1<sizeof(logs)-logged);logged+=(size_t)n;logs[logged++]='\n';logs[logged]=0;
}
'''
tests = r'''
int main(int argc,char **argv) {
 opt_in=argc==1;
 if(!opt_in) {boot_cache_loader_log_capture(20);boot_cache_loader_log_capture(121);assert(!opens && !allocations);puts("disabled capture: zero VFS I/O and zero allocations");return 0;}
 memset(file,'X',sizeof(file));memcpy(file+size-24,"expected loader failure\n",24);
 boot_cache_loader_log_capture(10);assert(opens==1 && !allocations && strstr(logs,"read status 2"));
 present=1;boot_cache_loader_log_capture(14);assert(reads==1 && allocations==1 && closes==1);
 assert(strstr(logs,"size 9000 offset 808 bytes 8192") && strstr(logs,"expected loader failure"));
 boot_cache_loader_log_capture(15);assert(opens==2);
 boot_cache_loader_log_capture(18);assert(reads==1 && closes==2);
 modified++;memcpy(file+size-24,"rewritten loader error!\n",24);
 boot_cache_loader_log_capture(22);assert(reads==2 && closes==3 && strstr(logs,"rewritten loader error!"));
 size++;fail_read=1;boot_cache_loader_log_capture(26);assert(reads==3 && closes==4 && strstr(logs,"read status 5"));
 boot_cache_loader_log_capture(121);assert(allocations==frees);
 assert(strstr(logs,"mtime 2."));
 puts("enabled capture: fixed path, 8 KiB tail bound, rate limit, same-size rewrite detection, unchanged-file skip, read failure and teardown pass");
}
'''
(out / 'test.c').write_text(prefix + body + tests)
subprocess.run(['clang', '-std=gnu11', '-O1', '-g', '-fsanitize=address,undefined',
                str(out / 'test.c'), '-o', str(out / 'test')], check=True)
output = ''
for arguments in [[], ['off']]:
    result = subprocess.run([str(out / 'test'), *arguments], capture_output=True, text=True)
    print(result.stdout, end=''); print(result.stderr, end='')
    if result.returncode: raise SystemExit(result.returncode)
    output += result.stdout
(out / 'report.json').write_text(json.dumps(dict(passed=True, vfs_simulated=True,
    source_sha256=hashlib.sha256(source.encode()).hexdigest(), output=output), indent=2) + '\n')
