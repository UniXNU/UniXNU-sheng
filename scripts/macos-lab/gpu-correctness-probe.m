// SPDX-License-Identifier: MIT
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static BOOL completed(id<MTLCommandBuffer> command) {
    [command commit]; [command waitUntilCompleted];
    if(command.status==MTLCommandBufferStatusCompleted)return YES;
    printf("correctness command status=%lu error=%s\n",(unsigned long)command.status,command.error.description.UTF8String);
    return NO;
}

static BOOL imports(id<MTLDevice> device) {
    const NSUInteger width=64,height=16,row=width*4,bytes=row*height;
    IOSurfaceRef surface=IOSurfaceCreate((__bridge CFDictionaryRef)@{
        (id)kIOSurfaceWidth:@(width),(id)kIOSurfaceHeight:@(height),
        (id)kIOSurfaceBytesPerElement:@4,(id)kIOSurfaceBytesPerRow:@(row),
        (id)kIOSurfacePixelFormat:@((uint32_t)0x52474241)});
    if(!surface)return NO;
    volatile uint8_t *pixels=IOSurfaceGetBaseAddress(surface);
    MTLTextureDescriptor *descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
    descriptor.storageMode=MTLStorageModeShared;
    descriptor.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
    id<MTLTexture> texture=[device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
    id<MTLBuffer> input=[device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> output=[device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue=[device newCommandQueue];
    unsigned cpuToGpu=0,gpuToCpu=0;
    BOOL ok=texture && input && output && queue && pixels && IOSurfaceGetBytesPerRow(surface)==row;
    for(unsigned round=0;ok && round<16;++round) {
        const uint8_t cpu=0x20+round,gpu=0x80+round;
        // Keep the import alive across CPU updates and pre-touch every cached byte before the GPU writes.
        for(NSUInteger i=0;i<bytes;++i)pixels[i]=cpu;
        memset(input.contents,gpu,bytes);
        id<MTLCommandBuffer> command=[queue commandBuffer];
        id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
        if(!blit){ok=NO;break;}
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width,height,1)
                    toBuffer:output destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:bytes];
        [blit copyFromBuffer:input sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:bytes sourceSize:MTLSizeMake(width,height,1)
                   toTexture:texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
        [blit endEncoding];
        if(!completed(command)){ok=NO;break;}
        const uint8_t *read=output.contents;
        for(NSUInteger i=0;i<bytes;++i){cpuToGpu+=read[i]!=cpu;gpuToCpu+=pixels[i]!=gpu;}
    }
    printf("correctness cached-iosurface rounds=16 bytes=%lu cpu-to-gpu-errors=%u gpu-to-cpu-errors=%u commands=%s\n",
           (unsigned long)bytes,cpuToGpu,gpuToCpu,ok?"pass":"FAIL");
    CFRelease(surface);
    return ok && !cpuToGpu && !gpuToCpu;
}

static BOOL compounds(id<MTLDevice> device,NSString *path) {
    NSData *bytes=[NSData dataWithContentsOfFile:path];
    if(!bytes){printf("correctness half input missing=%s\n",path.UTF8String);return NO;}
    dispatch_data_t data=dispatch_data_create(bytes.bytes,bytes.length,NULL,DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *error=nil;
    id<MTLLibrary> library=[device newLibraryWithData:data error:&error];
    id<MTLFunction> function=[library newFunctionWithName:@"compound"];
    id<MTLComputePipelineState> pipeline=function?[device newComputePipelineStateWithFunction:function error:&error]:nil;
    if(!pipeline){printf("correctness half pipeline error=%s\n",error.description.UTF8String);return NO;}
    const float values[]={-60000,60000,0.5,0,1,256,-65504,256};
    id<MTLBuffer> input=[device newBufferWithBytes:values length:sizeof values options:MTLResourceStorageModeShared];
    id<MTLBuffer> output=[device newBufferWithLength:16 options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue=[device newCommandQueue];
    id<MTLCommandBuffer> command=[queue commandBuffer];
    id<MTLComputeCommandEncoder> compute=[command computeCommandEncoder];
    if(!input || !output || !compute)return NO;
    [compute setComputePipelineState:pipeline];
    [compute setBuffer:input offset:0 atIndex:0]; [compute setBuffer:output offset:0 atIndex:1];
    [compute dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
    [compute endEncoding];
    BOOL ok=completed(command);
    const float *answer=output.contents;
    printf("correctness half-compound mix=%g dot=%g expected=0,32 commands=%s\n",answer[0],answer[1],ok?"pass":"FAIL");
    return ok && isfinite(answer[0]) && isfinite(answer[1]) && answer[0]==0 && answer[1]==32;
}

int main(int argc,const char **argv) {
    @autoreleasepool {
        setbuf(stdout,NULL);
        if(argc!=2)return 2;
        id<MTLDevice> device=MTLCreateSystemDefaultDevice();
        if(!device)return 2;
        printf("correctness device=%s\n",device.name.UTF8String);
        BOOL ok=imports(device);
        if(strcmp(argv[1],"--cache-only"))ok=compounds(device,@(argv[1])) && ok;
        return ok?0:1;
    }
}
