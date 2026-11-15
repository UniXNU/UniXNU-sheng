#import <AppKit/AppKit.h>
#import <CoreImage/CoreImage.h>
#import <Metal/Metal.h>
#include <dlfcn.h>

@interface NSObject (SettingsIconProbe)
- (id)initWithType:(NSString *)type;
- (id)initWithApplicationExtensionBundleIdentifier:(NSString *)identifier;
- (id)initWithSize:(CGSize)size scale:(double)scale;
- (void)getCGImageForImageDescriptor:(id)descriptor completion:(void (^)(CGImageRef))completion;
- (void)setIgnoreCache:(BOOL)ignore;
@end

static NSImage *settingsServiceIcon(NSString *path) {
 NSDictionary *info=[NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Contents/Info.plist"]];
 NSString *type=info[@"CFBundleIcons"][@"ISGraphicIconConfiguration"][@"ISTypeIdentifier"];
 id icon=type?[[NSClassFromString(@"ISIcon") alloc] initWithType:type]:
              [[NSClassFromString(@"ISIcon") alloc] initWithApplicationExtensionBundleIdentifier:info[@"CFBundleIdentifier"]];
 id descriptor=[[NSClassFromString(@"ISImageDescriptor") alloc] initWithSize:CGSizeMake(24,24) scale:2];
 [descriptor setIgnoreCache:YES];
 NSString *identifier=type?:info[@"CFBundleIdentifier"];
 printf("settings service type=%s descriptor=%s\n",identifier.UTF8String,[descriptor description].UTF8String);
 __block NSImage *rendered=nil;
 __block BOOL finished=NO;
 [icon getCGImageForImageDescriptor:descriptor completion:^(CGImageRef source) {
  @synchronized(icon) {
   if(source)rendered=[[NSImage alloc] initWithCGImage:source size:NSMakeSize(24,24)];
   finished=YES;
  }
 }];
 NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:30];
 while(deadline.timeIntervalSinceNow>0) {
  @synchronized(icon) {if(finished)return rendered;}
  [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.05]];
 }
 printf("settings service timeout identifier=%s\n",identifier.UTF8String);
 return nil;
}

static BOOL sourcePixels(NSImage *icon, NSUInteger index, NSUInteger size) {
 NSRect bounds=NSMakeRect(0,0,size,size);
 CGImageRef image=[icon CGImageForProposedRect:&bounds context:nil hints:nil];
 if(!image){printf("icon source index=%lu unavailable\n",(unsigned long)index);return NO;}
 unsigned bpc=(unsigned)CGImageGetBitsPerComponent(image),bpp=(unsigned)CGImageGetBitsPerPixel(image);
 CGBitmapInfo info=CGImageGetBitmapInfo(image);
 CFDataRef data=CGDataProviderCopyData(CGImageGetDataProvider(image));
 unsigned invalid=0,nans[4]={0},infinities[4]={0};
 size_t stride=CGImageGetBytesPerRow(image),width=CGImageGetWidth(image),height=CGImageGetHeight(image);
 if(data && (info&kCGBitmapFloatComponents) && (bpc==16 || bpc==32)) {
  const uint8_t *bytes=CFDataGetBytePtr(data);
  size_t length=(size_t)CFDataGetLength(data),step=bpc/8;
  for(size_t y=0;y<height && y*stride+stride<=length;++y) {
   for(size_t x=0;x<width*bpp/8;x+=step) {
    unsigned channel=(unsigned)(x/step)%4;
    uint32_t raw=0;BOOL bad=NO,nan=NO;
    if(bpc==16) {
     uint16_t bits;memcpy(&bits,bytes+y*stride+x,2);
     if((info&kCGBitmapByteOrderMask)==kCGBitmapByteOrder16Big)bits=__builtin_bswap16(bits);
     raw=bits;bad=(bits&0x7c00)==0x7c00;nan=(bits&0x3ff)!=0;
    } else {
     uint32_t bits;memcpy(&bits,bytes+y*stride+x,4);
     if((info&kCGBitmapByteOrderMask)==kCGBitmapByteOrder32Big)bits=__builtin_bswap32(bits);
     raw=bits;bad=(bits&0x7f800000)==0x7f800000;nan=(bits&0x7fffff)!=0;
    }
    if(bad) {
     if(invalid<8)printf("icon invalid index=%lu x=%zu y=%zu channel=%u raw=%08x\n",(unsigned long)index,x/(bpp/8),y,channel,raw);
     ++invalid;if(nan)++nans[channel];else ++infinities[channel];
    }
   }
  }
 }
 printf("icon source index=%lu requested=%lu size=%zux%zu bpc=%u bpp=%u bitmap=%#x nonfinite=%u\n",(unsigned long)index,(unsigned long)size,width,height,bpc,bpp,(unsigned)info,invalid);
 if(invalid)printf("icon nonfinite index=%lu nan=%u,%u,%u,%u inf=%u,%u,%u,%u\n",(unsigned long)index,
  nans[0],nans[1],nans[2],nans[3],infinities[0],infinities[1],infinities[2],infinities[3]);
 if(data)CFRelease(data);
 return data && !invalid;
}

