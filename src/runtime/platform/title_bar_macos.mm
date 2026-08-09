// ── title_bar_macos — Cocoa implementation of platwin::hideTitleBar ──────────
// See title_bar.h for WHY this is not an undecorated window.
#include "runtime/platform/title_bar.h"

#import <Cocoa/Cocoa.h>
#include <initializer_list>

namespace platwin {

bool hideTitleBar(void* nativeWindow) {
    if (!nativeWindow) return false;
    NSWindow* w = (__bridge NSWindow*)nativeWindow;

    // Cocoa is main-thread-only for view/window mutation. The editor calls this
    // from its boot path, which IS the main thread, but asserting the contract
    // beats discovering it from a rare, impossible-looking UI corruption.
    if (![NSThread isMainThread]) return false;

    // The three flags, and each does one thing:
    //   FullSizeContentView  content extends UP under the title bar area,
    //                        which is what actually reclaims the pixels
    //   titlebarAppearsTransparent  stop drawing the bar's background, so the
    //                        content underneath shows through
    //   titleVisibility      hide the window title text
    // The style mask keeps Titled/Closable/Miniaturizable/Resizable, so the
    // window is still a normal window to the WM: traffic lights work, the top
    // edge resizes, double-click still zooms, and Mission Control behaves.
    w.styleMask |= NSWindowStyleMaskFullSizeContentView;
    w.titlebarAppearsTransparent = YES;
    w.titleVisibility            = NSWindowTitleHidden;

    // The toolbar would re-introduce a band of chrome under the buttons.
    w.toolbar = nil;

    return true;
}

float titleBarInset(void* nativeWindow) {
    if (!nativeWindow) return 0.0f;
    NSWindow* w = (__bridge NSWindow*)nativeWindow;
    if (![NSThread isMainThread]) return 0.0f;

    // MEASURED, not hardcoded to 78. The traffic lights move: they shift with
    // the system's accessibility and appearance settings, and their spacing has
    // changed across macOS releases. Asking the buttons where they actually are
    // is the only version-proof answer.
    //
    // The inset is the RIGHT edge of the rightmost button plus a gap, expressed
    // from the window's leading edge.
    CGFloat rightMost = 0.0f;
    for (NSWindowButton b : { NSWindowCloseButton, NSWindowMiniaturizeButton,
                              NSWindowZoomButton }) {
        NSButton* btn = [w standardWindowButton:b];
        if (!btn || btn.isHidden) continue;
        const NSRect r = [btn.superview convertRect:btn.frame toView:nil];
        rightMost = MAX(rightMost, NSMaxX(r));
    }
    if (rightMost <= 0.0f) return 0.0f;      // no buttons (a panel, or hidden)

    // A gap so the first menu item does not touch the zoom button.
    return (float)rightMost + 8.0f;
}

float titleBarHeight(void* nativeWindow) {
    if (!nativeWindow) return 0.0f;
    NSWindow* w = (__bridge NSWindow*)nativeWindow;
    if (![NSThread isMainThread]) return 0.0f;

    // Derived from the window itself, not a constant. With
    // FullSizeContentView the content view spans the whole frame, so the
    // title-bar band is what `contentLayoutRect` — the area Cocoa considers
    // clear of the title bar — leaves at the top.
    const CGFloat frameH  = w.frame.size.height;
    const CGFloat layoutH = w.contentLayoutRect.size.height;
    const CGFloat band    = frameH - layoutH;
    // A window that is minimised or mid-transition can report nonsense; clamp
    // to something a menu bar can live with rather than propagating it.
    if (band <= 0.0f || band > 200.0f) return 0.0f;
    return (float)band;
}

bool beginWindowDrag(void* nativeWindow) {
    if (!nativeWindow) return false;
    NSWindow* w = (__bridge NSWindow*)nativeWindow;
    if (![NSThread isMainThread]) return false;

    // Hand the CURRENT event back to Cocoa and let it run the drag loop.
    // Deliberately not "track the mouse and setFrameOrigin each frame": that
    // reimplementation loses window snapping, the drag-to-another-display
    // behaviour, Spaces, and the release animation — and it fights the
    // compositor for every pixel.
    NSEvent* ev = NSApp.currentEvent;
    if (!ev) return false;
    [w performWindowDragWithEvent:ev];
    return true;
}

bool toggleWindowZoom(void* nativeWindow) {
    if (!nativeWindow) return false;
    NSWindow* w = (__bridge NSWindow*)nativeWindow;
    if (![NSThread isMainThread]) return false;

    // What a double-click on the title bar does is a SYSTEM PREFERENCE
    // (Appearance > "Double-click a window's title bar to"). Reading it means
    // the editor behaves like every other window on the user's machine
    // instead of assuming everyone wants zoom.
    NSString* action = [[NSUserDefaults standardUserDefaults]
                          stringForKey:@"AppleActionOnDoubleClick"];
    if ([action isEqualToString:@"Minimize"]) { [w miniaturize:nil]; return true; }
    if ([action isEqualToString:@"None"])     { return false; }
    [w zoom:nil];                       // "Maximize", and the default
    return true;
}

// The traffic lights are the window's own and outlive hiding the bar; the app
// only has to leave room for them. See titleBarInset().
bool titleBarNeedsCustomButtons() { return false; }

bool minimizeWindow(void* nativeWindow) {
    if (!nativeWindow) return false;
    NSWindow* w = (__bridge NSWindow*)nativeWindow;
    if (![NSThread isMainThread]) return false;
    [w miniaturize:nil];
    return true;
}

} // namespace platwin
