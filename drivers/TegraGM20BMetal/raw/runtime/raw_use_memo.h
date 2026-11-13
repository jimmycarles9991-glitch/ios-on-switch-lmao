// SPDX-License-Identifier: MIT
// The buffers and textures a command buffer named to its job already (raw::CommandBuffer::use): every draw's binding
// table names them again, and a second name of the same object, address, extent, import and access changes nothing in
// the job's list (a texture or level target remade in place at an address it held before is another name). A read
// and a write are apart: a texture's read names the view readTexture gives, its write the texture.
// A slot another name took makes the next of the first a full one again, never a missed one.
#pragma once
#include <array>
#include <cstdint>

namespace raw {

class UseMemo {
public:
    void clear()
    {
        if (++epoch_ == 0) {
            entries_ = {};
            epoch_ = 1;
        }
    }

    // True: `object` at `va` of `bytes` in `import` was named with `write` since clear(); else it is noted now
    bool named(const void *object, uint64_t va, uint64_t bytes, const void *import, bool write)
    {
        const uintptr_t key = uintptr_t(object) ^ uintptr_t(write) << 3;
        Entry &e = entries_[(key >> 3 ^ key >> 9) & (kEntries - 1)];
        if (e.epoch == epoch_ && e.object == object && e.va == va && e.bytes == bytes && e.import == import &&
            e.write == write)
            return true;
        e = {object, import, va, bytes, epoch_, write};
        return false;
    }

private:
    static constexpr size_t kEntries = 64;

    struct Entry {
        const void *object = nullptr, *import = nullptr;
        uint64_t va = 0, bytes = 0;
        uint32_t epoch = 0;
        bool write = false;
    };

    std::array<Entry, kEntries> entries_{};
    uint32_t epoch_ = 1;
};

}  // namespace raw
