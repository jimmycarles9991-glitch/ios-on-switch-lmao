// SPDX-License-Identifier: MIT
// A shared texture's IOSurface creation properties from its descriptor, and the descriptor read back from them, by the
// kind's keys (raw_shared_texture.h)
#pragma once
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <cctype>
#include <string>
#include "raw_shared_texture.h"

inline NSString *rmSharedString(std::string_view s)
{
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
}

inline SEL rmSharedSetter(std::string_view property)
{
    std::string name = "set" + std::string(property) + ":";
    name[3] = char(toupper(name[3]));
    return sel_registerName(name.c_str());
}

inline uint32_t rmSwizzleKey(MTLTextureSwizzleChannels s)
{
    return uint32_t(s.red) | uint32_t(s.green) << 8 | uint32_t(s.blue) << 16 | uint32_t(s.alpha) << 24;
}

inline MTLTextureSwizzleChannels rmSwizzleChannels(uint32_t key)
{
    return MTLTextureSwizzleChannelsMake(MTLTextureSwizzle(key & 0xff), MTLTextureSwizzle(key >> 8 & 0xff),
                                         MTLTextureSwizzle(key >> 16 & 0xff), MTLTextureSwizzle(key >> 24));
}

inline NSNumber *rmSharedValue(MTLTextureDescriptor *d, const raw::shared::Key &k)
{
    NSString *property = rmSharedString(k.property);
    switch (k.how) {
    case raw::shared::Property::Swizzle:
        return @(rmSwizzleKey(d.swizzle));
    case raw::shared::Property::Public:
        return [d valueForKey:property];
    case raw::shared::Property::Private:
        return [d respondsToSelector:NSSelectorFromString(property)] ? [d valueForKey:property] : @0;
    }
    return @0;
}

// The surface of `d`'s texels (`bpp` bytes each) and nothing else: nil where it has none
inline NSMutableDictionary *rmSharedSurfaceProperties(MTLTextureDescriptor *d, uint32_t bpp)
{
    raw::shared::SurfaceShape shape;
    if (d.width > TGM_LAYOUT_MAX_EXTENT || d.height > TGM_LAYOUT_MAX_EXTENT ||
        !raw::shared::surfaceShape(uint32_t(d.width), uint32_t(d.height), bpp, shape))
        return nil;
    return [@{
        (__bridge NSString *)kIOSurfaceAllocSize : @(shape.allocBytes),
        (__bridge NSString *)kIOSurfaceWidth : @(d.width),
        (__bridge NSString *)kIOSurfaceHeight : @(d.height),
        (__bridge NSString *)kIOSurfaceBytesPerElement : @(bpp),
        (__bridge NSString *)kIOSurfaceBytesPerRow : @(shape.bytesPerRow),
    } mutableCopy];
}

// The surface of a texture laid out in it (raw_surface_image.h), `bytes` its image's: AGX's two properties
inline NSMutableDictionary *rmLaidOutSurfaceProperties(uint64_t bytes)
{
    return [@{
        (__bridge NSString *)kIOSurfaceAllocSize : @(bytes),
        rmSharedString(raw::shared::kLayoutKey) : @0,
    } mutableCopy];
}

inline void rmAddSharedTextureKeys(NSMutableDictionary *p, MTLTextureDescriptor *d, uint64_t registryID,
                                   const raw::shared::Kind &kind)
{
    p[rmSharedString(raw::shared::kRegistryKey)] = @(registryID);
    for (const raw::shared::Key &k : raw::shared::kKeys)
        if (raw::shared::keyOf(kind, k))
            p[rmSharedString(k.key)] = rmSharedValue(d, k);
    p[rmSharedString(raw::shared::kProtection.key)] = rmSharedValue(d, raw::shared::kProtection);
}

// nil unless `p` holds every key of the kind and names the device `registryID`
inline MTLTextureDescriptor *rmSharedTextureDescriptor(NSDictionary *p, uint64_t registryID,
                                                       const raw::shared::Kind &kind)
{
    NSNumber *owner = p[rmSharedString(raw::shared::kRegistryKey)];
    if (![owner isKindOfClass:[NSNumber class]] || owner.unsignedLongLongValue != registryID)
        return nil;
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    for (const raw::shared::Key &k : raw::shared::kKeys) {
        if (!raw::shared::keyOf(kind, k))
            continue;
        NSNumber *value = p[rmSharedString(k.key)];
        if (![value isKindOfClass:[NSNumber class]])
            return nil;
        switch (k.how) {
        case raw::shared::Property::Swizzle:
            d.swizzle = rmSwizzleChannels(value.unsignedIntValue);
            break;
        case raw::shared::Property::Public:
            [d setValue:value forKey:rmSharedString(k.property)];
            break;
        case raw::shared::Property::Private:
            if ([d respondsToSelector:rmSharedSetter(k.property)])
                [d setValue:value forKey:rmSharedString(k.property)];
            break;
        }
    }
    return d;
}
