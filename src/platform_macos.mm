// The only file in this project allowed to touch AppKit. It owns the window
// and the frame-driving loop, and calls Init/FrameUpdate/FrameRender.

#import <Cocoa/Cocoa.h>
#import <MetalKit/MetalKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "app.h"
#include "arena.h"
#include "frame_stats.h"

namespace {
constexpr size_t kArenaSize = 64 * 1024 * 1024;
constexpr CGFloat kWindowWidth = 960;
constexpr CGFloat kWindowHeight = 600;
constexpr MTLPixelFormat kDepthFormat = MTLPixelFormatDepth32Float;

// A mouse wheel reports coarse line-based deltas (~1 per notch); scale them
// into the same magnification range a trackpad pinch produces.
constexpr float kMouseWheelZoom = 0.05f;
} // namespace

// Trackpad gestures (two-finger drag, pinch) and mouse events (right-drag,
// middle-drag, wheel) both land here as NSEvents, hit-tested to whichever view
// is under the cursor — they don't require first-responder status the way key
// events do. Deltas are just accumulated per frame and handed to the renderer
// in drawInMTKView:, which resets them.
@interface AppMetalView : MTKView
@property(nonatomic) float pendingPanX;
@property(nonatomic) float pendingPanY;
@property(nonatomic) float pendingZoom;
@property(nonatomic) float pendingOrbitYaw;
@property(nonatomic) float pendingOrbitPitch;
@property(nonatomic) BOOL pendingCycleDebug;
@property(nonatomic) BOOL pendingToggleFxaa;
@property(nonatomic) BOOL pendingToggleHud;
@end

@implementation AppMetalView

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)keyDown:(NSEvent *)event {
    if ([event.charactersIgnoringModifiers isEqualToString:@"o"]) {
        self.pendingCycleDebug = YES;
        return;
    }
    if ([event.charactersIgnoringModifiers isEqualToString:@"f"]) {
        self.pendingToggleFxaa = YES;
        return;
    }
    if (event.keyCode == 99) { // F3
        self.pendingToggleHud = YES;
        return;
    }
    [super keyDown:event];
}

- (void)scrollWheel:(NSEvent *)event {
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
    (void)event;
}

- (void)otherMouseDown:(NSEvent *)event {
    (void)event;
}

- (void)rightMouseDragged:(NSEvent *)event {
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
    self.pendingOrbitYaw += (float)event.deltaX;
    self.pendingOrbitPitch += (float)event.deltaY;
}

@end

// A transparent overlay pinned over the whole content view. It draws a
// frame-time readout and graph in its top-left corner from a FrameStats it
// owns; drawInMTKView: feeds it one sample per frame. hitTest: returns nil so
// camera drags pass straight through to the MTKView underneath.
@interface DebugHudView : NSView
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
    const float maxMs = 50.0f;
    CGFloat bottom = rect.origin.y + rect.size.height;

    [[NSColor colorWithWhite:1.0 alpha:0.12] set];
    NSRectFill(rect);

    [[NSColor colorWithWhite:1.0 alpha:0.25] set];
    NSRectFill(NSMakeRect(rect.origin.x, bottom - rect.size.height * (1000.0f / 60.0f / maxMs),
                          rect.size.width, 1));
    NSRectFill(NSMakeRect(rect.origin.x, bottom - rect.size.height * (1000.0f / 30.0f / maxMs),
                          rect.size.width, 1));

    for (int i = 0; i < _stats.count; ++i) {
        float ms = FrameStatsSample(&_stats, i);
        CGFloat barHeight = rect.size.height * std::min(ms / maxMs, 1.0f);
        CGFloat x = rect.origin.x + rect.size.width - _stats.count + i;
        NSColor *color = (ms <= 1000.0f / 60.0f)   ? [NSColor systemGreenColor]
                         : (ms <= 1000.0f / 30.0f) ? [NSColor systemYellowColor]
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

    NSString *text = [NSString stringWithFormat:@"FPS %.0f\navg %.2f ms\n1%% low %.0f fps  %.2f ms",
                                                fps, avgMs, lowFps, lowMs];
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

@interface AppViewDelegate : NSObject <MTKViewDelegate>
@property(nonatomic) Arena *arena;
@property(nonatomic) id<MTLCommandQueue> commandQueue;
@property(nonatomic) CFTimeInterval lastTime;
@property(nonatomic) DebugHudView *hudView;
@end

@implementation AppViewDelegate

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
    (void)view;
    FrameResize(self.arena, (float)size.width, (float)size.height);
}

