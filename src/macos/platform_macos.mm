// The AppKit platform layer both graphics backends share: the window, input,
// menus, HUD, and frame loop, calling Init/FrameUpdate/FrameRender. The
// drawable surface and presentation belong to the linked presenter
// (platform_macos_present.h); together they are the only AppKit code.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "app.h"
#include "arena.h"
#include "frame_stats.h"
#include "platform_macos_present.h"

namespace {
constexpr size_t kArenaSize = 192 * 1024 * 1024;
constexpr CGFloat kWindowWidth = 960;
constexpr CGFloat kWindowHeight = 600;

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
// in renderFrame, which resets them.
constexpr int kMaxDroppedFiles = 16;
constexpr int kMaxDropPathLength = 1024;
constexpr unsigned short kKeyCodeF3 = 99;

@interface AppContentView : NSView
@property(nonatomic) float pendingPanX;
@property(nonatomic) float pendingPanY;
@property(nonatomic) float pendingZoom;
@property(nonatomic) float pendingOrbitYaw;
@property(nonatomic) float pendingOrbitPitch;
@property(nonatomic) float pendingMagnification;
@property(nonatomic) BOOL f3Down; // held chord prefix for the debug shortcuts
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
@property(nonatomic) int pendingDropCount;
@property(nonatomic) float pendingDropX; // backing pixels, top-left origin
@property(nonatomic) float pendingDropY;
@property(nonatomic) BOOL pendingDragHovering;
@property(nonatomic) float pendingDragHoverX; // backing pixels, top-left origin
@property(nonatomic) float pendingDragHoverY;
@property(nonatomic) int pendingDragHoverCount;
@property(nonatomic) BOOL inFullscreen; // kept in sync by AppDelegate
@property(nonatomic, copy) void (^onToggleFullscreen)(void);
@property(nonatomic, copy) void (^onResize)(void); // bounds or backing scale changed
- (int)drainKeyEventsInto:(KeyEvent *)dest max:(int)max;
- (const char *const *)droppedFilePaths;
@end

@implementation AppContentView {
    KeyEvent _pendingKeyEvents[kMaxKeyEvents];
    int _pendingKeyEventCount;
    char _droppedPaths[kMaxDroppedFiles][kMaxDropPathLength];
    const char *_droppedPathPointers[kMaxDroppedFiles];
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self != nil) {
        self.wantsLayer = YES;
        [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    }
    return self;
}

- (CALayer *)makeBackingLayer {
    CALayer *layer = PresenterMakeBackingLayer();
    return layer != nil ? layer : [super makeBackingLayer];
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    if (self.onResize) {
        self.onResize();
    }
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    if (self.onResize) {
        self.onResize();
    }
}

// The drawable's height in backing pixels, for flipping AppKit's bottom-left
// points into the top-left pixels FrameInput uses.
- (CGFloat)backingHeight {
    return [self convertSizeToBacking:self.bounds.size].height;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

// macOS virtual key codes for the keys app.cpp and ui.cpp act on. They stay in
// this file; FrameInput carries the portable Key_* value instead.
static int NormalizedKeyCode(unsigned short macKeyCode) {
    switch (macKeyCode) {
    case 36: return Key_Return;
    case 53: return Key_Escape;
    case 51: return Key_Backspace;
    case 117: return Key_Delete;
    case 123: return Key_Left;
    case 124: return Key_Right;
    case kKeyCodeF3: return Key_F3;
    default: return Key_None;
    }
}

- (void)enqueueKey:(NSEvent *)event pressed:(BOOL)pressed {
    if (_pendingKeyEventCount >= kMaxKeyEvents) {
        return;
    }
    NSString *chars = event.charactersIgnoringModifiers;
    unsigned int codepoint = chars.length > 0 ? [chars characterAtIndex:0] : 0;
    if (codepoint >= 0xF700) {
        codepoint = 0; // function/arrow keys have no character
    }
    NSEventModifierFlags flags = event.modifierFlags;
    unsigned int mods = 0;
    if (flags & NSEventModifierFlagCommand) mods |= 1u;
    if (flags & NSEventModifierFlagShift) mods |= 2u;
    if (flags & NSEventModifierFlagControl) mods |= 4u;
    if (flags & NSEventModifierFlagOption) mods |= 8u;
    if (self.f3Down && event.keyCode != kKeyCodeF3) {
        mods |= 16u; // ShortcutMod_F3: F3 held as a chord prefix
    }
    _pendingKeyEvents[_pendingKeyEventCount++] =
        (KeyEvent){NormalizedKeyCode(event.keyCode), codepoint, mods, (bool)pressed};
}

- (int)drainKeyEventsInto:(KeyEvent *)dest max:(int)max {
    int count = _pendingKeyEventCount < max ? _pendingKeyEventCount : max;
    for (int i = 0; i < count; ++i) {
        dest[i] = _pendingKeyEvents[i];
    }
    _pendingKeyEventCount = 0;
    return count;
}

- (const char *const *)droppedFilePaths {
    return _droppedPathPointers;
}

- (NSArray<NSURL *> *)wavURLsFromDrag:(id<NSDraggingInfo>)sender {
    NSArray *urls = [sender.draggingPasteboard
        readObjectsForClasses:@[ [NSURL class] ]
                      options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    NSMutableArray<NSURL *> *wavs = [NSMutableArray array];
    for (NSURL *url in urls) {
        if ([url.pathExtension caseInsensitiveCompare:@"wav"] == NSOrderedSame) {
            [wavs addObject:url];
        }
    }
    return wavs;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    return [self draggingUpdated:sender];
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    int wavCount = (int)[self wavURLsFromDrag:sender].count;
    if (wavCount == 0) {
        self.pendingDragHovering = NO;
        return NSDragOperationNone;
    }
    NSPoint inView = [self convertPoint:sender.draggingLocation fromView:nil];
    NSPoint inBacking = [self convertPointToBacking:inView];
    self.pendingDragHoverX = (float)inBacking.x;
    self.pendingDragHoverY = (float)([self backingHeight] - inBacking.y);
    self.pendingDragHoverCount = wavCount;
    self.pendingDragHovering = YES;
    return NSDragOperationCopy;
}

- (void)draggingExited:(id<NSDraggingInfo>)sender {
    (void)sender;
    self.pendingDragHovering = NO;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    self.pendingDragHovering = NO;
    NSArray<NSURL *> *wavs = [self wavURLsFromDrag:sender];
    int count = (int)wavs.count;
    if (count == 0) {
        return NO;
    }
    if (count > kMaxDroppedFiles) {
        count = kMaxDroppedFiles;
    }
    for (int i = 0; i < count; ++i) {
        strlcpy(_droppedPaths[i], wavs[i].fileSystemRepresentation, kMaxDropPathLength);
        _droppedPathPointers[i] = _droppedPaths[i];
    }
    NSPoint inView = [self convertPoint:sender.draggingLocation fromView:nil];
    NSPoint inBacking = [self convertPointToBacking:inView];
    self.pendingDropX = (float)inBacking.x;
    self.pendingDropY = (float)([self backingHeight] - inBacking.y);
    self.pendingDropCount = count;
    return YES;
}

// Intercepts the View menu's Cmd-F item and any programmatic -toggleFullScreen:,
// replacing AppKit's Spaces fullscreen (which throttles the display link to
// 120 Hz) with a borderless screen-sized window.
- (void)toggleFullScreen:(id)sender {
    (void)sender;
    if (self.onToggleFullscreen) {
        self.onToggleFullscreen();
    }
}

- (void)keyDown:(NSEvent *)event {
    if (event.keyCode == kKeyCodeF3) {
        self.f3Down = YES;
    }
    if (!event.isARepeat) {
        [self enqueueKey:event pressed:YES];
    }
    if (event.keyCode == kKeyCodeF3) {
        return; // F3 is a chord prefix, never a character
    }
    if (event.keyCode == 53 && self.inFullscreen && self.onToggleFullscreen) { // Escape
        self.onToggleFullscreen();
        return;
    }
    [super keyDown:event];
}

- (void)keyUp:(NSEvent *)event {
    [self enqueueKey:event pressed:NO];
    if (event.keyCode == kKeyCodeF3) {
        self.f3Down = NO;
        return;
    }
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
    self.pendingMagnification += (float)event.magnification;
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
    self.pendingMouseY = (float)([self backingHeight] - inBacking.y);
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
// owns; renderFrame feeds it one sample per frame. hitTest: returns nil so
// camera drags pass straight through to the content view underneath.
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

// Drives one frame per display refresh: the presenter's display-link callback
// calls renderFrame.
@interface AppViewDelegate : NSObject
@property(nonatomic) Arena *arena;
@property(nonatomic) Presenter *presenter;
@property(nonatomic) CFTimeInterval lastTime;
@property(nonatomic) DebugHudView *hudView;
@property(nonatomic, weak) AppContentView *contentView;
@end

@implementation AppViewDelegate

// Sizes the presenter's drawable and the renderer's screen targets, in
// backing pixels.
- (void)resizeToBackingSize:(CGSize)size {
    if (size.width < 1.0 || size.height < 1.0) {
        return;
    }
    PresenterResize(self.presenter, size);
    FrameResize(self.arena, (float)size.width, (float)size.height);
}

- (void)renderFrame {
    CFTimeInterval now = CACurrentMediaTime();
    float deltaTime = (self.lastTime == 0) ? 0.0f : (float)(now - self.lastTime);
    self.lastTime = now;

    AppContentView *contentView = self.contentView;
    FrameInput frameInput = {
        .panX = contentView.pendingPanX,
        .panY = contentView.pendingPanY,
        .zoomDelta = contentView.pendingZoom,
        .orbitYaw = contentView.pendingOrbitYaw,
        .orbitPitch = contentView.pendingOrbitPitch,
        .magnification = contentView.pendingMagnification,
        .mouseX = contentView.pendingMouseX,
        .mouseY = contentView.pendingMouseY,
        .mouseLeftDown = (bool)contentView.pendingMouseLeftDown,
        .mouseRightDown = (bool)contentView.pendingMouseRightDown,
        .mouseMiddleDown = (bool)contentView.pendingMouseMiddleDown,
        .scrollX = contentView.pendingScrollX,
        .scrollY = contentView.pendingScrollY,
        .shift = (bool)contentView.pendingShift,
        .ctrl = (bool)contentView.pendingCtrl,
        .alt = (bool)contentView.pendingAlt,
        .cmd = (bool)contentView.pendingCmd,
        .fullscreen = (bool)contentView.inFullscreen,
    };
    frameInput.keyEventCount =
        [contentView drainKeyEventsInto:frameInput.keyEvents max:kMaxKeyEvents];
    frameInput.droppedFiles = [contentView droppedFilePaths];
    frameInput.droppedFileCount = contentView.pendingDropCount;
    frameInput.dropX = contentView.pendingDropX;
    frameInput.dropY = contentView.pendingDropY;
    frameInput.dragHovering = (bool)contentView.pendingDragHovering;
    frameInput.dragHoverX = contentView.pendingDragHoverX;
    frameInput.dragHoverY = contentView.pendingDragHoverY;
    frameInput.dragHoverFileCount = contentView.pendingDragHoverCount;
    contentView.pendingPanX = 0.0f;
    contentView.pendingPanY = 0.0f;
    contentView.pendingZoom = 0.0f;
    contentView.pendingOrbitYaw = 0.0f;
    contentView.pendingOrbitPitch = 0.0f;
    contentView.pendingScrollX = 0.0f;
    contentView.pendingScrollY = 0.0f;
    contentView.pendingMagnification = 0.0f;
    contentView.pendingDropCount = 0;

    bool needsRender = FrameUpdate(self.arena, deltaTime, frameInput);

    bool hudVisible = AppDebugHudVisible(self.arena);
    if (hudVisible == (bool)self.hudView.hidden) {
        self.hudView.hidden = !hudVisible;
        AppRequestRender(self.arena); // repopulate the frozen HUD readout
    }
    if (!needsRender) {
        return; // nothing changed: leave the last presented frame on screen
    }
    RenderTarget *target = PresenterBeginFrame(self.presenter);
    if (target == nullptr) {
        return;
    }

    if (deltaTime > 0.0f) {
        [self.hudView pushFrameTime:deltaTime];
    }

    FrameRender(self.arena, target);
    PresenterEndFrame(self.presenter);

    if (!self.hudView.hidden) {
        self.hudView.passTimings = FrameGpuTimings(self.arena);
    }
}

@end

static void PresenterFrameHook(void *context) {
    [(__bridge AppViewDelegate *)context renderFrame];
}

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate, NSMenuItemValidation> {
    Arena _arena;
    void *_arenaMemory;
}
@property(nonatomic) NSWindow *window;
@property(nonatomic) AppContentView *view;
@property(nonatomic) DebugHudView *hudView;
@property(nonatomic) AppViewDelegate *viewDelegate;
@property(nonatomic) Presenter *presenter;
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

    NSWindowStyleMask styleMask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                  NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    NSRect frame = NSMakeRect(0, 0, kWindowWidth, kWindowHeight);
    NSScreen *screen = [NSScreen mainScreen];
    if (screen != nil) {
        frame = [NSWindow contentRectForFrameRect:screen.visibleFrame styleMask:styleMask];
    }
    self.window = [[AppWindow alloc] initWithContentRect:frame
                                               styleMask:styleMask
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
    [self.window setTitle:@"Renderer"];
    [self.window setAcceptsMouseMovedEvents:YES];
    [self.window center];
    // AppKit's own fullscreen throttles the display link to 120 Hz; disable
    // it so the green button zooms and our borderless fullscreen is the only
    // fullscreen path.
    self.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone;

    // The view goes into the window before anything is sized, so its backing
    // scale (and so the drawable size) is the screen's.
    self.viewDelegate = [[AppViewDelegate alloc] init];
    self.view = [[AppContentView alloc] initWithFrame:frame];
    [self.window setContentView:self.view];
    self.presenter =
        PresenterCreate(self.view, PresenterFrameHook, (__bridge void *)self.viewDelegate);
    CGSize backingSize = [self.view convertSizeToBacking:self.view.bounds.size];
    PresenterResize(self.presenter, backingSize);

    PlatformMenuHooks menuHooks = {
        .importFile = MenuHookImportFile,
        .toggleFullscreen = MenuHookToggleFullscreen,
        .quit = MenuHookQuit,
        .context = (__bridge void *)self,
    };
    Init(&_arena, PresenterGpuContext(self.presenter), (float)backingSize.width,
         (float)backingSize.height, menuHooks);
    AppRequestRender(&_arena);

    self.hudView = [[DebugHudView alloc] initWithFrame:self.view.bounds];
    self.hudView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.hudView.hidden = YES;
    [self.view addSubview:self.hudView];

    self.viewDelegate.arena = &_arena;
    self.viewDelegate.presenter = self.presenter;
    self.viewDelegate.hudView = self.hudView;
    self.viewDelegate.contentView = self.view;

    __weak AppDelegate *weakSelf = self;
    self.view.onToggleFullscreen = ^{
        [weakSelf toggleBorderlessFullscreen];
    };
    __weak AppViewDelegate *weakViewDelegate = self.viewDelegate;
    __weak AppContentView *weakView = self.view;
    self.view.onResize = ^{
        [weakViewDelegate resizeToBackingSize:[weakView convertSizeToBacking:weakView.bounds.size]];
    };

    self.window.delegate = self;
    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.view];
    [NSApp activateIgnoringOtherApps:YES];

    // Started once the view is on screen, so the drawable surface is sized
    // and on the right display.
    PresenterStart(self.presenter);
    [self matchDisplayRefreshRate];
}

// Borderless screen-sized "fullscreen" — keeps the windowed compositor path,
// which runs the display link at the full display refresh, unlike AppKit's
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
    [self.viewDelegate resizeToBackingSize:[self.view convertSizeToBacking:self.view.bounds.size]];
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

// Pin the display link to the window's current display refresh rate, and
// hand the matching per-frame millisecond target to the HUD for its graph
// scale. Re-applied when the window is dragged to a different-rate monitor.
- (void)matchDisplayRefreshRate {
    NSScreen *screen = self.window.screen ?: [NSScreen mainScreen];
    NSInteger framesPerSecond = screen.maximumFramesPerSecond;
    if (framesPerSecond <= 0) {
        framesPerSecond = 60;
    }
    PresenterMatchScreen(self.presenter, screen, (int)framesPerSecond);
    self.hudView.targetFrameMs = 1000.0f / (float)framesPerSecond;
}

- (void)windowDidChangeScreen:(NSNotification *)notification {
    (void)notification;
    [self matchDisplayRefreshRate];
}

// Pause the display link whenever the rendered result can't be seen —
// app inactive, window minimized, or window fully occluded — and resume on
// the way back, re-priming the frame clock so the first frame back doesn't
// integrate the whole gap.
- (void)updateFrameLoopRunning {
    BOOL visible = (self.window.occlusionState & NSWindowOcclusionStateVisible) != 0;
    BOOL shouldRun = NSApp.active && visible && !self.window.miniaturized;
    if (shouldRun == PresenterRunning(self.presenter)) {
        return;
    }
    PresenterSetRunning(self.presenter, shouldRun);
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
