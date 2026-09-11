#include "offscreen.h"

#include "gpu_metal.h"

namespace {

GpuContext g_gpu;
id<MTLCommandQueue> g_queue;
id<MTLTexture> g_colorTexture;
RenderTarget g_target;
int g_width;
int g_height;

} // namespace

GpuContext *OffscreenInit(int width, int height) {
    g_gpu.device = MTLCreateSystemDefaultDevice();
    g_gpu.colorFormat = MTLPixelFormatBGRA8Unorm;
    g_gpu.depthFormat = MTLPixelFormatDepth32Float;
    g_queue = [g_gpu.device newCommandQueue];
    OffscreenResize(width, height);
    return &g_gpu;
}

void OffscreenResize(int width, int height) {
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:g_gpu.colorFormat
                                                          width:width
                                                         height:height
                                                      mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    g_colorTexture = [g_gpu.device newTextureWithDescriptor:descriptor];
    g_width = width;
    g_height = height;
}

RenderTarget *OffscreenBeginFrame() {
    g_target.commandBuffer = [g_queue commandBuffer];
    g_target.colorTexture = g_colorTexture;

    MTLRenderPassDescriptor *clear = [MTLRenderPassDescriptor renderPassDescriptor];
    clear.colorAttachments[0].texture = g_colorTexture;
    clear.colorAttachments[0].loadAction = MTLLoadActionClear;
    clear.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.0, 1.0, 1.0);
    clear.colorAttachments[0].storeAction = MTLStoreActionStore;
    [[g_target.commandBuffer renderCommandEncoderWithDescriptor:clear] endEncoding];
    return &g_target;
}

void OffscreenEndFrame(uint8_t *rgbaTopLeft) {
    [g_target.commandBuffer commit];
    [g_target.commandBuffer waitUntilCompleted];
    if (rgbaTopLeft == nullptr) {
        return;
    }

    [g_colorTexture getBytes:rgbaTopLeft
                 bytesPerRow:(NSUInteger)g_width * 4
                  fromRegion:MTLRegionMake2D(0, 0, g_width, g_height)
                 mipmapLevel:0];
    int pixelCount = g_width * g_height;
    for (int i = 0; i < pixelCount; ++i) {
        uint8_t blue = rgbaTopLeft[i * 4 + 0];
        rgbaTopLeft[i * 4 + 0] = rgbaTopLeft[i * 4 + 2];
        rgbaTopLeft[i * 4 + 2] = blue;
    }
}
