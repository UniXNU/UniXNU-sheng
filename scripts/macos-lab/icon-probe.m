#import <AppKit/AppKit.h>
#import <CoreImage/CoreImage.h>
#import <Metal/Metal.h>

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
  unsigned low=255,high=0,opaque=0;
  for(unsigned i=0;i<64*64;++i){unsigned value=pixels[4*i];low=MIN(low,value);high=MAX(high,value);opaque+=pixels[4*i+3]>0;}
  CGColorSpaceRelease(space);
  printf("CoreImage half blur pixels=%u range=%u..%u\n",opaque,low,high);
  if(!opaque || high<=low)return 3;
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
  [NSGraphicsContext saveGraphicsState];[NSGraphicsContext setCurrentContext:graphics];
  [[NSColor colorWithWhite:0.8 alpha:1] setFill];NSRectFill(NSMakeRect(0,0,width,height));
  for(NSUInteger i=0;i<count;++i) {
   @autoreleasepool {
    NSTimeInterval began=NSDate.timeIntervalSinceReferenceDate;
    NSImage *icon=[NSWorkspace.sharedWorkspace iconForFile:paths[i]];
    [icon drawInRect:NSMakeRect((i%8)*128,(i/8)*128,128,128) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1];
    printf("icon index=%lu ms=%.3f representations=%lu path=%s\n",(unsigned long)i,1000*(NSDate.timeIntervalSinceReferenceDate-began),(unsigned long)icon.representations.count,paths[i].UTF8String);
   }
  }
  [NSGraphicsContext restoreGraphicsState];
  NSData *png=[sheet representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
  BOOL saved=[png writeToFile:@(argv[1]) atomically:YES];
  printf("icon pass count=%lu saved=%d\n",(unsigned long)count,saved);
  return saved ? 0 : 4;
 }
}
