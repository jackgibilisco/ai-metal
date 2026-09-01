// The only file in this project allowed to touch AppKit. It owns the window
// and the frame-driving loop, and calls Init/FrameUpdate/FrameRender.

#import <Cocoa/Cocoa.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>
#import <QuartzCore/CAMetalDisplayLink.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "app.h"
#include "arena.h"
#include "frame_stats.h"
#include "gpu_metal.h"

namespace {
constexpr size_t kArenaSize = 192 * 1024 * 1024;
constexpr CGFloat kWindowWidth = 960;
constexpr CGFloat kWindowHeight = 600;
constexpr MTLPixelFormat kDepthFormat = MTLPixelFormatDepth32Float;

// A mouse wheel reports coarse line-based deltas (~1 per notch); scale them
// into the same magnification range a trackpad pinch produces.
constexpr float kMouseWheelZoom = 0.05f;
} // namespace

// A borderless window can't become key by default, which would kill keyboard
// input in borderless fullscreen; force it.
@interface AppWindow : NSWindow
@end

@implementation AppWindow
- (BOOL)canBecomeKeyWindow {
    return YES;
}
- (BOOL)canBecomeMainWindow {
    return YES;
}
@end

// Trackpad gestures (two-finger drag, pinch) and mouse events (right-drag,
// middle-drag, wheel) both land here as NSEvents, hit-tested to whichever view
// is under the cursor — they don't require first-responder status the way key
// events do. Deltas are just accumulated per frame and handed to the renderer
// in renderIntoDrawable:, which resets them.
@interface AppMetalView : MTKView
@property(nonatomic) float pendingPanX;
@property(nonatomic) float pendingPanY;
@property(nonatomic) float pendingZoom;
@property(nonatomic) float pendingOrbitYaw;
@property(nonatomic) float pendingOrbitPitch;
@property(nonatomic) BOOL pendingCycleDebug;
@property(nonatomic) BOOL pendingToggleFxaa;
@property(nonatomic) BOOL pendingToggleHud;
@property(nonatomic) float pendingMouseX; // backing pixels, top-left origin
@property(nonatomic) float pendingMouseY;
@property(nonatomic) BOOL pendingMouseLeftDown;
@property(nonatomic) BOOL pendingMouseRightDown;
@property(nonatomic) BOOL pendingMouseMiddleDown;
@property(nonatomic) float pendingScrollX; // UI scroll, separate from camera pan/zoom
@property(nonatomic) float pendingScrollY;
@property(nonatomic) BOOL pendingShift;
@property(nonatomic) BOOL pendingCtrl;
@property(nonatomic) BOOL pendingAlt;
@property(nonatomic) BOOL pendingCmd;
@property(nonatomic) BOOL inFullscreen; // kept in sync by AppDelegate
@property(nonatomic, copy) void (^onToggleFullscreen)(void);
- (int)drainKeyEventsInto:(KeyEvent *)dest max:(int)max;
@end

@implementation AppMetalView {
    KeyEvent _pendingKeyEvents[kMaxKeyEvents];
    int _pendingKeyEventCount;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)enqueueKey:(NSEvent *)event pressed:(BOOL)pressed {
    if (_pendingKeyEventCount >= kMaxKeyEvents) {
        return;
    }
    NSString *chars = event.charactersIgnoringModifiers;
    unsigned int codepoint = chars.length > 0 ? [chars characterAtIndex:0] : 0;
    NSEventModifierFlags flags = event.modifierFlags;
    unsigned int mods = 0;
    if (flags & NSEventModifierFlagCommand) mods |= 1u;
    if (flags & NSEventModifierFlagShift) mods |= 2u;
    if (flags & NSEventModifierFlagControl) mods |= 4u;
    if (flags & NSEventModifierFlagOption) mods |= 8u;
    _pendingKeyEvents[_pendingKeyEventCount++] =
        (KeyEvent){(int)event.keyCode, codepoint, mods, (bool)pressed};
}

- (int)drainKeyEventsInto:(KeyEvent *)dest max:(int)max {
    int count = _pendingKeyEventCount < max ? _pendingKeyEventCount : max;
    for (int i = 0; i < count; ++i) {
        dest[i] = _pendingKeyEvents[i];
    }
    _pendingKeyEventCount = 0;
    return count;
}