static NSArray<NSString *> *settingsIconPaths(void) {
 NSString *root=@"/System/Library/ExtensionKit/Extensions";
 NSMutableDictionary<NSString *,NSString *> *bundles=[NSMutableDictionary dictionary];
 for(NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:root error:nil]) {
  NSString *path=[root stringByAppendingPathComponent:name];
  NSDictionary *info=[NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Contents/Info.plist"]];
  NSString *identifier=info[@"CFBundleIdentifier"];
  if(identifier && info[@"CFBundleIcons"])bundles[identifier]=path;
 }
 NSArray *sidebar=[NSArray arrayWithContentsOfFile:@"/System/Applications/System Settings.app/Contents/Resources/Sidebar124.plist"];
 NSMutableArray<NSString *> *paths=[NSMutableArray array];
 for(NSDictionary *group in sidebar)for(NSString *key in @[@"ordered_content",@"sortable_content"])
  for(NSString *identifier in group[key]) {
   NSString *path=bundles[identifier];
   if(path && ![paths containsObject:path])[paths addObject:path];
  }
 return paths;
}

static BOOL symbolPixels(NSImage *image, NSString *name) {
 NSBitmapImageRep *bitmap=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:128 pixelsHigh:128 bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
 memset(bitmap.bitmapData,0,bitmap.bytesPerRow*128);
 [NSGraphicsContext saveGraphicsState];
 [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap]];
 [image drawInRect:NSMakeRect(0,0,128,128) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1];
 [NSGraphicsContext restoreGraphicsState];
 NSUInteger visible=0;
 for(NSUInteger y=0;y<128;++y)for(NSUInteger x=0;x<128;++x)visible+=bitmap.bitmapData[y*bitmap.bytesPerRow+x*4+3]!=0;
 printf("symbol name=%s visible=%lu\n",name.UTF8String,(unsigned long)visible);
 return visible>0;
}

