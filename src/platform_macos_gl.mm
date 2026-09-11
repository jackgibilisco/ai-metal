#include "gpu_gl.h" // first: silences the GL deprecation warnings before AppKit's NSOpenGL.h

#include "platform_macos_present.h"

#import <CoreVideo/CoreVideo.h>

#include <atomic>

// CVDisplayLink is deprecated as of macOS 15, but it is the only display
// callback that runs at the full refresh rate for a GL-backed view.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

struct Presenter {
    NSView *view;
    NSOpenGLContext *context;
    GpuContext gpu;
    RenderTarget target;
    CVDisplayLinkRef displayLink;
    std::atomic<bool> framePending;
    PresenterFrameCallback callback;
    void *callbackContext;
};

namespace {

// Fires on CVDisplayLink's own thread. The app is single-threaded, so each
// refresh hops to the main queue; framePending drops refreshes that land
// while a frame is already queued or running.
CVReturn DisplayLinkFired(CVDisplayLinkRef displayLink, const CVTimeStamp *now,
                          const CVTimeStamp *outputTime, CVOptionFlags flagsIn,
                          CVOptionFlags *flagsOut, void *context) {
    (void)displayLink;
    (void)now;
    (void)outputTime;
    (void)flagsIn;
    (void)flagsOut;
    Presenter *presenter = (Presenter *)context;
    if (presenter->framePending.exchange(true)) {
        return kCVReturnSuccess;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        presenter->callback(presenter->callbackContext);
        presenter->framePending = false;
    });
    return kCVReturnSuccess;
}

} // namespace

CALayer *PresenterMakeBackingLayer() { return nil; }

Presenter *PresenterCreate(NSView *view, PresenterFrameCallback callback, void *context) {
    NSOpenGLPixelFormatAttribute attributes[] = {
        NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion4_1Core,
        NSOpenGLPFADoubleBuffer,  NSOpenGLPFAAccelerated,
        NSOpenGLPFAColorSize,     24,
        NSOpenGLPFAAlphaSize,     8,
        0,
    };
    NSOpenGLPixelFormat *pixelFormat = [[NSOpenGLPixelFormat alloc] initWithAttributes:attributes];

    Presenter *presenter = new Presenter();
    presenter->view = view;
    presenter->context = [[NSOpenGLContext alloc] initWithFormat:pixelFormat shareContext:nil];
    presenter->callback = callback;
    presenter->callbackContext = context;
    view.wantsBestResolutionOpenGLSurface = YES;

    GLint swapInterval = 0; // the display link paces frames
    [presenter->context setValues:&swapInterval forParameter:NSOpenGLContextParameterSwapInterval];
    // Current from here on: Init creates the renderer's GL objects.
    [presenter->context makeCurrentContext];

    CVDisplayLinkCreateWithActiveCGDisplays(&presenter->displayLink);
    CVDisplayLinkSetOutputCallback(presenter->displayLink, DisplayLinkFired, presenter);
    return presenter;
}

GpuContext *PresenterGpuContext(Presenter *presenter) { return &presenter->gpu; }

void PresenterResize(Presenter *presenter, CGSize backingSize) {
    [presenter->context update];
    presenter->target.width = (int)backingSize.width;
    presenter->target.height = (int)backingSize.height;
}

// Attaching the context needs the view to be in a window.
void PresenterStart(Presenter *presenter) {
    presenter->context.view = presenter->view;
    [presenter->context update];
    CVDisplayLinkStart(presenter->displayLink);
}

void PresenterSetRunning(Presenter *presenter, bool running) {
    if (running) {
        CVDisplayLinkStart(presenter->displayLink);
    } else {
        CVDisplayLinkStop(presenter->displayLink);
    }
}

bool PresenterRunning(const Presenter *presenter) {
    return CVDisplayLinkIsRunning(presenter->displayLink);
}

void PresenterMatchScreen(Presenter *presenter, NSScreen *screen, int framesPerSecond) {
    (void)framesPerSecond;
    CGDirectDisplayID display = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
    CVDisplayLinkSetCurrentCGDisplay(presenter->displayLink, display);
}

RenderTarget *PresenterBeginFrame(Presenter *presenter) {
    [presenter->context makeCurrentContext];
    presenter->target.framebuffer = 0;
    return &presenter->target;
}

void PresenterEndFrame(Presenter *presenter) { [presenter->context flushBuffer]; }
