// ── hdr_surface_macos — EDR on a Cocoa window ────────────────────────────────
// See hdr_surface.h for why this configures the layer rather than patching bgfx.
#include "runtime/platform/hdr_surface.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>

#include <cstdio>   // std::snprintf — libc++ pulls it in transitively, libstdc++ does not

namespace platwin {
namespace {

CAMetalLayer* metalLayerFor(void* nativeWindow, bool create) {
    if (!nativeWindow || ![NSThread isMainThread]) return nil;
    NSWindow* window = (__bridge NSWindow*)nativeWindow;
    NSView* view = window.contentView;
    if (!view) return nil;

    if ([view.layer isKindOfClass:[CAMetalLayer class]])
        return (CAMetalLayer*)view.layer;
    if (!create) return nil;

    // The same layer bgfx would have made for itself, made here first so it can
    // carry EDR settings. bgfx's NSWindow path checks for exactly this.
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device = MTLCreateSystemDefaultDevice();
    view.wantsLayer = YES;
    view.layer = layer;
    return layer;
}

}  // namespace

bool enableExtendedDynamicRange(void* nativeWindow) {
    @autoreleasepool {
        CAMetalLayer* layer = metalLayerFor(nativeWindow, /*create*/ true);
        if (!layer) return false;

        // RGBA16Float is one of the two drawable formats CAMetalLayer accepts,
        // and the only one with range above 1.0. bgfx sets it too, from
        // resolution.formatColor — set here as well so the layer is valid
        // extended-range even between this call and renderer init.
        layer.pixelFormat = MTLPixelFormatRGBA16Float;
        layer.wantsExtendedDynamicRangeContent = YES;

        // Extended linear sRGB: the engine's own working primaries, with values
        // above 1.0 allowed. NOT Display P3 — see the header.
        CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
        if (!cs) return false;
        layer.colorspace = cs;
        CGColorSpaceRelease(cs);
        return true;
    }
}

float extendedDynamicRangeHeadroom(void* nativeWindow) {
    @autoreleasepool {
        if (!nativeWindow || ![NSThread isMainThread]) return 1.0f;
        NSWindow* window = (__bridge NSWindow*)nativeWindow;
        NSScreen* screen = window.screen ?: NSScreen.mainScreen;
        if (!screen) return 1.0f;
        // SDR-white-relative, and it moves with the brightness slider: on an XDR
        // panel it is ~1.0 at full brightness in a bright room and rises as the
        // backlight drops. Read per frame.
        const CGFloat v = screen.maximumExtendedDynamicRangeColorComponentValue;
        return v > 1.0 ? (float)v : 1.0f;
    }
}

std::string describeHdrSurface(void* nativeWindow) {
    @autoreleasepool {
        CAMetalLayer* layer = metalLayerFor(nativeWindow, /*create*/ false);
        if (!layer) return "no CAMetalLayer on the content view";
        NSString* space = layer.colorspace
            ? (NSString*)CFBridgingRelease(CGColorSpaceCopyName(layer.colorspace))
            : @"(none)";
        NSWindow* window = (__bridge NSWindow*)nativeWindow;
        NSScreen* screen = window.screen ?: NSScreen.mainScreen;
        char buf[320];
        std::snprintf(buf, sizeof buf,
                      "CAMetalLayer edr=%s colorspace=%s pixelFormat=%lu "
                      "headroom=%.2f (potential %.2f)",
                      layer.wantsExtendedDynamicRangeContent ? "YES" : "NO",
                      space.UTF8String ? space.UTF8String : "?",
                      (unsigned long)layer.pixelFormat,
                      screen ? screen.maximumExtendedDynamicRangeColorComponentValue : 1.0,
                      screen ? screen.maximumPotentialExtendedDynamicRangeColorComponentValue : 1.0);
        return buf;
    }
}

}  // namespace platwin
