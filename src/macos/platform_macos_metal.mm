#include "platform_macos_present.h"

#include "gpu_metal.h"

#import <QuartzCore/CAMetalDisplayLink.h>
#import <QuartzCore/QuartzCore.h>

// CAMetalDisplayLink wants an Objective-C delegate; this one forwards each
// refresh to the presenter.
@interface MetalFrameDelegate : NSObject <CAMetalDisplayLinkDelegate>
@property(nonatomic) Presenter *presenter;
@end

struct Presenter {
    CAMetalLayer *layer;
    GpuContext gpu;
    id<MTLCommandQueue> commandQueue;
    CAMetalDisplayLink *displayLink;
    MetalFrameDelegate *frameDelegate;
    PresenterFrameCallback callback;
    void *callbackContext;
    id<CAMetalDrawable> drawable; // non-nil only while the display link callback runs
    RenderTarget target;
};

@implementation MetalFrameDelegate
- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update {
    (void)link;
    Presenter *presenter = self.presenter;
    presenter->drawable = update.drawable;
    presenter->callback(presenter->callbackContext);
    presenter->drawable = nil;
}
@end

CALayer *PresenterMakeBackingLayer() {
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.device = MTLCreateSystemDefaultDevice();
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    return layer;
}

Presenter *PresenterCreate(NSView *view, PresenterFrameCallback callback, void *context) {
    Presenter *presenter = new Presenter();
    presenter->layer = (CAMetalLayer *)view.layer;
    presenter->gpu.device = presenter->layer.device;
    presenter->gpu.colorFormat = presenter->layer.pixelFormat;
    presenter->gpu.depthFormat = MTLPixelFormatDepth32Float;
    presenter->commandQueue = [presenter->gpu.device newCommandQueue];
    presenter->callback = callback;
    presenter->callbackContext = context;
    return presenter;
}

GpuContext *PresenterGpuContext(Presenter *presenter) { return &presenter->gpu; }

// CAMetalDisplayLink pulls drawables straight from the layer, and nothing else
// resizes it, so the drawable size is set here in backing pixels.
void PresenterResize(Presenter *presenter, CGSize backingSize) {
    presenter->layer.drawableSize = backingSize;
}

// MTKView's own draw loop tops out at 120 Hz on macOS, and so does an NSView
// CADisplayLink. CAMetalDisplayLink delivers drawables at the layer's true
// display refresh (240 Hz here).
void PresenterStart(Presenter *presenter) {
    presenter->frameDelegate = [[MetalFrameDelegate alloc] init];
    presenter->frameDelegate.presenter = presenter;
    presenter->displayLink = [[CAMetalDisplayLink alloc] initWithMetalLayer:presenter->layer];
    presenter->displayLink.delegate = presenter->frameDelegate;
    [presenter->displayLink addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSRunLoopCommonModes];
}

void PresenterSetRunning(Presenter *presenter, bool running) {
    presenter->displayLink.paused = !running;
}

bool PresenterRunning(const Presenter *presenter) { return !presenter->displayLink.paused; }

void PresenterMatchScreen(Presenter *presenter, NSScreen *screen, int framesPerSecond) {
    (void)screen;
    presenter->displayLink.preferredFrameRateRange =
        CAFrameRateRangeMake((float)framesPerSecond, (float)framesPerSecond, (float)framesPerSecond);
}

RenderTarget *PresenterBeginFrame(Presenter *presenter) {
    if (presenter->drawable == nil) {
        return nullptr;
    }
    presenter->target.commandBuffer = [presenter->commandQueue commandBuffer];
    presenter->target.colorTexture = presenter->drawable.texture;
    return &presenter->target;
}

void PresenterEndFrame(Presenter *presenter) {
    [presenter->target.commandBuffer presentDrawable:presenter->drawable];
    [presenter->target.commandBuffer commit];
}
