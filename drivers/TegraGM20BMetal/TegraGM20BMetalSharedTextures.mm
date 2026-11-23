// SPDX-License-Identifier: MIT
// Shared texture handles, in Apple's drivers' object model: a shared texture is an IOSurface texture of the device
// whose surface's creation properties hold its descriptor and the device's registry ID (shared_texture.h), made by
// newSharedTextureWithDescriptor: as _MTLDevice makes it over copyIOSurfaceSharedTextureProperties: (AGX's override);
// any texture over such a surface is shareable (IOGPUMetalTexture), and its handle is Metal's own
// MTLSharedTextureHandle over the surface, which finds the receiving process's device by that ID and crosses processes
// by XPC or mach port. The device's registry ID is the IORegistry entry ID of the one accelerator service with a Metal
// plugin (TegraGM20BMetalPlugin.mm), the same kernel object in every process.
#import <objc/message.h>
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalSharedTexture.h"

@interface MTLSharedTextureHandle (Surface)
- (instancetype)initWithIOSurface:(IOSurfaceRef)surface label:(NSString *)label;
- (IOSurfaceRef)ioSurface;
@end

namespace {

constexpr const gm20b::shared::Kind &kKind = gm20b::shared::kThisKind;

// kIOSurfaceCreationProperties, which no header declares
NSDictionary *creationProperties(IOSurfaceRef s)
{
    id p = CFBridgingRelease(IOSurfaceCopyValue(s, CFSTR("CreationProperties")));
    return [p isKindOfClass:[NSDictionary class]] ? p : nil;
}

MTLSharedTextureHandle *handleOver(IOSurfaceRef s, NSString *label)
{
    static const bool made = [MTLSharedTextureHandle instancesRespondToSelector:@selector(initWithIOSurface:label:)];
    if (!made) {
        SWITCH_REFUSE("gm20b", "texture.shared.handle",
                      "Metal's MTLSharedTextureHandle has no initWithIOSurface:label:");
        return nil;
    }
    MTLSharedTextureHandle *h = [[MTLSharedTextureHandle alloc] initWithIOSurface:s label:label];
    if (!h)
        SWITCH_REFUSE_DETAIL("gm20b", "texture.shared.device", IOSurfaceGetID(s),
                             "IOSurface %u: Metal found no device of its registry ID for its handle",
                             IOSurfaceGetID(s));
    return h;
}

bool rotated(MTLTextureDescriptor *d)
{
    static const SEL rotation = NSSelectorFromString(@"rotation");
    return [d respondsToSelector:rotation] && ((NSUInteger (*)(id, SEL))objc_msgSend)(d, rotation);
}

// The image a shared texture of `d` is in its surface; null when it has one, else why not
const char *sharedImage(MTLTextureDescriptor *d, gm20b::SurfaceImage &image)
{
    const gm20b::FormatInfo *f = gm20b::formatInfo(uint32_t(d.pixelFormat));
    if (d.storageMode == MTLStorageModeMemoryless)
        return "memoryless";
    if (rotated(d))
        return "rotated";
    gm20b::SurfaceImageFacts facts;
    facts.type = uint32_t(d.textureType);
    facts.width = uint32_t(std::min<NSUInteger>(d.width, UINT32_MAX));
    facts.height = uint32_t(std::min<NSUInteger>(d.height, UINT32_MAX));
    facts.depth = uint32_t(std::min<NSUInteger>(d.depth, UINT32_MAX));
    facts.levels = uint32_t(std::min<NSUInteger>(d.mipmapLevelCount, UINT32_MAX));
    facts.samples = uint32_t(std::min<NSUInteger>(d.sampleCount, UINT32_MAX));
    facts.arrayLength = uint32_t(std::min<NSUInteger>(d.arrayLength, UINT32_MAX));
    facts.bpp = f ? f->bpp : 0;
    facts.blockW = f ? f->blockW : 1;
    facts.blockH = f ? f->blockH : 1;
    facts.target = f && f->rt;
    facts.extended = gm20b::xrSampled(uint32_t(d.pixelFormat));
    facts.rendered = volumeRendered(d);
    if (const char *why = gm20b::surfaceImage(facts, image))
        return why;
    if (image.kind != gm20b::SurfaceImage::Plain)
        return nullptr;
    if (!gm20bShape(d))
        return "a one-level 2D texture of a format the driver keeps no IOSurface texture of";
    if (gm20b::twoPlaneXr(uint32_t(d.pixelFormat)))
        return "an extended-range format, which a surface holds in two planes";
    return nullptr;
}

id<MTLTexture> openShared(id<MTLDevice> device, MTLSharedTextureHandle *handle, const NSUInteger *resourceIndex)
{
    IOSurfaceRef s = [handle respondsToSelector:@selector(ioSurface)] ? [handle ioSurface] : nullptr;
    if (!s) {
        SWITCH_REFUSE("gm20b", "texture.shared.open", "newSharedTextureWithHandle: a handle with no IOSurface");
        return nil;
    }
    NSDictionary *p = creationProperties(s);
    MTLTextureDescriptor *d = p ? gm20bSharedTextureDescriptor(p, device.registryID, kKind) : nil;
    if (!d) {
        SWITCH_REFUSE_DETAIL("gm20b", "texture.shared.open", IOSurfaceGetID(s),
                             "newSharedTextureWithHandle: IOSurface %u holds no shared texture of registry ID 0x%llx",
                             IOSurfaceGetID(s), (unsigned long long)device.registryID);
        return nil;
    }
    if (resourceIndex && [d respondsToSelector:gm20bSharedSetter("resourceIndex")])
        [d setValue:@(*resourceIndex) forKey:@"resourceIndex"];
    id<MTLTexture> t = [device newTextureWithDescriptor:d iosurface:s plane:0];
    if ([(id)t isKindOfClass:[SwitchMetalTexture class]])
        ((SwitchMetalTexture *)t)->_shareable = SwitchMetalShareable::Yes;
    t.label = handle.label;
    return t;
}

}  // namespace