// Intercepts the View menu's Cmd-F item and any programmatic -toggleFullScreen:,
// replacing AppKit's Spaces fullscreen (which throttles CAMetalDisplayLink to
// 120 Hz) with a borderless screen-sized window.
- (void)toggleFullScreen:(id)sender {
    (void)sender;
    if (self.onToggleFullscreen) {
        self.onToggleFullscreen();
    }
}

- (void)keyDown:(NSEvent *)event {
    if (!event.isARepeat) {
        [self enqueueKey:event pressed:YES];
    }

    // The bare-letter debug toggles must not also fire when a modifier is
    // held — Cmd-F is a command shortcut (fullscreen), not the FXAA toggle.
    bool plainKey = (event.modifierFlags &
                     (NSEventModifierFlagCommand | NSEventModifierFlagControl |
                      NSEventModifierFlagOption)) == 0;
    if (plainKey && [event.charactersIgnoringModifiers isEqualToString:@"o"]) {
        self.pendingCycleDebug = YES;
        return;
    }
    if (plainKey && [event.charactersIgnoringModifiers isEqualToString:@"f"]) {
        self.pendingToggleFxaa = YES;
        return;
    }
    if (event.keyCode == 99) { // F3
        self.pendingToggleHud = YES;
        return;
    }
    if (event.keyCode == 53 && self.inFullscreen && self.onToggleFullscreen) { // Escape
        self.onToggleFullscreen();
        return;
    }
    [super keyDown:event];
}

- (void)keyUp:(NSEvent *)event {
    [self enqueueKey:event pressed:NO];
    [super keyUp:event];
}

- (void)flagsChanged:(NSEvent *)event {
    NSEventModifierFlags flags = event.modifierFlags;
    self.pendingShift = (flags & NSEventModifierFlagShift) != 0;
    self.pendingCtrl = (flags & NSEventModifierFlagControl) != 0;
    self.pendingAlt = (flags & NSEventModifierFlagOption) != 0;
    self.pendingCmd = (flags & NSEventModifierFlagCommand) != 0;
    [super flagsChanged:event];
}

- (void)scrollWheel:(NSEvent *)event {
    self.pendingScrollX += (float)event.scrollingDeltaX;
    self.pendingScrollY += (float)event.scrollingDeltaY;

    if (!event.hasPreciseScrollingDeltas) {
        self.pendingZoom += (float)event.scrollingDeltaY * kMouseWheelZoom;
        return;
    }
    bool orbiting = (event.modifierFlags & NSEventModifierFlagShift) != 0;
    if (orbiting) {
        self.pendingOrbitYaw += (float)event.scrollingDeltaX;
        self.pendingOrbitPitch += (float)event.scrollingDeltaY;
    } else {
        self.pendingPanX += (float)event.scrollingDeltaX;
        self.pendingPanY += (float)event.scrollingDeltaY;
    }
}

- (void)magnifyWithEvent:(NSEvent *)event {
    self.pendingZoom += (float)event.magnification;
}

- (void)rightMouseDown:(NSEvent *)event {
    [self trackMouse:event];
    self.pendingMouseRightDown = YES;
}

- (void)rightMouseUp:(NSEvent *)event {
    [self trackMouse:event];
    self.pendingMouseRightDown = NO;
}

- (void)otherMouseDown:(NSEvent *)event {
    [self trackMouse:event];
    self.pendingMouseMiddleDown = YES;
}

- (void)otherMouseUp:(NSEvent *)event {
    [self trackMouse:event];
    self.pendingMouseMiddleDown = NO;
}

// The UI reads an absolute cursor position in backing pixels with a top-left
// origin, matching the drawable. convertPointToBacking: yields bottom-left
// pixels, so flip Y against the drawable height.
- (void)trackMouse:(NSEvent *)event {
    NSPoint inView = [self convertPoint:event.locationInWindow fromView:nil];
    NSPoint inBacking = [self convertPointToBacking:inView];
    self.pendingMouseX = (float)inBacking.x;
    self.pendingMouseY = (float)(self.drawableSize.height - inBacking.y);
}

- (void)mouseDown:(NSEvent *)event {
    [self trackMouse:event];
    self.pendingMouseLeftDown = YES;
}

- (void)mouseUp:(NSEvent *)event {
    [self trackMouse:event];
    self.pendingMouseLeftDown = NO;
}

- (void)mouseDragged:(NSEvent *)event {
    [self trackMouse:event];
}

