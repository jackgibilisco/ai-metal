// The only file in this project allowed to touch AppKit. It owns the window
// and the frame-driving loop, and calls Init/FrameUpdate/FrameRender.

#import <Cocoa/Cocoa.h>
#import <MetalKit/MetalKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "app.h"
#include "arena.h"

namespace {
constexpr size_t kArenaSize = 64 * 1024 * 1024;
constexpr CGFloat kWindowWidth = 960;
constexpr CGFloat kWindowHeight = 600;
} // namespace

// Two-finger trackpad drag and pinch land here as NSEvents, hit-tested to
// whichever view is under the cursor — they don't require first-responder
// status the way key events do. Deltas are just accumulated per frame and
// handed to the renderer in drawInMTKView:, which resets them.
@interface AppMetalView : MTKView
@property(nonatomic) float pendingPanX;
@property(nonatomic) float pendingPanY;
@property(nonatomic) float pendingZoom;
@property(nonatomic) float pendingOrbitYaw;
@property(nonatomic) float pendingOrbitPitch;
@end

@implementation AppMetalView

- (void)scrollWheel:(NSEvent *)event {
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

@end

@interface AppViewDelegate : NSObject <MTKViewDelegate>
@property(nonatomic) Arena *arena;
@property(nonatomic) id<MTLCommandQueue> commandQueue;
@property(nonatomic) CFTimeInterval lastTime;
@end

@implementation AppViewDelegate

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
    (void)view;
    (void)size;
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
    };
    metalView.pendingPanX = 0.0f;
    metalView.pendingPanY = 0.0f;
    metalView.pendingZoom = 0.0f;
    metalView.pendingOrbitYaw = 0.0f;
    metalView.pendingOrbitPitch = 0.0f;

    FrameUpdate(self.arena, deltaTime, cameraInput);

    id<CAMetalDrawable> drawable = view.currentDrawable;
    MTLRenderPassDescriptor *passDescriptor = view.currentRenderPassDescriptor;
    if (drawable == nil || passDescriptor == nil) {
        return;
    }

    RenderTarget target;
    target.commandBuffer = [self.commandQueue commandBuffer];
    target.passDescriptor = passDescriptor;
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
    NSWindowStyleMask styleMask =
        NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                               styleMask:styleMask
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO];
    [self.window setTitle:@"Renderer"];
    [self.window center];

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();

    self.view = [[AppMetalView alloc] initWithFrame:frame device:device];
    self.view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    self.view.depthStencilPixelFormat = MTLPixelFormatDepth32Float;
    self.view.clearColor = MTLClearColorMake(0.05, 0.05, 0.08, 1.0);

    float aspectRatio = (float)(frame.size.width / frame.size.height);
    Init(&_arena, device, self.view.colorPixelFormat, self.view.depthStencilPixelFormat, aspectRatio);

    self.viewDelegate = [[AppViewDelegate alloc] init];
    self.viewDelegate.arena = &_arena;
    self.viewDelegate.commandQueue = [device newCommandQueue];
    self.view.delegate = self.viewDelegate;

    [self.window setContentView:self.view];
    [self.window makeKeyAndOrderFront:nil];
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
