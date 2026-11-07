// SPDX-License-Identifier: MIT
// See RawAlias.h.
#import "RawAlias.h"
#include "NativeBridgeMetalFrontend.h"

// Weak: the host benchmark links the raw path without the frontend.
extern "C" id native_bridge_metal_buffer_over_foreign(const struct switch_foreign_memory *memory, void *bytes,
                                                      NSUInteger length, NSUInteger options)
    __attribute__((weak_import));
extern "C" void native_bridge_metal_set_surface_importer(switch_surface_importer importer) __attribute__((weak_import));

namespace {
raw::Device *gDevice;

switch_foreign_memory foreignOf(const raw::Alias &a)
{
    return {a.cpu, a.bytes, a.handle, a.va, a.kind, raw::Device::holdLease, raw::Device::dropLease, a.lease};
}

bool importSurface(uint32_t surfaceId, const void *surface, void *base, uint64_t bytes, switch_foreign_memory *memory)
{
    raw::Alias a;
    if (!gDevice->aliasSurface(surfaceId, base, bytes, surface, a))
        return false;
    *memory = foreignOf(a);
    return true;
}
}  // namespace

void rawAliasSurfaces(raw::Device *dev)
{
    gDevice = dev;
    if (native_bridge_metal_set_surface_importer)
        native_bridge_metal_set_surface_importer(importSurface);
}

id<MTLBuffer> rawAliasBuffer(raw::Alias &a, void *bytes, NSUInteger length, MTLResourceOptions options)
{
    const switch_foreign_memory memory = foreignOf(a);
    id<MTLBuffer> made = native_bridge_metal_buffer_over_foreign
                             ? native_bridge_metal_buffer_over_foreign(&memory, bytes, length, options)
                             : nil;
    raw::Device::dropLease(a.lease);
    return made;
}
