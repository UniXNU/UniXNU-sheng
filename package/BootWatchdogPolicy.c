// SPDX-License-Identifier: MIT
// Explicit temporary bring-up policy for an unavailable watchdog backend.
// Runs after writable scratch Data mounts, before the unchanged launchd exec.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysproto.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <pexpert/pexpert.h>

static const char policy_head[] =
 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
 "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
 "<plist version=\"1.0\"><dict>";
static const char policy_watchdogd[]="<key>com.apple.watchdogd</key><true/>";
// Bring-up isolation: hardware stacks show identityservicesd, contextstored and locationd in a
// cycle of synchronous XPC waits. A disabled job makes its Mach service lookups fail instead of hang.
// sheng-disable-jobs=<label>[,<label>...] (restricted label characters, bounded length).
static const char policy_tail[]="</dict></plist>\n";
// sheng-lite=1: jobs that only reach for the network, iCloud or the photo library and have nothing to do on this tablet (no radios, no accounts),
// all of which start at the first boot of a fresh Data and compete with SpringBoard for the CPU and for memory. The first four are the
// cycle of synchronous XPC waits the isolation work found. Labels are the ones in the target's own LaunchDaemons.
static const char lite_jobs[]=
 "com.apple.identityservicesd,com.apple.locationd,com.apple.CommCenter,com.apple.bluetoothd,"
 "com.apple.photoanalysisd,com.apple.mediaanalysisd,com.apple.mediastream.mstreamd,com.apple.icloud.searchpartyd,"
 "com.apple.findmy.findmybeaconingd,com.apple.familycircled,com.apple.financed,com.apple.softwareupdateservicesd,"
 "com.apple.ndoagent,com.apple.biomesyncd,com.apple.email.maild,com.apple.amsengagementd,com.apple.adid,com.apple.eyereliefd,"
 // Crash or respawn loops seen on hardware (int-176): fairplayd.A2 is KeepAlive and exits at once (22 launches in 6 minutes), passd crashes,
 // obliteration is a data-wipe daemon with nothing to do here. iconservicesagent also dies in CI::GLContext::init, but disabling it leaves
 // the icons missing (seen on hardware), so it stays enabled.
 "com.apple.fairplayd.A2,com.apple.passd,com.apple.mobile.obliteration";
static char policy[2048],jobs[200];
volatile int sheng_watchdog_policy_status=-1;

// sheng-setup=1 or sheng-data-reset=2 (the install entry's "show Hello" choice): remove Setup's completion flags once, so this boot shows the Hello page.
// iPadOS writes them again when Setup finishes, so later boots do not show it. Absent files are fine.
static int sheng_boot_setup_reset(proc_t p,uint64_t scratch) {
 static const char *const files[]={
  "/private/var/mobile/Library/Preferences/com.apple.purplebuddy.plist",
  "/private/var/mobile/Library/Preferences/com.apple.purplebuddy.notbackedup.plist"};
 for(unsigned i=0;i<sizeof(files)/sizeof(files[0]);++i) {
  int32_t result=0;
  int error=copyout(files[i],scratch,strlen(files[i])+1);
  if(error)return error;
  struct unlink_args rm={.path=scratch};
  error=unlink(p,&rm,&result);
  if(error && error!=ENOENT)return error;
 }
 return 0;
}

// One comma-separated label at *c into the policy as a disabled key; advances *c past it.
static int append_job(const char **cursor) {
 const char *c=*cursor,*e=c;while(*e && *e!=',')++e;
 if(e>c) {
  if(strlcat(policy,"<key>",sizeof(policy))>=sizeof(policy))return EINVAL;
  size_t at=strlen(policy);if(at+(size_t)(e-c)+1>=sizeof(policy))return EINVAL;
  memcpy(policy+at,c,(size_t)(e-c));policy[at+(size_t)(e-c)]=0;
  if(strlcat(policy,"</key><true/>",sizeof(policy))>=sizeof(policy))return EINVAL;
 }
 *cursor=*e?e+1:e;return 0;
}