- (void)mouseMoved:(NSEvent *)event {
    [self trackMouse:event];
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea *area in [self.trackingAreas copy]) {
        [self removeTrackingArea:area];
    }
    NSTrackingArea *area = [[NSTrackingArea alloc]
        initWithRect:self.bounds
             options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:area];
}

- (void)rightMouseDragged:(NSEvent *)event {
    [self trackMouse:event];
    bool orbiting = (event.modifierFlags & NSEventModifierFlagShift) != 0;
    if (orbiting) {
        self.pendingOrbitYaw += (float)event.deltaX;
        self.pendingOrbitPitch += (float)event.deltaY;
    } else {
        self.pendingPanX += (float)event.deltaX;
        self.pendingPanY += (float)event.deltaY;
    }
}

- (void)otherMouseDragged:(NSEvent *)event {
    [self trackMouse:event];
    self.pendingOrbitYaw += (float)event.deltaX;
    self.pendingOrbitPitch += (float)event.deltaY;
}

@end

// A transparent overlay pinned over the whole content view. It draws a
// frame-time readout and graph in its top-left corner from a FrameStats it
// owns; renderIntoDrawable: feeds it one sample per frame. hitTest: returns nil so
// camera drags pass straight through to the MTKView underneath.
@interface DebugHudView : NSView
@property(nonatomic) float targetFrameMs; // one display refresh; sets the graph scale
@property(nonatomic) RendererPassTimings passTimings; // last frame's per-pass GPU time
- (void)pushFrameTime:(float)deltaSeconds;
@end

@implementation DebugHudView {
    FrameStats _stats;
    CFTimeInterval _lastRedraw;
}

- (BOOL)isFlipped {
    return YES;
}

- (NSView *)hitTest:(NSPoint)point {
    (void)point;
    return nil;
}

- (void)pushFrameTime:(float)deltaSeconds {
    FrameStatsPush(&_stats, deltaSeconds);
    CFTimeInterval now = CACurrentMediaTime();
    if (now - _lastRedraw > 0.066) {
        _lastRedraw = now;
        self.needsDisplay = YES;
    }
}

- (void)drawGraphInRect:(NSRect)rect {
    float targetMs = _targetFrameMs > 0.0f ? _targetFrameMs : 1000.0f / 60.0f;
    float maxMs = targetMs * 3.0f;
    CGFloat bottom = rect.origin.y + rect.size.height;

    [[NSColor colorWithWhite:1.0 alpha:0.12] set];
    NSRectFill(rect);

    [[NSColor colorWithWhite:1.0 alpha:0.25] set];
    NSRectFill(NSMakeRect(rect.origin.x, bottom - rect.size.height * (targetMs / maxMs),
                          rect.size.width, 1));
    NSRectFill(NSMakeRect(rect.origin.x, bottom - rect.size.height * (2.0f * targetMs / maxMs),
                          rect.size.width, 1));

    for (int i = 0; i < _stats.count; ++i) {
        float ms = FrameStatsSample(&_stats, i);
        CGFloat barHeight = rect.size.height * std::min(ms / maxMs, 1.0f);
        CGFloat x = rect.origin.x + rect.size.width - _stats.count + i;
        NSColor *color = (ms <= targetMs)          ? [NSColor systemGreenColor]
                         : (ms <= 2.0f * targetMs) ? [NSColor systemYellowColor]
                                                   : [NSColor systemRedColor];
        [color set];
        NSRectFill(NSMakeRect(x, bottom - barHeight, 1, barHeight));
    }
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;

    float fps = 1000.0f / std::max(FrameStatsMeanMs(&_stats, 20), 0.001f);
    float avgMs = FrameStatsMeanMs(&_stats, FrameStats::kCapacity);
    float lowMs = FrameStatsOnePercentLowMs(&_stats);
    float lowFps = 1000.0f / std::max(lowMs, 0.001f);

    float displayHz = _targetFrameMs > 0.0f ? 1000.0f / _targetFrameMs : 0.0f;
    RendererPassTimings gpu = _passTimings;
    NSString *text = [NSString
        stringWithFormat:@"FPS %.0f  (%.0f Hz)\navg %.2f ms\n1%% low %.0f fps  %.2f ms\n"
                          "GPU %.2f ms\n geo %.2f  ao %.2f  lit %.2f  fxaa %.2f",
                         fps, displayHz, avgMs, lowFps, lowMs, gpu.totalMs, gpu.geometryMs,
                         gpu.aoMs, gpu.lightingMs, gpu.fxaaMs];
    NSDictionary *attributes = @{
        NSFontAttributeName : [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightMedium],
        NSForegroundColorAttributeName : [NSColor whiteColor],
    };
    NSSize textSize = [text sizeWithAttributes:attributes];

    const CGFloat margin = 10;
    const CGFloat graphWidth = FrameStats::kCapacity;
    const CGFloat graphHeight = 64;
    CGFloat panelWidth = std::max((CGFloat)textSize.width, graphWidth) + margin * 2;
    CGFloat panelHeight = textSize.height + graphHeight + margin * 3;

    [[NSColor colorWithWhite:0.0 alpha:0.55] set];
    NSRectFill(NSMakeRect(0, 0, panelWidth, panelHeight));
    [text drawAtPoint:NSMakePoint(margin, margin) withAttributes:attributes];
    [self drawGraphInRect:NSMakeRect(margin, margin * 2 + textSize.height, graphWidth, graphHeight)];
}