int main(int argc, const char **argv) {
 @autoreleasepool {
  setvbuf(stdout, NULL, _IOLBF, 0);
  id<MTLDevice> device=MTLCreateSystemDefaultDevice();
  printf("CoreImage probe device=%s mps=%s\n", device.name.UTF8String ?: "none", getenv("CI_ENABLE_MPS") ?: "default");
  if(!device)return 2;
  CIContext *context=[CIContext contextWithMTLDevice:device options:@{kCIContextWorkingFormat:@(kCIFormatRGBAh), kCIContextUseSoftwareRenderer:@NO}];
  CIFilter *checker=[CIFilter filterWithName:@"CICheckerboardGenerator" withInputParameters:@{
   @"inputColor0":[CIColor colorWithRed:1 green:0.2 blue:0.1],
   @"inputColor1":[CIColor colorWithRed:0.1 green:0.4 blue:1], @"inputWidth":@8}];
  CIImage *input=[checker.outputImage imageByCroppingToRect:CGRectMake(0,0,64,64)];
  CIImage *blur=[input imageByApplyingFilter:@"CIGaussianBlur" withInputParameters:@{kCIInputRadiusKey:@5}];
  uint8_t pixels[64*64*4]={0};
  CGColorSpaceRef space=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  [context render:blur toBitmap:pixels rowBytes:64*4 bounds:CGRectMake(0,0,64,64) format:kCIFormatRGBA8 colorSpace:space];
  uint16_t halves[64*64*4]={0};
  [context render:blur toBitmap:halves rowBytes:64*8 bounds:CGRectMake(0,0,64,64) format:kCIFormatRGBAh colorSpace:space];
  unsigned halfInvalid=0;
  for(unsigned i=0;i<64*64*4;++i)halfInvalid+=(halves[i]&0x7c00)==0x7c00;
  unsigned low=255,high=0,opaque=0;
  for(unsigned i=0;i<64*64;++i){unsigned value=pixels[4*i];low=MIN(low,value);high=MAX(high,value);opaque+=pixels[4*i+3]>0;}
  CGColorSpaceRelease(space);
  printf("CoreImage half blur pixels=%u range=%u..%u nonfinite=%u\n",opaque,low,high,halfInvalid);
  if(!opaque || high<=low || halfInvalid)return 3;
  if(argc<2)return 0;
  BOOL settings=argc==3 && !strcmp(argv[1],"--settings-icons");
  BOOL symbols=argc==4 && !strcmp(argv[1],"--icon-symbols");
  BOOL single=argc==4 && (!strcmp(argv[1],"--icon") || symbols);
  if(argc!=2 && !single && !settings)return 2;
  const char *destination=single?argv[3]:settings?argv[2]:argv[1];
  NSMutableArray<NSString *> *paths=[NSMutableArray array];
  NSFileManager *files=NSFileManager.defaultManager;
  if(settings) {
   dlopen("/System/Library/PrivateFrameworks/IconServices.framework/IconServices",RTLD_LAZY);
   for(NSString *path in settingsIconPaths()) {
    [paths addObject:path];[paths addObject:[@"iconservices:" stringByAppendingString:path]];
   }
  }
  else if(single)[paths addObject:@(argv[2])];
  else for(NSString *root in @[@"/System/Applications",@"/System/Library/PreferencePanes"]) {
   for(NSString *name in [[files contentsOfDirectoryAtPath:root error:nil] sortedArrayUsingSelector:@selector(compare:)]) {
    if([@[@"app",@"prefPane"] containsObject:name.pathExtension])[paths addObject:[root stringByAppendingPathComponent:name]];
   }
  }
  if(symbols)[paths addObjectsFromArray:@[@"wifi",@"gearshape",@"network",@"display",@"keyboard",@"battery.100percent",@"speaker.wave.2",@"hand.raised",@"person.crop.circle",@"accessibility"]];
  NSUInteger count=MIN(paths.count,64),width=MAX(MIN(count,8),1)*128,height=MAX((count+7)/8,1)*128;
  NSBitmapImageRep *sheet=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:width pixelsHigh:height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
  NSGraphicsContext *graphics=[NSGraphicsContext graphicsContextWithBitmapImageRep:sheet];
  NSMutableArray<NSImage *> *icons=[NSMutableArray array];
  BOOL available=count>0;
  [NSGraphicsContext saveGraphicsState];[NSGraphicsContext setCurrentContext:graphics];
  [[NSColor colorWithWhite:0.8 alpha:1] setFill];NSRectFill(NSMakeRect(0,0,width,height));
  for(NSUInteger i=0;i<count;++i) {
   @autoreleasepool {
    NSTimeInterval began=NSDate.timeIntervalSinceReferenceDate;
    BOOL service=[paths[i] hasPrefix:@"iconservices:"];
    NSString *path=service?[paths[i] substringFromIndex:13]:paths[i];
    NSImage *icon=service?settingsServiceIcon(path):symbols && i>0?[NSImage imageWithSystemSymbolName:path accessibilityDescription:nil]:[NSWorkspace.sharedWorkspace iconForFile:path];
    if(!icon){printf("icon index=%lu unavailable path=%s\n",(unsigned long)i,paths[i].UTF8String);available=NO;continue;}
    if(settings || (symbols && i>0))available=symbolPixels(icon,paths[i]) && available;
    if(settings) {
     NSDictionary *info=[NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Contents/Info.plist"]];
     printf("settings icon index=%lu configuration=%s\n",(unsigned long)i,[info[@"CFBundleIcons"] description].UTF8String);
     available=sourcePixels(icon,i,24) && available;
    }
    [icons addObject:icon];
    [icon drawInRect:NSMakeRect((i%8)*128,(i/8)*128,128,128) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1];
    printf("icon index=%lu ms=%.3f representations=%lu path=%s\n",(unsigned long)i,1000*(NSDate.timeIntervalSinceReferenceDate-began),(unsigned long)icon.representations.count,paths[i].UTF8String);
   }
  }
  [NSGraphicsContext restoreGraphicsState];
  NSData *png=[sheet representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
  BOOL saved=[png writeToFile:@(destination) atomically:YES];
  BOOL finite=available;
  for(NSUInteger i=0;i<icons.count;++i)finite=sourcePixels(icons[i],i,128) && finite;
  printf("icon pass count=%lu saved=%d finite=%d\n",(unsigned long)count,saved,finite);
  return !saved ? 4 : finite ? 0 : 5;
 }
}