int sheng_boot_watchdog_policy(void *process, uint64_t scratch) {
 proc_t p=process;
 int enabled=0,error=0,fd=-1;int32_t result=0;
 PE_parse_boot_argn("sheng-disable-watchdogd",&enabled,sizeof(enabled));
 if(!PE_parse_boot_argn("sheng-disable-jobs",jobs,sizeof(jobs)))jobs[0]=0;jobs[sizeof(jobs)-1]=0;
 int lite=0;PE_parse_boot_argn("sheng-lite",&lite,sizeof(lite));if(lite!=1)lite=0;
 if(enabled!=1)enabled=0;
 for(const char *c=jobs;*c;++c)if(!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')||*c=='.'||*c=='-'||*c==','))return EINVAL;
 int setup=0,reset=0;
 PE_parse_boot_argn("sheng-setup",&setup,sizeof(setup));
 PE_parse_boot_argn("sheng-data-reset",&reset,sizeof(reset));   // 2 = erase Data and show the Hello page; 1 = erase Data, Setup stays skipped
 if(reset==2)setup=1;
 if(setup==1 && (error=sheng_boot_setup_reset(p,scratch))) {
  sheng_watchdog_policy_status=error;
  printf("sheng setup flags reset failed: %d\n",error);
  return error;
 }
 if(setup==1)printf("sheng setup flags reset: Hello page on this boot\n");
 if(!enabled && !jobs[0] && !lite)return 0;
 policy[0]=0;strlcat(policy,policy_head,sizeof(policy));
 if(enabled)strlcat(policy,policy_watchdogd,sizeof(policy));
 if(lite)for(const char *c=lite_jobs;*c;) {if((error=append_job(&c)))return error;}
 for(const char *c=jobs;*c;) {if((error=append_job(&c)))return error;}
 strlcat(policy,policy_tail,sizeof(policy));
 const char *directory="/private/var/db/com.apple.xpc.launchd";
 const char *path="/private/var/db/com.apple.xpc.launchd/disabled.plist";
 if((error=copyout(directory,scratch,strlen(directory)+1)))goto done;
 struct mkdir_args md={.path=scratch,.mode=0755};
 error=mkdir(p,&md,&result);
 if(error && error!=EEXIST)goto done;
 if((error=copyout(path,scratch,strlen(path)+1)))goto done;
 // With persistent Data the previous boot's file is there: this policy is rewritten in place on every boot (it must follow the
 // boot arguments). Symlinks are still refused (O_NOFOLLOW) and the fstat check below insists on a root-owned regular file.
 struct open_args op={.path=scratch,.flags=O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,.mode=0644};
 if((error=open(p,&op,&result)))goto done;
 fd=result;
 // The scratch page keeps the cache-mount environment slots at +0x400, so the policy (the lite list makes it longer than 0x200 bytes) goes
 // out in pieces through the 0x200 bytes at +0x200.
 for(size_t sent=0,total=strlen(policy);sent<total;) {
  size_t piece=total-sent<0x1f0?total-sent:0x1f0;
  if((error=copyout(policy+sent,scratch+0x200,piece)))goto done;
  struct write_args wr={.fd=fd,.cbuf=scratch+0x200,.nbyte=piece};
  user_ssize_t written=0;
  if((error=write(p,&wr,&written)))goto done;
  if(written!=(user_ssize_t)piece){error=EIO;goto done;}
  sent+=piece;
 }
 struct fchown_args owner={.fd=fd,.uid=0,.gid=0};
 if((error=fchown(p,&owner,&result)))goto done;
 struct fchmod_args mode={.fd=fd,.mode=0644};
 if((error=fchmod(p,&mode,&result)))goto done;
 struct fsync_args sync={.fd=fd};
 if((error=fsync(p,&sync,&result)))goto done;
 // Preserve the cache-mount environment slots at +0x400, +0x500, +0x600.
 struct fstat64_args sa={.fd=fd,.ub=scratch+0x800};struct stat64 st={0};
 if((error=sys_fstat64(p,&sa,&result)) || (error=copyin(sa.ub,&st,sizeof(st))))goto done;
 if(st.st_uid!=0 || st.st_gid!=0 || !S_ISREG(st.st_mode) || (st.st_mode&0777)!=0644 || st.st_size!=(off_t)(strlen(policy)))error=EINVAL;
 done:
 if(fd>=0){struct close_args cl={.fd=fd};int e=sys_close(p,&cl,&result);if(!error)error=e;}
 // A real empty migration marker selects the qualified modern override loader.
 if(!error) {
  const char *marker="/private/var/db/com.apple.xpc.launchd/disabled.migrated";
  error=copyout(marker,scratch,strlen(marker)+1);
  if(!error) {
   struct open_args mark={.path=scratch,.flags=O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,.mode=0644};
   error=open(p,&mark,&result);
   if(!error){struct close_args cl={.fd=result};error=sys_close(p,&cl,&result);}
   else if(error==EEXIST)error=0;
  }
 }
 sheng_watchdog_policy_status=error;
 printf("sheng bring-up watchdogd launch policy: %d (hardware watchdog not implemented)\n",error);
 return error;
}