@end

// MTKView is kept only as a configured CAMetalLayer host and resize hook; its
// own draw loop is paused (it caps at 120 Hz on macOS). Frames are driven by
// a CAMetalDisplayLink in AppDelegate, which calls renderIntoDrawable: with a
// drawable from the layer's real display refresh.
@interface AppViewDelegate : NSObject <MTKViewDelegate>
@property(nonatomic) Arena *arena;
@property(nonatomic) id<MTLCommandQueue> commandQueue;
@property(nonatomic) CFTimeInterval lastTime;
@property(nonatomic) DebugHudView *hudView;
@property(nonatomic, weak) AppMetalView *metalView;
@end

@implementation AppViewDelegate

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
    (void)view;
    [self resizeToDrawableSize:size];
}

// The paused MTKView no longer syncs its CAMetalLayer's drawableSize on its
// own, and CAMetalDisplayLink pulls drawables straight from that layer, so
// set it here (in backing pixels) alongside the renderer's screen targets.
- (void)resizeToDrawableSize:(CGSize)size {
    if (size.width < 1.0 || size.height < 1.0) {
        return;
    }
    ((CAMetalLayer *)self.metalView.layer).drawableSize = size;
    FrameResize(self.arena, (float)size.width, (float)size.height);
}

- (void)drawInMTKView:(MTKView *)view {
    (void)view;
}

- (void)renderIntoDrawable:(id<CAMetalDrawable>)drawable {
    CFTimeInterval now = CACurrentMediaTime();
    float deltaTime = (self.lastTime == 0) ? 0.0f : (float)(now - self.lastTime);
    self.lastTime = now;

    AppMetalView *metalView = self.metalView;
    FrameInput frameInput = {
        .panX = metalView.pendingPanX,
        .panY = metalView.pendingPanY,
        .zoomDelta = metalView.pendingZoom,
        .orbitYaw = metalView.pendingOrbitYaw,
        .orbitPitch = metalView.pendingOrbitPitch,
        .cycleDebugView = (bool)metalView.pendingCycleDebug,
        .toggleFxaa = (bool)metalView.pendingToggleFxaa,
        .mouseX = metalView.pendingMouseX,
        .mouseY = metalView.pendingMouseY,
        .mouseLeftDown = (bool)metalView.pendingMouseLeftDown,
        .mouseRightDown = (bool)metalView.pendingMouseRightDown,
        .mouseMiddleDown = (bool)metalView.pendingMouseMiddleDown,
        .scrollX = metalView.pendingScrollX,
        .scrollY = metalView.pendingScrollY,
        .shift = (bool)metalView.pendingShift,
        .ctrl = (bool)metalView.pendingCtrl,
        .alt = (bool)metalView.pendingAlt,
        .cmd = (bool)metalView.pendingCmd,
        .fullscreen = (bool)metalView.inFullscreen,
    };
    frameInput.keyEventCount =
        [metalView drainKeyEventsInto:frameInput.keyEvents max:kMaxKeyEvents];
    metalView.pendingPanX = 0.0f;
    metalView.pendingPanY = 0.0f;
    metalView.pendingZoom = 0.0f;
    metalView.pendingOrbitYaw = 0.0f;
    metalView.pendingOrbitPitch = 0.0f;
    metalView.pendingScrollX = 0.0f;
    metalView.pendingScrollY = 0.0f;
    metalView.pendingCycleDebug = NO;
    metalView.pendingToggleFxaa = NO;

    if (metalView.pendingToggleHud) {
        self.hudView.hidden = !self.hudView.hidden;
        metalView.pendingToggleHud = NO;
        AppRequestRender(self.arena); // repopulate the frozen HUD readout
    }

    bool needsRender = FrameUpdate(self.arena, deltaTime, frameInput);
    if (!needsRender || drawable == nil) {
        return; // nothing changed: leave the last presented frame on screen
    }

    if (deltaTime > 0.0f) {
        [self.hudView pushFrameTime:deltaTime];
    }

    RenderTarget target;
    target.commandBuffer = [self.commandQueue commandBuffer];
    target.drawable = drawable;

    FrameRender(self.arena, &target);
    [target.commandBuffer presentDrawable:target.drawable];
    [target.commandBuffer commit];

    if (!self.hudView.hidden) {
        self.hudView.passTimings = FrameGpuTimings(self.arena);
    }
}

