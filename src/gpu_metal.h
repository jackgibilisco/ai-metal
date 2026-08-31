#pragma once

// Metal definitions of the opaque types in gpu.h. Included only by the Metal
// backend (renderer_metal.mm, ui_render_metal.mm) and by platform_macos.mm,
// which builds a GpuContext once and a RenderTarget each frame.

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

struct GpuContext {
    id<MTLDevice> device;
    MTLPixelFormat colorFormat;
    MTLPixelFormat depthFormat;
};

struct RenderTarget {
    id<MTLCommandBuffer> commandBuffer;
    id<CAMetalDrawable> drawable;
};
