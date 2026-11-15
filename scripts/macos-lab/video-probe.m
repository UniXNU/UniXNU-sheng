// SPDX-License-Identifier: MIT
#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

struct Results { atomic_uint decoded, dropped, errors; };

static void decoded(void *context, void *frame, OSStatus status, VTDecodeInfoFlags flags,
                    CVImageBufferRef image, CMTime pts, CMTime duration) {
    (void)frame; (void)pts; (void)duration;
    struct Results *results = context;
    if (status) {
        atomic_fetch_add(&results->errors, 1);
        printf("video-probe callback status=%d flags=%u\n", (int)status, (unsigned)flags);
    } else if (image) atomic_fetch_add(&results->decoded, 1);
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
        OSStatus status = 0;
        CFAbsoluteTime started = CFAbsoluteTimeGetCurrent();
        CMSampleBufferRef sample;
        while ((sample = [output copyNextSampleBuffer])) {
            if (!session) {
                CMVideoFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
                CMVideoDimensions size = CMVideoFormatDescriptionGetDimensions(format);
                FourCharCode codec = CMFormatDescriptionGetMediaSubType(format);
                NSDictionary *spec = required ? @{(__bridge NSString *)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES}
                    : @{(__bridge NSString *)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder: @(!software)};
                status = VTDecompressionSessionCreate(kCFAllocatorDefault, format, (__bridge CFDictionaryRef)spec,
                    NULL, &callback, &session);
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
        printf("video-probe submitted=%u decoded=%u dropped=%u errors=%u seconds=%.6f reader=%ld error=%s\n",
            submitted, count, atomic_load(&results.dropped), errors, CFAbsoluteTimeGetCurrent() - started,
            (long)reader.status, reader.error.description.UTF8String ?: "");
        return status || errors || !count || count != submitted || reader.status == AVAssetReaderStatusFailed ? 1 : 0;
    }
}
