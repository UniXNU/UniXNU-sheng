// SPDX-License-Identifier: MIT
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <stdio.h>
#include <string.h>

int main(int argc, const char **argv) {
    @autoreleasepool {
        CAFrameRateRange rate = CAFrameRateRangeMake(120, 120, 120);
        if (argc == 2 && strcmp(argv[1], "--describe") == 0) {
            CABasicAnimation *animation = [CABasicAnimation animationWithKeyPath:@"transform.translation.x"];
            animation.preferredFrameRateRange = rate;
            rate = animation.preferredFrameRateRange;
            printf("scene layers=20 duration=360 frame-rate=%.0f/%.0f/%.0f\n", rate.minimum, rate.maximum, rate.preferred);
            return 0;
        }
        NSApplication *app = NSApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSWindow *window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(80, 80, 1050, 700)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                      NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
            backing:NSBackingStoreBuffered defer:NO];
        window.title = @"UniXNU GPU frame workload";
        window.releasedWhenClosed = NO;
        window.contentView.wantsLayer = YES;
        CALayer *root = window.contentView.layer;
        root.backgroundColor = [NSColor colorWithCalibratedRed:0.08 green:0.10 blue:0.14 alpha:1].CGColor;
        for (unsigned index = 0; index < 20; index++) {
            CALayer *layer = [CALayer layer];
            layer.frame = NSMakeRect(30 + (index % 5) * 130, 35 + (index / 5) * 145, 100, 100);
            layer.backgroundColor = [NSColor colorWithCalibratedRed:0.15 + index * 0.025 green:0.55 blue:0.75 alpha:0.8].CGColor;
            [root addSublayer:layer];
            CABasicAnimation *motion = [CABasicAnimation animationWithKeyPath:@"transform.translation.x"];
            motion.fromValue = @0;
            motion.toValue = @260;
            motion.duration = 2;
            motion.autoreverses = YES;
            motion.repeatCount = 1000;
            motion.preferredFrameRateRange = rate;
            [layer addAnimation:motion forKey:@"translation"];
        }
        [app finishLaunching];
        [window makeKeyAndOrderFront:nil];
        [app activateIgnoringOtherApps:YES];
        printf("scene start layers=20 duration=360 preferred-fps=120\n");
        fflush(stdout);
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:360]];
        [window close];
        printf("scene complete\n");
    }
    return 0;
}
