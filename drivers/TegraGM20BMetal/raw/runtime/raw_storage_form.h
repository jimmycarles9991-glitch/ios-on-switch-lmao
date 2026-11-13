// SPDX-License-Identifier: MIT
// A state's storage (interlocked) form binds the colors it fetches as storage images, which a pass's attachment can be
// viewed as only where its texel size and samples are the form's and its memory is not compressed, until rule
// storage.compressed holds: whether the GM20B's surface loads and stores (SULD/SUST) go through the L2's compression tags
// like every other client, or read a compressible kind's raw bytes, is unmeasured (nouveau's nvc0 and NVK never give a
// shader image a compressed kind). gpu.raw.storage_compressed decides it: a fetching draw's storage form over a ZBC-cleared
// C32_2CRA target (switchRawProbeStorageCompressed:) against the same draws on plain memory. Where an attachment does not
// fit, the draw takes the state's barrier form (its twin in the pack, resolved with the state), which orders its fetches
// with barriers instead. Metal's lossless-compression flag (Texture::compressed) is only how m2v clears and filters such a
// texture: the memory is compressed only where Texture::comptags says so.
#pragma once
#include <cstdint>

namespace raw {

constexpr bool storageReadsCompressed()
{
    return false;
}

enum class StorageFallback : uint8_t {
    None,
    Metadata,  // the pack's storage record cannot be used (raw_storage.h usableStorageMetadata): its barrier record is
    TexelBytes,  // an attachment's texel size is not the storage view's
    Samples,     // an attachment's samples are not the form's
    Compressed,  // an attachment's memory carries compression tags
    // it fetches only colors it never writes (Core Animation's memoryless image sources): no draw of it reads what another
    // of its draws wrote, so the pixel interlock orders nothing the barrier form's barriers do not
    ReadOnly,
    NoTwin,  // one of the above with no barrier form to take: the encoder is refused
    Count
};

constexpr size_t kStorageFallbacks = size_t(StorageFallback::Count);
constexpr const char *kStorageFallbackNames[kStorageFallbacks] = {"-",          "metadata", "texel", "samples",
                                                                  "compressed", "readonly", "notwin"};

// A storage form whose fetched colors (fetch) are none it writes, through the ROP (write) or as storage (storageWrite), and
// whose barrier form also writes none of them
constexpr bool fetchesOnlyUnwritten(uint8_t fetch, uint8_t write, uint8_t storageWrite, uint8_t twinFetch,
                                    uint8_t twinWrite)
{
    return fetch && !(fetch & (write | storageWrite)) && twinFetch == fetch && !(twinFetch & twinWrite);
}

// Why attachment `t` cannot be viewed as the storage image of a form whose view has `viewTexelBytes` and `samples`;
// `compressedOk`: compressed memory can be (rule storage.compressed, or its probe's switch)
template <typename Texture>
constexpr StorageFallback storageViewMismatch(uint32_t viewTexelBytes, uint32_t samples, const Texture &t,
                                              bool compressedOk = storageReadsCompressed())
{
    if (viewTexelBytes != t.bpp)
        return StorageFallback::TexelBytes;
    if (samples != t.samples)
        return StorageFallback::Samples;
    if (t.comptags && !compressedOk)
        return StorageFallback::Compressed;
    return StorageFallback::None;
}

}  // namespace raw