@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate,
                                   CAMetalDisplayLinkDelegate, NSMenuItemValidation> {
    Arena _arena;
    void *_arenaMemory;
}
@property(nonatomic) NSWindow *window;
@property(nonatomic) AppMetalView *view;
@property(nonatomic) DebugHudView *hudView;
@property(nonatomic) AppViewDelegate *viewDelegate;
@property(nonatomic) CAMetalDisplayLink *metalDisplayLink;
@property(nonatomic) BOOL borderlessFullscreen;
@property(nonatomic) NSRect windowedFrame;
@property(nonatomic) NSWindowStyleMask windowedStyleMask;
- (void)toggleBorderlessFullscreen;
- (void)importBlendFile:(id)sender;
@end

// Trampolines so portable menu dispatch can trigger the AppKit-only actions
// through PlatformMenuHooks function pointers.
static void MenuHookImportFile(void *context) {
    [(__bridge AppDelegate *)context importBlendFile:nil];
}
static void MenuHookToggleFullscreen(void *context) {
    [(__bridge AppDelegate *)context toggleBorderlessFullscreen];
}
static void MenuHookQuit(void *context) {
    (void)context;
    [NSApp terminate:nil];
}

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;

    // Allocated once at startup, zeroed so the ARC-managed Metal object
    // pointers placed inside it start as nil, and never freed — the OS
    // reclaims it when the process exits.
    _arenaMemory = calloc(1, kArenaSize);
    _arena = ArenaCreate(_arenaMemory, kArenaSize);

    NSRect frame = NSMakeRect(0, 0, kWindowWidth, kWindowHeight);
    NSWindowStyleMask styleMask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                  NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    self.window = [[AppWindow alloc] initWithContentRect:frame
                                               styleMask:styleMask
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
    [self.window setTitle:@"Renderer"];
    [self.window setAcceptsMouseMovedEvents:YES];
    [self.window center];
    // AppKit's own fullscreen throttles CAMetalDisplayLink to 120 Hz; disable
    // it so the green button zooms and our borderless fullscreen is the only
    // fullscreen path.
    self.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone;

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();

    self.view = [[AppMetalView alloc] initWithFrame:frame device:device];
    self.view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;

    CGSize drawableSize = self.view.drawableSize;
    PlatformMenuHooks menuHooks = {
        .importFile = MenuHookImportFile,
        .toggleFullscreen = MenuHookToggleFullscreen,
        .quit = MenuHookQuit,
        .context = (__bridge void *)self,
    };
    GpuContext gpu = {
        .device = device,
        .colorFormat = self.view.colorPixelFormat,
        .depthFormat = kDepthFormat,
    };
    Init(&_arena, &gpu, (float)drawableSize.width, (float)drawableSize.height, menuHooks);
    AppRequestRender(&_arena);

    self.hudView = [[DebugHudView alloc] initWithFrame:self.view.bounds];
    self.hudView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.hudView.hidden = YES;
    [self.view addSubview:self.hudView];

    self.viewDelegate = [[AppViewDelegate alloc] init];
    self.viewDelegate.arena = &_arena;
    self.viewDelegate.commandQueue = [device newCommandQueue];
    self.viewDelegate.hudView = self.hudView;
    self.viewDelegate.metalView = self.view;
    self.view.delegate = self.viewDelegate;

    __weak AppDelegate *weakSelf = self;
    self.view.onToggleFullscreen = ^{
        [weakSelf toggleBorderlessFullscreen];
    };

    // MTKView's own draw loop tops out at 120 Hz on macOS, and so does an
    // NSView CADisplayLink. Pause the view and drive frames from a
    // CAMetalDisplayLink, which delivers drawables at the layer's true
    // display refresh (240 Hz here). Created after the view is in a window
    // so its layer is sized and on the right screen.
    self.view.paused = YES;
    self.view.enableSetNeedsDisplay = NO;

    self.window.delegate = self;
    [self.window setContentView:self.view];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.view];
    [NSApp activateIgnoringOtherApps:YES];

    self.metalDisplayLink =
        [[CAMetalDisplayLink alloc] initWithMetalLayer:(CAMetalLayer *)self.view.layer];
    self.metalDisplayLink.delegate = self;
    [self.metalDisplayLink addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSRunLoopCommonModes];

    [self matchDisplayRefreshRate];
}

