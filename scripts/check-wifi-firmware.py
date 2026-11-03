#!/usr/bin/env python3
"""Run the production Data firmware reader against explicit VFS mocks."""
from pathlib import Path
import hashlib,json,re,subprocess
root=Path(__file__).resolve().parents[1];source=(root/'package/WifiFirmware.c').read_text();body=source[source.index('int sheng_wifi_firmware_read('):]
out=root/'build/wifi-firmware';out.mkdir(exist_ok=True)
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "WifiFirmware.h"
typedef void *vfs_context_t;typedef void *vnode_t;typedef char *caddr_t;
struct vnode_attr {uint64_t va_data_size;};
#define VATTR_INIT(p) memset(p,0,sizeof(*(p)))
#define VATTR_WANTED(p,f) ((void)0)
enum {FREAD=1,VREG=1,UIO_READ=1,UIO_SYSSPACE=2,IO_NOAUTH=4};
static unsigned opens,closes,allocations,frees,reads;static int present=1,regular=1,short_read,read_error,allocation_error,attribute_error;
static volatile int native_gpu_addon_stage;
static uint64_t size=64;static unsigned char file[128];
static vfs_context_t vfs_context_kernel(void){return (void *)1;}
static void *vfs_context_ucred(void *c){return c;}static void *vfs_context_proc(void *c){return c;}
static int vnode_open(const char *p,int flags,int a,int b,vnode_t *v,void *ctx){(void)a;(void)b;(void)ctx;assert(!strcmp(p,SHENG_WIFI_FIRMWARE_ROOT "/amss.bin")&&flags==FREAD);opens++;if(!present)return ENOENT;*v=(void *)1;return 0;}
static int vnode_getattr(vnode_t v,struct vnode_attr *a,void *ctx){(void)v;(void)ctx;a->va_data_size=size;return attribute_error;}
static int vnode_vtype(vnode_t v){(void)v;return regular?VREG:2;}
static void vnode_close(vnode_t v,int flags,void *ctx){(void)ctx;assert(v&&flags==FREAD);closes++;}
static void *IOMallocData(size_t n){if(allocation_error)return NULL;assert(n<=sizeof file);allocations++;return malloc(n);}
static void IOFreeData(void *p,size_t n){(void)n;assert(p);frees++;free(p);}
static int vn_rdwr(int op,vnode_t v,char *buf,int length,long offset,int space,int flags,void *cred,int *resid,void *proc){(void)v;(void)cred;(void)proc;assert(op==UIO_READ&&!offset&&space==UIO_SYSSPACE&&flags==IO_NOAUTH);reads++;if(read_error)return EIO;memcpy(buf,file,length);*resid=short_read;return 0;}
'''
tests=r'''
int main(void){
 for(unsigned i=0;i<sizeof file;i++)file[i]=(unsigned char)i;
 uint8_t *data;size_t length;assert(sheng_wifi_firmware_read("amss.bin",&data,&length)==EAGAIN&&!data&&!length&&!opens);native_gpu_addon_stage=1;
 assert(!sheng_wifi_firmware_read("amss.bin",&data,&length)&&length==64&&!memcmp(data,file,64));IOFreeData(data,length);
 unsigned before=opens;assert(sheng_wifi_firmware_read("../amss.bin",&data,&length)==ENOENT&&!data&&!length&&opens==before);
 assert(sheng_wifi_firmware_read("join.txt",&data,&length)==ENOENT&&opens==before);
 present=0;assert(sheng_wifi_firmware_read("amss.bin",&data,&length)==ENOENT&&!data&&!length);present=1;
 size=0;assert(sheng_wifi_firmware_read("amss.bin",&data,&length)==EINVAL);size=32u*1024*1024+1u;assert(sheng_wifi_firmware_read("amss.bin",&data,&length)==EINVAL);size=64;
 regular=0;assert(sheng_wifi_firmware_read("amss.bin",&data,&length)==EINVAL);regular=1;
 attribute_error=EIO;assert(sheng_wifi_firmware_read("amss.bin",&data,&length)==EIO);attribute_error=0;
 allocation_error=1;assert(sheng_wifi_firmware_read("amss.bin",&data,&length)==ENOMEM);allocation_error=0;
 short_read=1;assert(sheng_wifi_firmware_read("amss.bin",&data,&length)==EIO&&!data&&!length);short_read=0;
 read_error=1;assert(sheng_wifi_firmware_read("amss.bin",&data,&length)==EIO&&!data&&!length);
 assert(opens==closes+1&&allocations==frees);
 puts("PASS: production Data firmware path, content/ownership, whitelist, absent/empty/oversized/nonregular input, VFS/allocation/short-read failures and cleanup");
}
'''
(out/'test.c').write_text(prefix+body+tests)
subprocess.run(['clang','-std=c11','-O1','-g','-fsanitize=address,undefined','-I',str(root/'package'),str(out/'test.c'),'-o',str(out/'test')],check=True)
r=subprocess.run([str(out/'test')],capture_output=True,text=True,check=True);print(r.stdout,end='')
manifest=json.loads((root/'firmware/wcn7850/manifest.json').read_text())['files']
for name,info in manifest.items():
 data=(root/'firmware/wcn7850'/name).read_bytes();assert len(data)==info['bytes'] and hashlib.sha256(data).hexdigest()==info['sha256']
assert '"oops"' not in (root/'package/WifiProbe.cpp').read_text()
(out/'report.json').write_text(json.dumps(dict(passed=True,vfs_simulated=True,hardware_verified=False,source_sha256=hashlib.sha256(source.encode()).hexdigest(),firmware_manifest=manifest,output=r.stdout),indent=2)+'\n')