bool laidOutSurface(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane, gm20b::SurfaceImage &image)
{
    if (plane || IOSurfaceGetPlaneCount(s) || sharedImage(d, image) || image.kind == gm20b::SurfaceImage::Plain ||
        IOSurfaceGetAllocSize(s) < image.layout.bytes)
        return false;
    NSDictionary *p = creationProperties(s);
    return p[gm20bSharedString(gm20b::shared::kRegistryKey)] && p[gm20bSharedString(gm20b::shared::kLayoutKey)];
}

// Its methods go onto the device class of a kind whose _MTLDevice has them (installSharedTextureMethods)
@interface SwitchMetalSharedTextureAtIndex : NSObject
@end
@implementation SwitchMetalSharedTextureAtIndex

- (id<MTLTexture>)newSharedTextureWithHandle:(MTLSharedTextureHandle *)handle withResourceIndex:(NSUInteger)index
{
    return openShared((id<MTLDevice>)self, handle, &index);
}

@end

void installSharedTextureMethods(Class cls)
{
    if constexpr (kKind.resourceIndexOpen) {
        const SEL sel = @selector(newSharedTextureWithHandle:withResourceIndex:);
        Method m = class_getInstanceMethod([SwitchMetalSharedTextureAtIndex class], sel);
        class_addMethod(cls, sel, method_getImplementation(m), method_getTypeEncoding(m));
    } else {
        (void)cls;
    }
}

@interface SwitchMetalDevice (SharedTextures)
@end
@implementation SwitchMetalDevice (SharedTextures)

- (NSMutableDictionary *)copyIOSurfaceSharedTextureProperties:(MTLTextureDescriptor *)d
{
    gm20b::SurfaceImage image;
    if (sharedImage(d, image))
        return nil;
    return image.kind == gm20b::SurfaceImage::Plain
               ? gm20bSharedSurfaceProperties(d, gm20b::formatInfo(uint32_t(d.pixelFormat))->bpp)
               : gm20bLaidOutSurfaceProperties(image.layout.bytes);
}

- (id<MTLTexture>)newSharedTextureWithDescriptor:(MTLTextureDescriptor *)d
{
    NSMutableDictionary *p = [self copyIOSurfaceSharedTextureProperties:d];
    if (!p) {
        gm20b::SurfaceImage image;
        const char *why = sharedImage(d, image);
        SWITCH_REFUSE_DETAIL(
            "gm20b", "texture.shared", d.pixelFormat << 8 | d.textureType,
            "newSharedTextureWithDescriptor: %lux%lux%lu format %lu type %lu levels %lu samples %lu "
            "array %lu storage %lu: %s",
            (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.depth, (unsigned long)d.pixelFormat,
            (unsigned long)d.textureType, (unsigned long)d.mipmapLevelCount, (unsigned long)d.sampleCount,
            (unsigned long)d.arrayLength, (unsigned long)d.storageMode, why ? why : "an extent past the GPU's");
        return nil;
    }
    gm20bAddSharedTextureKeys(p, d, [(id<MTLDevice>)self registryID], kKind);
    IOSurfaceRef s = IOSurfaceCreate((__bridge CFDictionaryRef)p);
    if (!s) {
        SWITCH_REFUSE("gm20b", "texture.shared.surface", "IOSurfaceCreate refused a %lux%lu shared texture's surface",
                      (unsigned long)d.width, (unsigned long)d.height);
        return nil;
    }
    MTLSharedTextureHandle *h = handleOver(s, nil);
    CFRelease(s);
    return h ? [self newSharedTextureWithHandle:h] : nil;
}

- (id<MTLTexture>)newSharedTextureWithHandle:(MTLSharedTextureHandle *)handle
{
    return openShared((id<MTLDevice>)self, handle, nullptr);
}

@end

@interface SwitchMetalTexture (SharedTextures)
@end
@implementation SwitchMetalTexture (SharedTextures)

- (BOOL)isShareable
{
    SwitchMetalShareable known = _shareable.load(std::memory_order_relaxed);
    if (known == SwitchMetalShareable::Unasked) {
        NSDictionary *p = _kind == SwitchMetalTextureSurface && _surface ? creationProperties(_surface) : nil;
        known =
            p[gm20bSharedString(gm20b::shared::kRegistryKey)] ? SwitchMetalShareable::Yes : SwitchMetalShareable::No;
        _shareable.store(known, std::memory_order_relaxed);
    }
    return known == SwitchMetalShareable::Yes;
}

- (MTLSharedTextureHandle *)newSharedTextureHandle
{
    return [self isShareable] ? handleOver(_surface, self.label) : nil;
}

@end
