// SPDX-License-Identifier: MIT
// A kernel's texture whose texels the texture header cannot address where they lie (Texture::sampled: rows or a start
// 16 bytes off 32, the header's ADDRESS_BITS31TO5 and PITCH_BITS20TO5 in clb097tex.h; or a cropped view of a
// block-linear surface) is read and written through its own 32-aligned image. The copy engine fills that image from the
// texels when its compute encoder begins and copies it back when the encoder ends. Inside the encoder it does either
// sooner only where another resource of a dispatch reaches the same bytes: the image is copied back before a dispatch
// that reaches them otherwise, and filled again before a dispatch that uses it after another resource wrote them.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace raw {

struct ByteSpan {
    uint64_t va = 0, bytes = 0;

    bool overlaps(const ByteSpan &o) const
    {
        return bytes && o.bytes && (va <= o.va ? o.va - va < bytes : va - o.va < o.bytes);
    }
};

// The bytes a texture's texels lie in: its rows when it is one pitch image, else its storage
template <typename Texture> ByteSpan kernelTextureSpan(const Texture &t)
{
    if (!t.blockLinear && !t.chain && t.pitch && t.width && t.height && t.layers <= 1 && t.depth <= 1)
        return {t.va, uint64_t(t.pitch) * (t.height - 1) + uint64_t(t.width) * t.bpp};
    const auto range = t.storageRange();
    return {range.va, range.bytes};
}

// One resource of a dispatch: `shadowed`, a texture it reaches through the texture's image
struct KernelUse {
    const void *object = nullptr;
    ByteSpan span;
    bool shadowed = false, writes = false;
};

// A dispatch that stores to the same bytes through a texture's image and through another resource leaves the image's
// values there: the image is copied back after it, and a shadowed texture over those bytes is filled again before its
// next use. Metal promises nothing for that dispatch ("that data may not be coherent within a single render pass ... a
// texture you create from the method may not be able to immediately reflect changes to the underlying buffer that come
// from a render or kernel function": MTLBuffer makeTexture(descriptor:offset:bytesPerRow:)).
// The copies around one compute encoder's dispatches. Every write reaching texels in place (a dispatch's, or an image's
// copy back) takes the next tick of the encoder's clock; an image filled at tick t holds every write before t.
class KernelShadows {
public:
    struct Steps {
        std::vector<const void *> copyBack, fill;  // images into their texels, then texels into their images

        bool empty() const
        {
            return copyBack.empty() && fill.empty();
        }
    };

    // The encoder begins with every shadowed texture of its dispatches filled
    void begin(const KernelUse *uses, size_t count)
    {
        shadowed_.clear();
        written_.clear();
        writes_.clear();
        clock_ = 0;
        for (size_t i = 0; i < count; ++i)
            if (uses[i].shadowed && !find(uses[i].object))
                shadowed_.push_back({uses[i].object, uses[i].span, 0});
    }

    // What runs before a dispatch of these uses
    const Steps &before(const KernelUse *uses, size_t count)
    {
        steps_.copyBack.clear();
        steps_.fill.clear();
        for (auto w = written_.begin(); w != written_.end();) {
            const bool reached = std::any_of(uses, uses + count, [&](const KernelUse &u) {
                return u.object != w->object && u.span.overlaps(w->span);
            });
            if (!reached) {
                ++w;
                continue;
            }
            steps_.copyBack.push_back(w->object);
            writes_.push_back({w->object, w->span, clock_++});
            w = written_.erase(w);
        }
        for (size_t i = 0; i < count; ++i) {
            if (!uses[i].shadowed ||
                std::find(steps_.fill.begin(), steps_.fill.end(), uses[i].object) != steps_.fill.end())
                continue;
            Shadowed *s = find(uses[i].object);
            const bool stale = !s || std::any_of(writes_.begin(), writes_.end(), [&](const Write &w) {
                return w.object != s->object && w.tick >= s->filled && w.span.overlaps(s->span);
            });
            if (!stale)
                continue;
            if (!s) {
                shadowed_.push_back({uses[i].object, uses[i].span, clock_});
            } else {
                s->filled = clock_;
            }
            steps_.fill.push_back(uses[i].object);
        }
        return steps_;
    }

    // A dispatch of these uses ran
    void after(const KernelUse *uses, size_t count)
    {
        for (size_t i = 0; i < count; ++i) {
            if (!uses[i].writes)
                continue;
            if (!uses[i].shadowed)
                writes_.push_back({uses[i].object, uses[i].span, clock_});
            else if (std::none_of(written_.begin(), written_.end(), [&](const Write &w) {
                         return w.object == uses[i].object;
                     }))
                written_.push_back({uses[i].object, uses[i].span, clock_});
        }
        ++clock_;
    }

    // The images written since their last copy back, copied back as the encoder ends
    std::vector<const void *> end()
    {
        std::vector<const void *> back;
        for (const Write &w : written_)
            back.push_back(w.object);
        written_.clear();
        return back;
    }

private:
    struct Shadowed {
        const void *object;
        ByteSpan span;
        uint32_t filled;  // the tick its image was last filled at
    };

    struct Write {
        const void *object;
        ByteSpan span;
        uint32_t tick;
    };

    Shadowed *find(const void *object)
    {
        for (Shadowed &s : shadowed_)
            if (s.object == object)
                return &s;
        return nullptr;
    }

    std::vector<Shadowed> shadowed_;
    std::vector<Write> written_, writes_;
    Steps steps_;
    uint32_t clock_ = 0;
};

}  // namespace raw
