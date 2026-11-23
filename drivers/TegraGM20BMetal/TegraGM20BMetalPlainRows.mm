// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalPlainRows.h"
#include "plain_rows.h"
#include "surface_refusal.h"
#include "xr.h"
#include "ycbcr.h"
#include "ycbcr_store.h"
#include <mach/vm_page_size.h>

namespace {

// The rows an M4 Max's GPU reads a plane at: its bytesPerRow rounded down to 16, rows under 16 bytes 1 MiB apart (the
// bytesPerRow in 16-byte units less one, kept to 16 bits)
size_t gpuReadRows(size_t pitch)
{
    constexpr size_t kMebibyte = size_t(1) << 20;
    return pitch < 16 ? kMebibyte : pitch & ~size_t(15);
}

uint64_t planeOffset(IOSurfaceRef s, NSUInteger plane)
{
    return uintptr_t(IOSurfaceGetBaseAddressOfPlane(s, plane)) - uintptr_t(IOSurfaceGetBaseAddress(s));
}

}  // namespace

bool plainRowsShape(MTLTextureDescriptor *d, IOSurfaceRef s, NSUInteger plane)
{
    const uint32_t format = uint32_t(d.pixelFormat);
    const gm20b::FormatInfo *f = gm20b::formatInfo(format);
    const size_t planes = IOSurfaceGetPlaneCount(s);
    if (!f || gm20b::blockCompressed(f) || gm20b::twoPlaneXr(format) || gm20b::isYcbcrFormat(format) ||
        gm20b::packedPlaneChannels(format) || d.textureType != MTLTextureType2D || !gm20bShape(d) || plane >= planes)
        return false;
    const size_t pitch = IOSurfaceGetBytesPerRowOfPlane(s, plane), read = gpuReadRows(pitch);
    const uint64_t row = uint64_t(d.width) * f->bpp;
    const uint64_t end = (planeOffset(s, plane) & ~15ull) + uint64_t(read) * (d.height - 1) + row;
    return (d.height > 1 && (read < row || pitch < 16)) || end > IOSurfaceGetAllocSize(s);
}

bool makePlainRows(SwitchMetalTexture *t, IOSurfaceRef s, NSUInteger plane, uint32_t width, uint32_t height)
{
    gm20b::Device &device = *gRM->raw;
    const uint32_t format = uint32_t(t->_descriptor.pixelFormat);
    const gm20b::FormatInfo *f = gm20b::formatInfo(format);
    uint8_t *base = static_cast<uint8_t *>(IOSurfaceGetBaseAddress(s));
    const uint64_t offset = planeOffset(s, plane);
    const size_t pitch = IOSurfaceGetBytesPerRowOfPlane(s, plane), read = gpuReadRows(pitch);
    const uint64_t row = uint64_t(width) * f->bpp, end = (offset & ~15ull) + uint64_t(read) * (height - 1) + row;
    const bool storedSixteen = height > 1 && pitch < 16;
    const bool pooled = (height > 1 && read < row) || storedSixteen;
    gm20b::Texture &x = t->_raw;
    if (!device.createSurfaceTexture(IOSurfaceGetID(s), base, IOSurfaceGetAllocSize(s), s, offset & ~15ull, width,
                                     height, uint32_t(read), format, x, false,
                                     std::max<uint64_t>(end, IOSurfaceGetAllocSize(s)), pooled))
        return false;
    const uint64_t mapped = (IOSurfaceGetAllocSize(s) + vm_page_size - 1) & ~uint64_t(vm_page_size - 1);
    const auto undo = [&](const char *why) {
        gm20b::surfaceRefusal = why;
        device.destroyTexture(x);
        return false;
    };
    // the kernel lays a surface out block-linear for the first texture that renders it: no rows of the plane's are left
    if (x.blockLinear)
        return undo("a surface the kernel laid out block-linear, whose rows no longer lie as the plane's do");
    // past the last page an M4 Max reads 0 and drops stores, as a sparse range does
    if (end > mapped && !device.openSparseWindow(x, end))
        return undo("no sparse VA window for rows past the surface's last page");
    x.storePitch = storedSixteen ? 16 : 0;
    t->_cpuRows = base + offset;
    t->_cpuPitch = pitch;
    if (pooled) {
        x.storeView = new gm20b::Texture;
        if (!device.createView(*x.sampled, gm20b::plainRowsFormat(f->bpp), *x.storeView)) {
            delete x.storeView;
            x.storeView = nullptr;
            return undo("no texture header for the view its rows are stored from");
        }
    }
    if (!keepExtendedRange(t->_descriptor, x, false))
        return undo("no memory or texture header for the stand-in it is sampled through");
    return true;
}
