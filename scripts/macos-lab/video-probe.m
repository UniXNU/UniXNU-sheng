// SPDX-License-Identifier: MIT
#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

struct Results { atomic_uint decoded, dropped, errors; int width, height; };

static void samplePixels(CVImageBufferRef image,CMTime pts,const struct Results *results) {
    OSType format=CVPixelBufferGetPixelFormatType(image);
    printf("video-probe pixels format=0x%08x size=%zux%zu coded=%dx%d pts=%.6f\n",(unsigned)format,
           CVPixelBufferGetWidth(image),CVPixelBufferGetHeight(image),results->width,results->height,CMTimeGetSeconds(pts));
    BOOL ten=format==kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange || format==kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
    if(!ten && format!=kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange && format!=kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)return;
    if(CVPixelBufferLockBaseAddress(image,kCVPixelBufferLock_ReadOnly))return;
    for(size_t plane=0;plane<2;++plane) {
        const uint8_t *base=CVPixelBufferGetBaseAddressOfPlane(image,plane);
        size_t stride=CVPixelBufferGetBytesPerRowOfPlane(image,plane);
        size_t height=MIN(CVPixelBufferGetHeightOfPlane(image,plane),(size_t)results->height/(plane?2:1));
        size_t width=MIN(CVPixelBufferGetWidthOfPlane(image,plane)*(plane?2:1),(size_t)results->width);
        size_t xStep=MAX((size_t)16,((width+39)/40+1)&~(size_t)1);
        size_t yStep=MAX((size_t)16,((size_t)results->height+23)/24);
        for(size_t y=0;base && y<height;y+=yStep) {
            printf("video-probe grid plane=%zu row=%zu step=%zu hex=",plane,y,xStep);
            for(size_t x=0;x<width;x+=xStep)for(size_t channel=0;channel<(plane?2:1) && x+channel<width;++channel) {
                const uint8_t *at=base+y*stride+(x+channel)*(ten?2:1);
                if(ten){uint16_t value;memcpy(&value,at,2);printf("%04x",value>>6);}
                else printf("%02x",*at);
            }
            putchar('\n');
        }
    }
    CVPixelBufferUnlockBaseAddress(image,kCVPixelBufferLock_ReadOnly);
}

