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

namespace gm20b {

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

// One resource of a dispatch: `shadowed`, a texture it reaches through the texture's image; `derived`, one it reads through a
// stand-in drawn from its texels (an extended-range texture, xr.h), which every write reaching those texels leaves
// stale, its own included
struct KernelUse {
    const void *object = nullptr;
    ByteSpan span;
    bool shadowed = false, writes = false, derived = false;
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
        std::vector<const void *> redraw;          // then stand-ins drawn again from their texels or images

        bool empty() const
        {
            return copyBack.empty() && fill.empty() && redraw.empty();
        }
    };

    // The encoder begins with every shadowed texture of its dispatches filled. A texture over several spans (a two-plane
    // texture's planes in pools of their own) is one use a span: each span is tracked, and its images move together.
    void begin(const KernelUse *uses, size_t count)
    {
        shadowed_.clear();
        derived_.clear();
        written_.clear();
        writes_.clear();
        clock_ = 0;
        for (size_t i = 0; i < count; ++i) {
            if (uses[i].shadowed && !find(uses[i]))
                shadowed_.push_back({uses[i].object, uses[i].span, 0});
            if (uses[i].derived && !findDerived(uses[i]))
                derived_.push_back({uses[i].object, uses[i].span, 0, false});
        }
    }

    // What runs before a dispatch of these uses
    const Steps &before(const KernelUse *uses, size_t count)
    {
        steps_.copyBack.clear();
        steps_.fill.clear();
        steps_.redraw.clear();
        for (const Write &w : written_)
            if (std::any_of(uses, uses + count, [&](const KernelUse &u) {
                    return u.object != w.object && u.span.overlaps(w.span);
                }))
                addOnce(steps_.copyBack, w.object);
        for (auto w = written_.begin(); w != written_.end();) {
            if (!listed(steps_.copyBack, w->object)) {
                ++w;
                continue;
            }
            writes_.push_back({w->object, w->span, clock_++});
            w = written_.erase(w);
        }
        for (size_t i = 0; i < count; ++i) {
            if (!uses[i].shadowed)
                continue;
            Shadowed *s = find(uses[i]);
            if (!s) {
                shadowed_.push_back({uses[i].object, uses[i].span, clock_});
                s = &shadowed_.back();
                addOnce(steps_.fill, uses[i].object);
            }
            if (listed(steps_.fill, uses[i].object))
                continue;
            const bool stale = std::any_of(writes_.begin(), writes_.end(), [&](const Write &w) {
                return w.object != s->object && w.tick >= s->filled && w.span.overlaps(s->span);
            });
            if (stale)
                addOnce(steps_.fill, uses[i].object);
        }
        for (Shadowed &s : shadowed_)
            if (listed(steps_.fill, s.object))
                s.filled = clock_;
        for (size_t i = 0; i < count; ++i)
            if (uses[i].derived && !uses[i].writes && staleDerived(uses[i]))
                steps_.redraw.push_back(uses[i].object);
        return steps_;
    }

    // A dispatch of these uses ran
    void after(const KernelUse *uses, size_t count)
    {
        for (size_t i = 0; i < count; ++i) {
            if (!uses[i].writes)
                continue;
            for (Derived &d : derived_)
                if (d.object == uses[i].object)
                    d.dirty = true;
            if (!uses[i].shadowed)
                writes_.push_back({uses[i].object, uses[i].span, clock_});
            else if (std::none_of(written_.begin(), written_.end(), [&](const Write &w) {
                         return w.object == uses[i].object && same(w.span, uses[i].span);
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
            addOnce(back, w.object);
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

    struct Derived {
        const void *object;
        ByteSpan span;
        uint32_t drawn;  // the tick its stand-in was last drawn at
        bool dirty;      // its own texels or image written since
    };

    static bool same(const ByteSpan &a, const ByteSpan &b)
    {
        return a.va == b.va && a.bytes == b.bytes;
    }

    static bool listed(const std::vector<const void *> &list, const void *object)
    {
        return std::find(list.begin(), list.end(), object) != list.end();
    }

    static void addOnce(std::vector<const void *> &list, const void *object)
    {
        if (!listed(list, object))
            list.push_back(object);
    }

    Shadowed *find(const KernelUse &u)
    {
        for (Shadowed &s : shadowed_)
            if (s.object == u.object && same(s.span, u.span))
                return &s;
        return nullptr;
    }

    Derived *findDerived(const KernelUse &u)
    {
        for (Derived &d : derived_)
            if (d.object == u.object && same(d.span, u.span))
                return &d;
        return nullptr;
    }

    // Whether the stand-in `u` reads holds every write before this dispatch; if not it is drawn again now, for all its
    // spans at once
    bool staleDerived(const KernelUse &u)
    {
        if (listed(steps_.redraw, u.object))
            return false;
        Derived *d = findDerived(u);
        if (!d) {
            derived_.push_back({u.object, u.span, 0, false});
            d = &derived_.back();
        }
        const bool refilled = listed(steps_.fill, u.object);
        const bool stale = refilled || d->dirty || std::any_of(writes_.begin(), writes_.end(), [&](const Write &w) {
                               return w.tick >= d->drawn && w.span.overlaps(d->span);
                           });
        if (stale)
            for (Derived &x : derived_)
                if (x.object == u.object) {
                    x.drawn = clock_;
                    x.dirty = false;
                }
        return stale;
    }

    std::vector<Shadowed> shadowed_;
    std::vector<Derived> derived_;
    std::vector<Write> written_, writes_;
    Steps steps_;
    uint32_t clock_ = 0;
};

}  // namespace gm20b
