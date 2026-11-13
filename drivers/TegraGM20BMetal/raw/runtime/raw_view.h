// SPDX-License-Identifier: MIT
// A kernel's texture slot where m2v binds a view of the texture in another format (PackKernelEntry::window: word 0
// of NVK's header of that view). m2v writes a half-float texture through its unsigned integer twin (R16_UINT,
// R16G16_UINT, R16G16B16A16_UINT), converting each texel in the shader. The raw path binds a header of its own over
// the same memory with that word 0 (Device::viewTic): on Maxwell NVK's sampled and storage descriptors alike are the
// header's index alone.
#pragma once
#include <cstdint>

namespace raw {

enum class SlotView : uint8_t {
    Own,   // the texture's own header is the one m2v binds
    View,  // a header of the texture's memory with m2v's word 0
    None,  // m2v's view reads texels of another size, or the texture's are blocks
};

// NIL's view access (nil_view.access): a storage view of a multisampled image spans its sample grid (viewHeader)
enum class ViewAccess : uint8_t {
    Texture,
    Storage,
};

// Bytes of a texel under header word 0's COMPONENTS (clb097tex.h); 0 for a block-compressed, depth or video layout
inline uint32_t texelBytes(uint32_t word0)
{
    switch (word0 & 0x7f) {
    case 0x01:  // R32_G32_B32_A32
        return 16;
    case 0x02:  // R32_G32_B32
        return 12;
    case 0x03:  // R16_G16_B16_A16
    case 0x04:  // R32_G32
    case 0x05:  // R32_B24G8
        return 8;
    case 0x07:  // X8B8G8R8
    case 0x08:  // A8B8G8R8
    case 0x09:  // A2B10G10R10
    case 0x0c:  // R16_G16
    case 0x0d:  // G8R24
    case 0x0e:  // G24R8
    case 0x0f:  // R32
    case 0x20:  // E5B9G9R9_SHAREDEXP
    case 0x21:  // BF10GF11RF11
        return 4;
    case 0x12:  // A4B4G4R4
    case 0x13:  // A5B5G5R1
    case 0x14:  // A1B5G5R5
    case 0x15:  // B5G6R5
    case 0x16:  // B6G5R5
    case 0x18:  // G8R8
    case 0x1b:  // R16
        return 2;
    case 0x1d:  // R8
    case 0x1e:  // G4R4
        return 1;
    default:
        return 0;
    }
}

// The header m2v binds at a slot of word 0 `window` (0: the record names none), for a texture of `bytes` a texel
// or block whose format's word 0 is `tic0` and whose own header's is `own` (tic0 under the texture's swizzle)
inline SlotView slotView(uint32_t window, uint32_t tic0, uint32_t own, uint32_t bytes, bool blocks)
{
    if (!window || window == tic0 || window == own)
        return SlotView::Own;
    return !blocks && texelBytes(window) == bytes ? SlotView::View : SlotView::None;
}

// The header of the texture headed by `own` viewed as word 0 `word0`. No sRGB conversion: m2v reads and writes an
// sRGB texture in a kernel through its UNORM twin, encoding in the shader, and NVK's view of a format of other data
// types converts none.
//
// A storage view of a multisampled texture whose samples run 1 << sxLog2 across and 1 << syLog2 down a pixel spans the
// sample grid, as NIL's storage descriptor does (descriptor.rs normalize_extent). The surface unit ignores
// MULTI_SAMPLE_COUNT and bounds each load and store by WIDTH and HEIGHT alone, while NAK addresses each sample at its
// place in the grid (nak_nir_lower_tex.c lower_msaa_image_access, which reads MULTI_SAMPLE_COUNT through TXQ). At the
// pixel extent every surface op outside the grid's top-left width x height is dropped.
inline void viewHeader(const uint32_t own[8], uint32_t word0, uint32_t out[8], uint8_t sxLog2 = 0, uint8_t syLog2 = 0)
{
    for (int i = 0; i < 8; i++)
        out[i] = own[i];
    out[0] = word0;
    out[4] &= ~(1u << 22);                                                     // S_R_G_B_CONVERSION (bit 150)
    out[4] = (out[4] & ~0xffffu) | ((((out[4] & 0xffff) + 1) << sxLog2) - 1);  // WIDTH_MINUS_ONE (143:128)
    out[5] = (out[5] & ~0xffffu) | ((((out[5] & 0xffff) + 1) << syLog2) - 1);  // HEIGHT_MINUS_ONE (175:160)
}

}  // namespace raw
