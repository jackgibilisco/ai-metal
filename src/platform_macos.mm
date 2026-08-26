// The only file in this project allowed to touch AppKit. It owns the window
// and the frame-driving loop, and calls Init/FrameUpdate/FrameRender.

#import <Cocoa/Cocoa.h>
#import <MetalKit/MetalKit.h>

#include "app.h"
#include "arena.h"

namespace {
constexpr size_t kArenaSize = 64 * 1024 * 1024;
constexpr CGFloat kWindowWidth = 960;
constexpr CGFloat kWindowHeight = 600;
} // namespace

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

    FrameUpdate(self.arena, deltaTime);

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
@property(nonatomic) MTKView *view;
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

    self.view = [[MTKView alloc] initWithFrame:frame device:device];
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

@end

namespace {

void InstallMainMenu() {
    NSMenu *menuBar = [[NSMenu alloc] init];
    NSMenuItem *appMenuItem = [[NSMenuItem alloc] init];
    [menuBar addItem:appMenuItem];
    [NSApp setMainMenu:menuBar];

    NSMenu *appMenu = [[NSMenu alloc] init];
    NSString *quitTitle = [@"Quit " stringByAppendingString:[[NSProcessInfo processInfo] processName]];
    NSMenuItem *quitItem = [[NSMenuItem alloc] initWithTitle:quitTitle
                                                       action:@selector(terminate:)
                                                keyEquivalent:@"q"];
    [appMenu addItem:quitItem];
    [appMenuItem setSubmenu:appMenu];
}

} // namespace

int main(int argc, const char *argv[]) {
    (void)argc;
    (void)argv;

    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        InstallMainMenu();
        AppDelegate *delegate = [[AppDelegate alloc] init];
        [NSApp setDelegate:delegate];
        [NSApp run];
    }

    return 0;
}
