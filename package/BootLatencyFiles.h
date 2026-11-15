/* SPDX-License-Identifier: MIT */
#ifndef BOOT_LATENCY_FILES_H
#define BOOT_LATENCY_FILES_H
#include <string.h>
static inline int bl_latency_contains(const char *text,size_t size,const char *term) {
 size_t length=strlen(term);
 for(size_t at=0;at+length<=size;++at)if(!memcmp(text+at,term,length))return 1;
 return 0;
}
static const char *const bl_crash_processes[]={"STExtractionService","dasd","WidgetRenderer","FitnessWidget","CalendarWidget",
 "WindowServer","Finder","loginwindow","System Settings","WallpaperAgent","WallpaperVideoExtension",
 "WallpaperDynamicExtension","VTDecoderXPCService","iconservicesagent","iconservicesd","icon-probe","Aerials"};
enum {BlCrashProcessCount=sizeof bl_crash_processes/sizeof bl_crash_processes[0]};
static inline int bl_crash_process(const char *path) {
 const char *name=path;for(const char *at=path;*at;++at)if(*at=='/')name=at+1;
 for(unsigned i=0;i<BlCrashProcessCount;++i) {
  size_t n=strlen(bl_crash_processes[i]);
  if(!strncmp(name,bl_crash_processes[i],n) && (name[n]=='-' || name[n]=='_' || !strncmp(name+n,"Extension-",10)))return (int)i;
 }
 return -1;
}
static inline int bl_latency_file(const char *path) {
 return !strcmp(path,"/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/frames.trace") ||
        !strcmp(path,"/private/var/root/Library/Caches/com.apple.backboardd/m2v/frames.trace") ||
        !strcmp(path,"/private/var/tmp/NativeAdrenoMetal.trace/frames.trace") ||
        ((bl_latency_contains(path,strlen(path),"/Library/Logs/CrashReporter/") ||
          bl_latency_contains(path,strlen(path),"/Library/Logs/DiagnosticReports/")) && bl_crash_process(path)>=0);
}
static inline unsigned bl_macos_file_limit(const char *path) {
 static const char cache[]="/private/var/tmp/NativeAdrenoMetal.cache/";
 static const char session[]="/Users/Shared/unixnu-test/";
 static const char *const prime[]={"prime.log","prime-done","prime-fn2","prime-refused2","prime-bad","prime-current"};
 if(!strncmp(path,cache,sizeof cache-1)) {
  const char *name=path+sizeof cache-1;
  if(!strcmp(name,"timing.log"))return 512*1024;
  if(!strcmp(name,"runtime-policy.log"))return 256*1024;
  if(!strcmp(name,"prime-resume"))return 1024;
  for(unsigned i=0;i<sizeof prime/sizeof prime[0];++i)if(!strcmp(name,prime[i]))return i==2?4*1024*1024:512*1024;
 }
 if(!strncmp(path,session,sizeof session-1)) {
  const char *name=path+sizeof session-1;
  if(!strcmp(name,"daemon.log") || !strcmp(name,"agent.log"))return 256*1024;
  if(!strcmp(name,"graphics-events.log") || !strcmp(name,"graphics-events-previous.log"))return 2*1024*1024;
  if(!strcmp(name,"graphics-state.log"))return 512*1024;
  if(!strcmp(name,"video-probe.log") || !strcmp(name,"previous-video-probe.log"))return 128*1024;
  if(!strcmp(name,"icon-probe.log") || !strcmp(name,"previous-icon-probe.log"))return 256*1024;
  if(!strcmp(name,"icons.png") || !strcmp(name,"previous-icons.png"))return 2*1024*1024;
  if(!strcmp(name,"desktop-180.png") || !strcmp(name,"desktop-270.png"))return 2*1024*1024;
 }
 return 0;
}
static inline int bl_latency_message(const char *line,size_t size) {
 static const char *const terms[]={"panic","EXC_","memorystatus:","watermark","STExtractionService",
  "dasd","WidgetRenderer","FitnessWidget","CalendarWidget","GPU fault","GPU hang","PAC exception",
  "WindowServer","loginwindow","NativeAdrenoMetal","memory outside","swap file","vm: Demand swap","unixnu-exit:"};
 for(unsigned i=0;i<sizeof terms/sizeof terms[0];++i) {
  if(bl_latency_contains(line,size,terms[i]))return 1;
 }
 return 0;
}
static inline unsigned bl_latency_messages(char *text,unsigned bytes) {
 unsigned out=0;
 for(unsigned at=0;at<bytes;) {
  unsigned end=at;while(end<bytes && text[end]!='\n')++end;
  unsigned size=end-at+(end<bytes);
  if(bl_latency_message(text+at,size)){memmove(text+out,text+at,size);out+=size;}
  at+=size;
 }
 return out;
}
#endif
