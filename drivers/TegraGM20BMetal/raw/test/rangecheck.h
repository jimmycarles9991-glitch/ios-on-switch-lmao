// SPDX-License-Identifier: MIT
#pragma once
#include "wrapcheck.h"

namespace raw {
template <class Expect> void DeviceHarness::ranges(Device &device, Expect expect)
{
    Texture pitch, view, mip, part, level, array, layer, bufferView;
    Buffer buffer;
    if (!device.createTexture(13, 9, 80, pitch) || !device.createView(pitch, 81, view) ||
        !device.createMipTexture(65, 33, 80, 4, mip) ||
        !device.createPartView(mip, 80, fx::kTexTwoD, 2, 1, 0, 1, part) || !chainLevelImage(mip, 2, level) ||
        !device.createArray(17, 9, 3, 80, fx::kTexTwoDArray, array) ||
        !device.createPartView(array, 80, fx::kTexTwoD, 0, 1, 1, 1, layer) ||
        !device.createBuffer(1024, buffer, false) || !device.createBufferTexture(buffer, 128, 8, 4, 32, 80, bufferView))
        std::abort();
    const auto same = [](const Texture::StorageRange &a, const Texture::StorageRange &b) {
        return a.va == b.va && a.bytes == b.bytes && a.hostVisible == b.hostVisible;
    };
    expect(pitch.storageRange().va == pitch.span.va && pitch.storageRange().bytes == pitch.span.bytes &&
               pitch.storageRange().bytes >= uint64_t(pitch.pitch) * pitch.height,
           "pitch extent includes allocated row padding");
    expect(!view.span.valid() && !view.import && same(view.storageRange(), pitch.storageRange()),
           "format view keeps storage extent after dropping ownership fields");
    expect(same(mip.storageRange(), part.storageRange()) && same(mip.storageRange(), level.storageRange()) &&
               mip.storageRange().bytes >= mip.chain->layout.bytes,
           "mip views retain the complete padded chain allocation");
    expect(same(array.storageRange(), layer.storageRange()) && layer.va != array.va &&
               array.storageRange().bytes >= array.layerBytes * array.layers,
           "layer view retains allocation including every padded layer");
    expect(bufferView.storageRange().va == buffer.va + 128 && bufferView.storageRange().bytes == 128 &&
               bufferView.storageRange().hostVisible,
           "buffer texture records only its bounded rows within the parent buffer");
    Texture bad;
    expect(!device.createBufferTexture(buffer, UINT64_MAX - 31, 8, 4, 32, 80, bad),
           "buffer texture offset overflow cannot create a qualified range");

    auto &flags = const_cast<TegraGM20BAbiInfo &>(device.k_.info()).flags;
    const auto saved = flags;
    flags |= TegraGM20BInfoSubmitAddress;
    auto *cb = device.begin();
    if (!cb)
        std::abort();
    const auto exact = [&](uint64_t va, uint32_t bytes, bool write) {
        const auto &uses = cb->surfaces_.uses();
        return uses.size() == 1 && uses[0].handle == va && uses[0].reserved == bytes &&
               (uses[0].access & (TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceRange)) ==
                   (TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceRange) &&
               bool(uses[0].access & TegraGM20BSurfaceWrite) == write;
    };
    Import imported;
    imported.bo = {17, buffer.va, buffer.length};
    Buffer alias = buffer;
    alias.import = &imported;
    cb->use(alias, true);
    expect(exact(buffer.va, uint32_t(buffer.length), true),
           "buffer write declares exact bytes without a duplicate whole-import write");
    cb->surfaces_.clear();
    Texture importedTexture = bufferView;
    importedTexture.import = &imported;
    cb->use(importedTexture, true);
    expect(exact(buffer.va + 128, 128, true), "ordinary imported texture write keeps its exact storage range");
    cb->surfaces_.clear();
    Texture converted = pitch;
    converted.planar = true;
    imported.bo.va = 0x10000;
    converted.import = &imported;
    cb->use(converted, true);
    expect(cb->surfaces_.uses().size() == 2 && cb->surfaces_.uses()[0].handle == imported.bo.handle &&
               cb->surfaces_.uses()[1].reserved == pitch.storageRange().bytes,
           "converted image preserves distinct plane import and image ownership");
    cb->surfaces_.clear();
    Texture unknown;
    unknown.va = buffer.va;
    cb->use(unknown, true);
    expect(cb->surfaces_.uses().size() == 1 && cb->surfaces_.uses()[0].reserved == 0 &&
               !(cb->surfaces_.uses()[0].access & TegraGM20BSurfaceRange),
           "unknown texture extent never becomes a one-byte exact write");
    cb->surfaces_.clear();
    auto *blit = cb->blit();
    if (!blit)
        std::abort();
    blit->fillBytes(buffer.va + 64, 73, 0);
    expect(exact(buffer.va + 64, 73, true), "CE fill carries its exact destination extent");
    cb->surfaces_.clear();
    blit->copyRect(buffer.va, 64, buffer.va + 512, 96, 7, 3, 4);
    const auto &rows = cb->surfaces_.uses();
    expect(rows.size() == 2 && rows[0].reserved == 156 && rows[1].reserved == 220,
           "CE rectangle extent includes row gaps and the final visible row");
    cb->surfaces_.clear();
    blit->copyBufferTexels(buffer.va, 64, pitch, 0, 0, 0, 7, 3, false, kAspectWhole);
    expect(cb->surfaces_.uses()[0].handle == buffer.va && cb->surfaces_.uses()[0].reserved == 156,
           "texture readback carries the buffer-side row extent");
    blit->end();
    cb->surfaces_.clear();
    cb->useAddress(UINT64_MAX - 7, 16, true);
    expect(!cb->surfaces_.valid(), "overflowed GPU address extent cannot be submitted");
    cb->abandon();
    flags = saved;
    for (Texture *texture : {&bufferView, &layer, &array, &part, &mip, &view, &pitch})
        device.destroyTexture(*texture);
    device.destroyBuffer(buffer);
}
}  // namespace raw