static void decoded(void *context, void *frame, OSStatus status, VTDecodeInfoFlags flags,
                    CVImageBufferRef image, CMTime pts, CMTime duration) {
    (void)frame; (void)duration;
    struct Results *results = context;
    if (status) {
        atomic_fetch_add(&results->errors, 1);
        printf("video-probe callback status=%d flags=%u\n", (int)status, (unsigned)flags);
    } else if (image) {
        if(!atomic_fetch_add(&results->decoded,1))samplePixels(image,pts,results);
    }
    else atomic_fetch_add(&results->dropped, 1);
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc < 2 || argc > 3) {
            fprintf(stderr, "usage: video-probe FILE [--require-hardware|--software]\n");
            return 2;
        }
        setbuf(stdout, NULL);
        BOOL required = argc == 3 && !strcmp(argv[2], "--require-hardware");
        BOOL software = argc == 3 && !strcmp(argv[2], "--software");
        if (argc == 3 && !required && !software) return 2;
        AVURLAsset *asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(argv[1])] options:nil];
        AVAssetTrack *track = [[asset tracksWithMediaType:AVMediaTypeVideo] firstObject];
        NSError *error = nil;
        AVAssetReader *reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
        if (!track || !reader) {
            printf("video-probe asset error=%s\n", error.description.UTF8String ?: "no video track");
            return 1;
        }
        AVAssetReaderTrackOutput *output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:nil];
        output.alwaysCopiesSampleData = NO;
        if (![reader canAddOutput:output]) return 1;
        [reader addOutput:output];
        if (![reader startReading]) {
            printf("video-probe reader error=%s\n", reader.error.description.UTF8String);
            return 1;
        }
        struct Results results = {0};
        VTDecompressionOutputCallbackRecord callback = {decoded, &results};
        VTDecompressionSessionRef session = NULL;
        unsigned submitted = 0;
        unsigned markers = 0;
        OSStatus status = 0;
        CFAbsoluteTime started = CFAbsoluteTimeGetCurrent();
        CMSampleBufferRef sample;
        while ((sample = [output copyNextSampleBuffer])) {
            if (!CMSampleBufferGetNumSamples(sample)) {
                ++markers;
                CFRelease(sample);
                continue;
            }
            if (!session) {
                CMVideoFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
                printf("video-probe sample valid=%d ready=%d samples=%ld bytes=%zu format=%p\n",
                    CMSampleBufferIsValid(sample), CMSampleBufferDataIsReady(sample),
                    (long)CMSampleBufferGetNumSamples(sample), CMSampleBufferGetTotalSampleSize(sample), format);
                if (!format) format = (__bridge CMVideoFormatDescriptionRef)track.formatDescriptions.firstObject;
                CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format);
                results.width=size.width; results.height=size.height;
                FourCharCode codec = CMFormatDescriptionGetMediaSubType(format);
                NSDictionary *extensions=(__bridge NSDictionary *)CMFormatDescriptionGetExtensions(format);
                NSDictionary *atoms=extensions[(__bridge NSString *)kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms];
                NSData *configuration=atoms[@"hvcC"];
                BOOL ten=configuration.length>18 && ((((const uint8_t *)configuration.bytes)[17]&7)!=0);
                NSDictionary *attributes=@{(__bridge NSString *)kCVPixelBufferPixelFormatTypeKey:
                    @(ten?kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
                    (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey:@{}};
                NSDictionary *spec = required ? @{(__bridge NSString *)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES}
                    : @{(__bridge NSString *)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder: @(!software)};
                status = VTDecompressionSessionCreate(kCFAllocatorDefault, format, (__bridge CFDictionaryRef)spec,
                    (__bridge CFDictionaryRef)attributes, &callback, &session);
                printf("video-probe codec=0x%08x size=%dx%d mode=%s create=%d\n", (unsigned)codec,
                    (int)size.width, (int)size.height, required ? "required" : software ? "software" : "enabled", (int)status);
                if (status) { CFRelease(sample); break; }
                CFTypeRef hardware = NULL;
                OSStatus property = VTSessionCopyProperty(session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
                    kCFAllocatorDefault, &hardware);
                printf("video-probe hardware-property=%d hardware=%d\n", (int)property,
                    hardware && CFGetTypeID(hardware) == CFBooleanGetTypeID() ? CFBooleanGetValue(hardware) : -1);
                if (hardware) CFRelease(hardware);
            }
            status = VTDecompressionSessionDecodeFrame(session, sample, kVTDecodeFrame_EnableAsynchronousDecompression, NULL, NULL);
            CFRelease(sample);
            if (status) { printf("video-probe submit=%u status=%d\n", submitted, (int)status); break; }
            ++submitted;
        }
        if (session) {
            OSStatus finish = VTDecompressionSessionFinishDelayedFrames(session);
            OSStatus wait = VTDecompressionSessionWaitForAsynchronousFrames(session);
            printf("video-probe finish=%d wait=%d\n", (int)finish, (int)wait);
            if (!status) status = finish ? finish : wait;
            VTDecompressionSessionInvalidate(session);
            CFRelease(session);
        }
        unsigned count = atomic_load(&results.decoded), errors = atomic_load(&results.errors);
        printf("video-probe submitted=%u decoded=%u dropped=%u errors=%u markers=%u seconds=%.6f reader=%ld error=%s\n",
            submitted, count, atomic_load(&results.dropped), errors, markers, CFAbsoluteTimeGetCurrent() - started,
            (long)reader.status, reader.error.description.UTF8String ?: "");
        return status || errors || !count || count != submitted || reader.status == AVAssetReaderStatusFailed ? 1 : 0;
    }
}
