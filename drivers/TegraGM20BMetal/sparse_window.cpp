// SPDX-License-Identifier: MIT
// A YCbCr texture whose plane rows run past its surface's allocation (ycbcr.h YcbcrPlan::window), which Metal reads as 0
// there: its planes are read through a GPU VA window of their own, the surface import's pages bound again at its start (the
// same BO, no second BoImport) and the rest sparse (the GMMU answers a read with 0 and drops a write, with no fault), in one
// VmBind (Kernel::bindWindow). A job names only the bound pages (Texture::storage, Window::pages): the kernel refuses a
// use no one binding holds.
#include "lock.h"
#include "gpu.h"

#include <algorithm>

#include "gm20b_gpu_internal.h"

namespace gm20b {

bool Device::openSparseWindow(Texture &t, uint64_t end, uint64_t sink)
{
    // a YCbCr texture's planes, or a texture of its own rows (plain_rows.h)
    Import *import = t.onePlane || !t.luma ? t.import : t.luma->import;
    if (!import || import->kernelTiled() || !k_.sparse())
        return false;
    const uint64_t page = k_.info().page_size, pages = import->bo.bytes, into = import->bo.va - import->bo.vaBase;
    // a sink starts past both the bytes read and the import's pages, so that it is sparse from its first byte
    const uint64_t reach = (into + end + page - 1) & ~(page - 1), sinkSpan = (sink + page - 1) & ~(page - 1);
    const uint64_t bytes = sink ? std::max(reach, (pages + page - 1) & ~(page - 1)) + sinkSpan : reach;
    if (bytes <= pages)
        return true;
    const uint64_t va = k_.reserveVa(bytes);
    if (!va)
        return false;
    if (!k_.bindWindow(import->bo, va, import->kind, bytes)) {
        k_.unbind(va, bytes);
        k_.releaseVa(va, bytes);
        return false;
    }
    t.window = {va, bytes, pages};
    // a place past the surface's base, in the window (each plane's import binds the same pages, a surface of ID 0 anew)
    const auto moved = [&](uint64_t at, const Import &of) {
        return va + into + (at - of.bo.va);
    };
    if (t.onePlane) {
        t.planes.luma = moved(t.planes.luma, *import);
        return true;
    }
    std::lock_guard<gm20b::Mutex> l(lock_);
    for (Texture *plane : {t.luma ? t.luma : &t, t.chroma}) {
        if (!plane || plane->memoryless)
            continue;
        plane->va = moved(plane->va, *plane->import);
        plane->storage.va = plane->va;
        plane->storage.bytes = std::min(plane->storage.bytes, va + pages - plane->va);
        if (!plane->sampled)
            writeTic(*plane);
    }
    return true;
}

bool Device::openSparseSink(Texture &t, uint64_t bytes)
{
    const uint64_t page = k_.info().page_size, size = (bytes + page - 1) & ~(page - 1);
    const uint64_t va = k_.sparse() && size ? k_.reserveVa(size) : 0;
    if (!va)
        return false;
    if (!k_.bindSparse(va, size)) {
        k_.releaseVa(va, size);
        return false;
    }
    t.window = {va, size, 0};
    return true;
}

void Device::closeSparseWindow(Texture &t)
{
    if (!t.window.bytes)
        return;
    k_.unbind(t.window.va, t.window.bytes);
    k_.releaseVa(t.window.va, t.window.bytes);
    t.window = {};
}

}  // namespace gm20b