// Borderless screen-sized "fullscreen" — keeps the windowed compositor path,
// which runs CAMetalDisplayLink at the full display refresh, unlike AppKit's
// Spaces fullscreen.
- (void)toggleBorderlessFullscreen {
    self.borderlessFullscreen = !self.borderlessFullscreen;
    if (self.borderlessFullscreen) {
        self.windowedFrame = self.window.frame;
        self.windowedStyleMask = self.window.styleMask;
        [NSApp setPresentationOptions:NSApplicationPresentationHideDock |
                                      NSApplicationPresentationHideMenuBar];
        [self.window setStyleMask:NSWindowStyleMaskBorderless];
        self.window.hasShadow = NO;
        [self.window setLevel:NSMainMenuWindowLevel + 1];
        // Overhang the screen by 1px on every side. A borderless window that
        // covers the display *exactly* triggers macOS's fullscreen bypass
        // (direct scanout), which double-buffers and pins us to 120 Hz on a
        // 240 Hz display; overhanging keeps the normal compositor path at the
        // full refresh, and the 1px is clipped off-screen so nothing shows.
        [self.window setFrame:NSInsetRect(self.window.screen.frame, -1.0, -1.0) display:YES];
    } else {
        [NSApp setPresentationOptions:NSApplicationPresentationDefault];
        [self.window setLevel:NSNormalWindowLevel];
        self.window.hasShadow = YES;
        [self.window setStyleMask:self.windowedStyleMask];
        [self.window setFrame:self.windowedFrame display:YES];
    }
    self.view.inFullscreen = self.borderlessFullscreen;
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.view];
    [self.viewDelegate resizeToDrawableSize:[self.view convertSizeToBacking:self.view.bounds.size]];
    [self matchDisplayRefreshRate];
}

// The green zoom button: run our borderless fullscreen instead of a normal
// zoom (AppKit's own fullscreen is disabled via collectionBehavior).
- (BOOL)windowShouldZoom:(NSWindow *)window toFrame:(NSRect)newFrame {
    (void)window;
    (void)newFrame;
    [self toggleBorderlessFullscreen];
    return NO;
}

- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update {
    (void)link;
    [self.viewDelegate renderIntoDrawable:update.drawable];
}

// Pin the display link to the window's current display refresh rate, and
// hand the matching per-frame millisecond target to the HUD for its graph
// scale. Re-applied when the window is dragged to a different-rate monitor.
- (void)matchDisplayRefreshRate {
    NSScreen *screen = self.window.screen ?: [NSScreen mainScreen];
    NSInteger framesPerSecond = screen.maximumFramesPerSecond;
    if (framesPerSecond <= 0) {
        framesPerSecond = 60;
    }
    self.metalDisplayLink.preferredFrameRateRange =
        CAFrameRateRangeMake((float)framesPerSecond, (float)framesPerSecond, (float)framesPerSecond);
    self.hudView.targetFrameMs = 1000.0f / (float)framesPerSecond;
}

- (void)windowDidChangeScreen:(NSNotification *)notification {
    (void)notification;
    [self matchDisplayRefreshRate];
}

// Pause the CAMetalDisplayLink whenever the rendered result can't be seen —
// app inactive, window minimized, or window fully occluded — and resume on
// the way back, re-priming the frame clock so the first frame back doesn't
// integrate the whole gap.
- (void)updateFrameLoopRunning {
    BOOL visible = (self.window.occlusionState & NSWindowOcclusionStateVisible) != 0;
    BOOL shouldRun = NSApp.active && visible && !self.window.miniaturized;
    if (shouldRun == !self.metalDisplayLink.paused) {
        return;
    }
    self.metalDisplayLink.paused = !shouldRun;
    if (shouldRun) {
        self.viewDelegate.lastTime = 0;
        AppRequestRender(&_arena);
    }
}