- (void)drawInMTKView:(MTKView *)view {
    CFTimeInterval now = CACurrentMediaTime();
    float deltaTime = (self.lastTime == 0) ? 0.0f : (float)(now - self.lastTime);
    self.lastTime = now;

    AppMetalView *metalView = (AppMetalView *)view;
    CameraInput cameraInput = {
        .panX = metalView.pendingPanX,
        .panY = metalView.pendingPanY,
        .zoomDelta = metalView.pendingZoom,
        .orbitYaw = metalView.pendingOrbitYaw,
        .orbitPitch = metalView.pendingOrbitPitch,
        .cycleDebugView = (bool)metalView.pendingCycleDebug,
        .toggleFxaa = (bool)metalView.pendingToggleFxaa,
    };
    metalView.pendingPanX = 0.0f;
    metalView.pendingPanY = 0.0f;
    metalView.pendingZoom = 0.0f;
    metalView.pendingOrbitYaw = 0.0f;
    metalView.pendingOrbitPitch = 0.0f;
    metalView.pendingCycleDebug = NO;
    metalView.pendingToggleFxaa = NO;

    if (metalView.pendingToggleHud) {
        self.hudView.hidden = !self.hudView.hidden;
        metalView.pendingToggleHud = NO;
    }
    if (deltaTime > 0.0f) {
        [self.hudView pushFrameTime:deltaTime];
    }

    FrameUpdate(self.arena, deltaTime, cameraInput);

    id<CAMetalDrawable> drawable = view.currentDrawable;
    if (drawable == nil) {
        return;
    }

    RenderTarget target;
    target.commandBuffer = [self.commandQueue commandBuffer];
    target.drawable = drawable;

    FrameRender(self.arena, target);
}

@end

@interface AppDelegate : NSObject <NSApplicationDelegate> {
    Arena _arena;
    void *_arenaMemory;
}
@property(nonatomic) NSWindow *window;
@property(nonatomic) AppMetalView *view;
@property(nonatomic) DebugHudView *hudView;
@property(nonatomic) AppViewDelegate *viewDelegate;
@end

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
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                               styleMask:styleMask
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
    [self.window setTitle:@"Renderer"];
    [self.window center];

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();

    self.view = [[AppMetalView alloc] initWithFrame:frame device:device];
    self.view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;

    CGSize drawableSize = self.view.drawableSize;
    Init(&_arena, device, self.view.colorPixelFormat, kDepthFormat, (float)drawableSize.width,
         (float)drawableSize.height);

    self.hudView = [[DebugHudView alloc] initWithFrame:self.view.bounds];
    self.hudView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.hudView.hidden = YES;
    [self.view addSubview:self.hudView];

    self.viewDelegate = [[AppViewDelegate alloc] init];
    self.viewDelegate.arena = &_arena;
    self.viewDelegate.commandQueue = [device newCommandQueue];
    self.viewDelegate.hudView = self.hudView;
    self.view.delegate = self.viewDelegate;

    [self.window setContentView:self.view];
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.view];
    [NSApp activateIgnoringOtherApps:YES];
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

    if ([panel runModal] != NSModalResponseOK) {
        return;
    }
    NSURL *url = panel.URLs.firstObject;
    if (url == nil) {
        return;
    }
    ImportBlendFile(&_arena, url.fileSystemRepresentation);
}

@end

namespace {

void InstallMainMenu(AppDelegate *delegate) {
    NSMenu *menuBar = [[NSMenu alloc] init];
    NSMenuItem *appMenuItem = [[NSMenuItem alloc] init];
    [menuBar addItem:appMenuItem];

    NSMenuItem *fileMenuItem = [[NSMenuItem alloc] initWithTitle:@"File"
                                                            action:nil
                                                     keyEquivalent:@""];
    [menuBar addItem:fileMenuItem];

    [NSApp setMainMenu:menuBar];

    NSMenu *appMenu = [[NSMenu alloc] init];
    NSString *quitTitle = [@"Quit " stringByAppendingString:[[NSProcessInfo processInfo] processName]];
    NSMenuItem *quitItem = [[NSMenuItem alloc] initWithTitle:quitTitle
                                                       action:@selector(terminate:)
                                                keyEquivalent:@"q"];
    [appMenu addItem:quitItem];
    [appMenuItem setSubmenu:appMenu];

    NSMenu *fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
    NSMenuItem *importItem = [[NSMenuItem alloc] initWithTitle:@"Import File…"
                                                          action:@selector(importBlendFile:)
                                                   keyEquivalent:@""];
    [importItem setTarget:delegate];
    [fileMenu addItem:importItem];
    [fileMenuItem setSubmenu:fileMenu];
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
