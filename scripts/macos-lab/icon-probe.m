#import <AppKit/AppKit.h>
#import <CoreImage/CoreImage.h>
#import <Metal/Metal.h>

static BOOL sourcePixels(NSImage *icon, NSUInteger index) {
 NSRect bounds=NSMakeRect(0,0,128,128);
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
 printf("icon source index=%lu size=%zux%zu bpc=%u bpp=%u bitmap=%#x nonfinite=%u\n",(unsigned long)index,width,height,bpc,bpp,(unsigned)info,invalid);
 if(invalid)printf("icon nonfinite index=%lu nan=%u,%u,%u,%u inf=%u,%u,%u,%u\n",(unsigned long)index,
  nans[0],nans[1],nans[2],nans[3],infinities[0],infinities[1],infinities[2],infinities[3]);
 if(data)CFRelease(data);
 return data && !invalid;
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
  NSMutableArray<NSString *> *paths=[NSMutableArray array];
  NSFileManager *files=NSFileManager.defaultManager;
  for(NSString *root in @[@"/System/Applications",@"/System/Library/PreferencePanes"]) {
   for(NSString *name in [[files contentsOfDirectoryAtPath:root error:nil] sortedArrayUsingSelector:@selector(compare:)]) {
    if([@[@"app",@"prefPane"] containsObject:name.pathExtension])[paths addObject:[root stringByAppendingPathComponent:name]];
   }
  }
  NSUInteger count=MIN(paths.count,64),width=8*128,height=MAX((count+7)/8,1)*128;
  NSBitmapImageRep *sheet=[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:width pixelsHigh:height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
  NSGraphicsContext *graphics=[NSGraphicsContext graphicsContextWithBitmapImageRep:sheet];
  NSMutableArray<NSImage *> *icons=[NSMutableArray array];
  [NSGraphicsContext saveGraphicsState];[NSGraphicsContext setCurrentContext:graphics];
  [[NSColor colorWithWhite:0.8 alpha:1] setFill];NSRectFill(NSMakeRect(0,0,width,height));
  for(NSUInteger i=0;i<count;++i) {
   @autoreleasepool {
    NSTimeInterval began=NSDate.timeIntervalSinceReferenceDate;
    NSImage *icon=[NSWorkspace.sharedWorkspace iconForFile:paths[i]];
    [icons addObject:icon];
    [icon drawInRect:NSMakeRect((i%8)*128,(i/8)*128,128,128) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1];
    printf("icon index=%lu ms=%.3f representations=%lu path=%s\n",(unsigned long)i,1000*(NSDate.timeIntervalSinceReferenceDate-began),(unsigned long)icon.representations.count,paths[i].UTF8String);
   }
  }
  [NSGraphicsContext restoreGraphicsState];
  NSData *png=[sheet representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
  BOOL saved=[png writeToFile:@(argv[1]) atomically:YES];
  BOOL finite=YES;
  for(NSUInteger i=0;i<icons.count;++i)finite=sourcePixels(icons[i],i) && finite;
  printf("icon pass count=%lu saved=%d finite=%d\n",(unsigned long)count,saved,finite);
  return !saved ? 4 : finite ? 0 : 5;
 }
}