- (void)windowDidChangeOcclusionState:(NSNotification *)notification {
    (void)notification;
    [self updateFrameLoopRunning];
}

- (void)windowDidMiniaturize:(NSNotification *)notification {
    (void)notification;
    [self updateFrameLoopRunning];
}

- (void)windowDidDeminiaturize:(NSNotification *)notification {
    (void)notification;
    [self updateFrameLoopRunning];
}

- (void)applicationDidBecomeActive:(NSNotification *)notification {
    (void)notification;
    [self updateFrameLoopRunning];
}

- (void)applicationDidResignActive:(NSNotification *)notification {
    (void)notification;
    [self updateFrameLoopRunning];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return YES;
}

- (void)importBlendFile:(id)sender {
    (void)sender;

    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.allowedContentTypes = @[ [UTType typeWithFilenameExtension:@"blend"] ];
    panel.allowsMultipleSelection = NO;
    panel.canChooseDirectories = NO;

    // Present as an async sheet, not runModal: opening the panel spins up an
    // XPC service on first use, and runModal blocks the main thread through
    // that round-trip, freezing the display-link frame loop for a few frames.
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse result) {
        if (result != NSModalResponseOK) {
            return;
        }
        NSURL *url = panel.URLs.firstObject;
        if (url == nil) {
            return;
        }
        ImportBlendFile(&self->_arena, url.fileSystemRepresentation);
    }];
}

// Every native menu item carries its CommandId in its tag and routes here,
// through the same command table the in-app strip and keybindings use.
- (void)dispatchMenuAction:(NSMenuItem *)sender {
    if (_arenaMemory == NULL) {
        return;
    }
    AppInvokeCommand(&_arena, (CommandId)sender.tag);
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (_arenaMemory == NULL) {
        return YES;
    }
    const Command *command = CommandById((CommandId)item.tag);
    if (command == nullptr) {
        return YES;
    }
    CommandContext ctx = AppCommandContext(&_arena);
    if (command->isChecked != nullptr) {
        item.state = command->isChecked(ctx) ? NSControlStateValueOn : NSControlStateValueOff;
    }
    return command->isEnabled == nullptr || command->isEnabled(ctx);
}

@end

namespace {

// Build the real menu bar from the portable layout + command table. menus[0]
// is shown by macOS as the application menu (its title is replaced with the
// app name). Native items carry no keyEquivalent: the app's own keybinding
// matcher (FrameUpdate scanning FrameInput.keyEvents) is the sole shortcut
// handler, so shortcuts also work on a platform with no NSMenu.
void InstallMainMenu(AppDelegate *delegate) {
    MenuBar layout = MenuBarDefault();
    NSMenu *menuBar = [[NSMenu alloc] init];

    for (int i = 0; i < layout.menuCount; ++i) {
        NSMenuItem *containerItem = [[NSMenuItem alloc] init];
        [menuBar addItem:containerItem];

        NSMenu *submenu =
            [[NSMenu alloc] initWithTitle:[NSString stringWithUTF8String:layout.menus[i].title]];
        [containerItem setSubmenu:submenu];

        for (int j = 0; j < layout.menus[i].entryCount; ++j) {
            CommandId entry = layout.menus[i].entries[j];
            if (entry == kMenuSeparator) {
                [submenu addItem:[NSMenuItem separatorItem]];
                continue;
            }
            const Command *command = CommandById(entry);
            if (command == nullptr) {
                continue;
            }
            NSMenuItem *nsItem =
                [[NSMenuItem alloc] initWithTitle:[NSString stringWithUTF8String:command->label]
                                          action:@selector(dispatchMenuAction:)
                                   keyEquivalent:@""];
            nsItem.target = delegate;
            nsItem.tag = command->id;
            [submenu addItem:nsItem];
        }
    }

    [NSApp setMainMenu:menuBar];
}

} // namespace

int main(int argc, const char *argv[]) {
    (void)argc;
    (void)argv;

    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        AppDelegate *delegate = [[AppDelegate alloc] init];
        InstallMainMenu(delegate);
        [NSApp setDelegate:delegate];
        [NSApp run];
    }

    return 0;
}
