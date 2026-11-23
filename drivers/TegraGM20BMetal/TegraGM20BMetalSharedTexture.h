// SPDX-License-Identifier: MIT
// A shared texture's IOSurface creation properties from its descriptor, and the descriptor read back from them, by the
// kind's keys (shared_texture.h)
#pragma once
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <cctype>
#include <string>
#include "shared_texture.h"

inline NSString *gm20bSharedString(std::string_view s)
{
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
}

inline SEL gm20bSharedSetter(std::string_view property)
{
    std::string name = "set" + std::string(property) + ":";
    name[3] = char(toupper(name[3]));
    return sel_registerName(name.c_str());
}

inline uint32_t gm20bSwizzleKey(MTLTextureSwizzleChannels s)
{
    return uint32_t(s.red) | uint32_t(s.green) << 8 | uint32_t(s.blue) << 16 | uint32_t(s.alpha) << 24;
}

inline MTLTextureSwizzleChannels gm20bSwizzleChannels(uint32_t key)
{
    return MTLTextureSwizzleChannelsMake(MTLTextureSwizzle(key & 0xff), MTLTextureSwizzle(key >> 8 & 0xff),
                                         MTLTextureSwizzle(key >> 16 & 0xff), MTLTextureSwizzle(key >> 24));
}

inline NSNumber *gm20bSharedValue(MTLTextureDescriptor *d, const gm20b::shared::Key &k)
{
    NSString *property = gm20bSharedString(k.property);
    switch (k.how) {
    case gm20b::shared::Property::Swizzle:
        return @(gm20bSwizzleKey(d.swizzle));
    case gm20b::shared::Property::Public:
        return [d valueForKey:property];
    case gm20b::shared::Property::Private:
        return [d respondsToSelector:NSSelectorFromString(property)] ? [d valueForKey:property] : @0;
    }
    return @0;
}

// The surface of `d`'s texels (`bpp` bytes each) and nothing else: nil where it has none
inline NSMutableDictionary *gm20bSharedSurfaceProperties(MTLTextureDescriptor *d, uint32_t bpp)
{
    gm20b::shared::SurfaceShape shape;
    if (d.width > TGM_LAYOUT_MAX_EXTENT || d.height > TGM_LAYOUT_MAX_EXTENT ||
        !gm20b::shared::surfaceShape(uint32_t(d.width), uint32_t(d.height), bpp, shape))
        return nil;
    return [@{
        (__bridge NSString *)kIOSurfaceAllocSize : @(shape.allocBytes),
        (__bridge NSString *)kIOSurfaceWidth : @(d.width),
        (__bridge NSString *)kIOSurfaceHeight : @(d.height),
        (__bridge NSString *)kIOSurfaceBytesPerElement : @(bpp),
        (__bridge NSString *)kIOSurfaceBytesPerRow : @(shape.bytesPerRow),
    } mutableCopy];
}

// The surface of a texture laid out in it (surface_image.h), `bytes` its image's: AGX's two properties
inline NSMutableDictionary *gm20bLaidOutSurfaceProperties(uint64_t bytes)
{
    return [@{
        (__bridge NSString *)kIOSurfaceAllocSize : @(bytes),
        gm20bSharedString(gm20b::shared::kLayoutKey) : @0,
    } mutableCopy];
}

inline void gm20bAddSharedTextureKeys(NSMutableDictionary *p, MTLTextureDescriptor *d, uint64_t registryID,
                                      const gm20b::shared::Kind &kind)
{
    p[gm20bSharedString(gm20b::shared::kRegistryKey)] = @(registryID);
    for (const gm20b::shared::Key &k : gm20b::shared::kKeys)
        if (gm20b::shared::keyOf(kind, k))
            p[gm20bSharedString(k.key)] = gm20bSharedValue(d, k);
    p[gm20bSharedString(gm20b::shared::kProtection.key)] = gm20bSharedValue(d, gm20b::shared::kProtection);
}

// nil unless `p` holds every key of the kind and names the device `registryID`
inline MTLTextureDescriptor *gm20bSharedTextureDescriptor(NSDictionary *p, uint64_t registryID,
                                                          const gm20b::shared::Kind &kind)
{
    NSNumber *owner = p[gm20bSharedString(gm20b::shared::kRegistryKey)];
    if (![owner isKindOfClass:[NSNumber class]] || owner.unsignedLongLongValue != registryID)
        return nil;
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    for (const gm20b::shared::Key &k : gm20b::shared::kKeys) {
        if (!gm20b::shared::keyOf(kind, k))
            continue;
        NSNumber *value = p[gm20bSharedString(k.key)];
        if (![value isKindOfClass:[NSNumber class]])
            return nil;
        switch (k.how) {
        case gm20b::shared::Property::Swizzle:
            d.swizzle = gm20bSwizzleChannels(value.unsignedIntValue);
            break;
        case gm20b::shared::Property::Public:
            [d setValue:value forKey:gm20bSharedString(k.property)];
            break;
        case gm20b::shared::Property::Private:
            if ([d respondsToSelector:gm20bSharedSetter(k.property)])
                [d setValue:value forKey:gm20bSharedString(k.property)];
            break;
        }
    }
    return d;
}
