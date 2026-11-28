// SPDX-License-Identifier: MIT
// See store_key.h.
#include "store_key.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include "function_tags.h"
#include "plugin/MetallibSlice.h"

namespace gm20b {
namespace {

// Each key's own magic; any change to what goes into a content takes the next one, so no reader takes an entry of the
// layout before
constexpr char kContentMagic[8] = {'S', 'W', 'C', 'O', 'N', 'T', '0', '3'};

constexpr uint64_t rotl(uint64_t x, int r)
{
    return x << r | x >> (64 - r);
}

constexpr uint64_t finish(uint64_t h)
{
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    return h ^ (h >> 33);
}

// The content's bytes, and its name folded alongside: each 8-byte word into one of four lanes in turn, then the lanes
// finished into each other so every byte of the name depends on every word (the store probes from the name's first 4)
class Builder {
public:
    std::string out;

    void bytes(const void *p, size_t n)
    {
        out.append(static_cast<const char *>(p), n);
        const uint8_t *b = static_cast<const uint8_t *>(p);
        for (; n >= 8; b += 8, n -= 8) {
            uint64_t w = 0;
            memcpy(&w, b, 8);
            fold(w);
        }
        if (n) {
            uint64_t w = uint64_t(n) << 56;
            memcpy(&w, b, n);
            fold(w);
        }
    }

    template <class T> void value(T v)
    {
        bytes(&v, sizeof v);
    }

    void text(const std::string &s)
    {
        value(uint32_t(s.size()));
        bytes(s.data(), s.size());
    }

    void word(uint64_t w)
    {
        value(w);
    }

    Digest name() const
    {
        uint64_t h[4];
        h[0] = finish(lanes_[0] ^ rotl(lanes_[1], 17) ^ rotl(lanes_[2], 31) ^ rotl(lanes_[3], 47) ^ at_);
        for (int i = 1; i < 4; i++)
            h[i] = finish(lanes_[i] ^ rotl(h[i - 1], 13));
        Digest out{};
        memcpy(out.data(), h, sizeof h);
        return out;
    }

private:
    uint64_t lanes_[4] = {0x243f6a8885a308d3ull, 0x13198a2e03707344ull, 0xa4093822299f31d0ull, 0x082efa98ec4e6c89ull};
    uint64_t at_ = 0;

    void fold(uint64_t w)
    {
        uint64_t &h = lanes_[at_++ & 3];
        h = (h ^ w) * 0x9e3779b97f4a7c15ull;
        h ^= h >> 29;
    }
};

bool renderKind(const CompileRequest &r)
{
    return r.kind == RequestKind::Render ||
           (r.kind == RequestKind::Reflection && !computeReflection(r) && !functionAttributes(r) && !bufferEncoders(r));
}

// A stage's part: its tag, its library function's name (a library may state one HASH for many functions: Core Image's,
// CoreUI's and Vision's, a dynamic library's), AIR, facts and constants; a linked function's name, which its caller
// links by, and a function's attributes' name, which their key holds (function_attributes.h)
void addStage(Builder &b, const RequestStage &s, const StageContent &f, bool named)
{
    b.text(s.tag);
    b.text(s.function);
    b.bytes(f.air.data(), f.air.size());
    b.text(f.facts);
    b.value(uint8_t(s.specialized));
    b.text(s.constants);
    if (named)
        b.text(s.name);
    if (s.tag == "kg")
        b.text(s.group);
}

void addVariant(Builder &b, const RenderVariant &v)
{
    b.value(uint32_t(v.cubic.samplers.size()));
    for (const CubicBinds::Sampler &s : v.cubic.samplers)
        for (const uint64_t w : {uint64_t(s.slot), uint64_t(s.s), uint64_t(s.t), uint64_t(s.r), uint64_t(s.border),
                                 uint64_t(s.normalized), uint64_t(s.stage)})
            b.word(w);
    b.value(uint32_t(v.cubic.textures.size()));
    for (const CubicBinds::Texture &t : v.cubic.textures)
        for (const uint64_t w : {uint64_t(t.slot), uint64_t(t.format), uint64_t(t.stage)})
            b.word(w);
    b.value(v.fragmentTests);
    b.value(v.xrSlots);
    b.value(uint32_t(v.written.size()));
    for (const WrittenTexture &t : v.written) {
        b.value(t.slot);
        b.value(t.format);
    }
}

void addDispatch(Builder &b, const KernelRequest &k)
{
    b.value(k.maxThreads);
    b.value(uint8_t(k.multipleOfWidth));
    b.value(uint8_t(k.regionMode));
    b.bytes(k.threads, sizeof k.threads);
    b.bytes(k.memory, sizeof k.memory);
    b.value(uint32_t(k.textures.size()));
    b.bytes(k.textures.data(), k.textures.size() * sizeof(NmbDirectBoundTexture));
    b.value(uint32_t(k.samplers.size()));
    b.bytes(k.samplers.data(), k.samplers.size() * sizeof(NmbDirectBoundSampler));
}

}  // namespace

Digest candidateName(const Digest &name, uint32_t k)
{
    Digest out = name;
    const uint64_t step = 0x9e3779b97f4a7c15ull * k;
    for (size_t i = 0; i < sizeof step; ++i)
        out[i] ^= uint8_t(step >> (8 * i));
    return out;
}

Digest programSlotName(const void *slot, size_t size)
{
    constexpr char kSlotMagic[8] = {'S', 'W', 'S', 'L', 'O', 'T', '0', '1'};
    constexpr size_t kHead = 256, kSpread = 32;
    const uint8_t *bytes = static_cast<const uint8_t *>(slot);
    Builder b;
    b.bytes(kSlotMagic, sizeof kSlotMagic);
    b.word(size);
    b.bytes(bytes, size < kHead ? size : kHead);
    if (size >= kHead + 8 * kSpread) {
        const size_t step = (size - kHead) / kSpread & ~size_t(7);
        for (size_t i = 0; i < kSpread; i++)
            b.bytes(bytes + kHead + i * step, 8);
    }
    return b.name();
}

std::string metallibListedFacts(const uint8_t *whole, size_t wholeBytes, const std::string &name)
{
    size_t sliceAt = 0, size = 0;
    if (!switch_metal::airSlice(whole, wholeBytes, sliceAt, size))
        return std::string();
    return listedFacts(whole + sliceAt, size, name);
}

std::string metallibListedFacts(const std::string &path, const std::string &name)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return std::string();
    struct stat st;
    void *mapped = fstat(fd, &st) == 0 && st.st_size > 0
                       ? mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0)
                       : MAP_FAILED;
    close(fd);
    if (mapped == MAP_FAILED)
        return std::string();
    std::string facts = metallibListedFacts(static_cast<const uint8_t *>(mapped), size_t(st.st_size), name);
    munmap(mapped, size_t(st.st_size));
    return facts;
}

bool scoped(const CompileRequest &r)
{
    return std::any_of(r.libraries.begin(), r.libraries.end(), [](const RequestLibrary &l) {
        return !l.system;
    });
}

StoreKey storeKeyOf(const CacheKey &k, const CompileRequest &r, const std::vector<StageContent> &stages,
                    const StoreScope &scope)
{
    Builder b;
    b.bytes(kContentMagic, sizeof kContentMagic);
    b.value(k.stack);
    b.value(uint8_t(r.kind));
    b.value(uint8_t(r.form));
    const bool render = renderKind(r);
    if (r.kind != RequestKind::Library) {
        const size_t linkedFrom = render ? firstLinked(r) : 1;
        b.value(uint32_t(r.stages.size()));
        for (size_t i = 0; i < r.stages.size(); ++i)
            addStage(b, r.stages[i], i < stages.size() ? stages[i] : StageContent{},
                     i >= linkedFrom || functionAttributes(r));
    }
    if (render) {
        keyFieldWords(r.fields, [&b](uint64_t w) {
            b.word(w);
        });
        b.word(0x380);
        b.word(r.fields.patchType);
        addVariant(b, r.variant);
    } else {
        b.text(k.key);
        if (r.kind == RequestKind::Kernel)
            addDispatch(b, r.kernel);
    }
    const bool own = scoped(r);
    b.value(own ? scope.kind : 0u);
    b.text(own ? scope.identity : std::string());
    StoreKey out;
    out.content.stack = k.stack;
    memcpy(out.content.vertexAir, k.vertexAir, sizeof k.vertexAir);
    memcpy(out.content.fragmentAir, k.fragmentAir, sizeof k.fragmentAir);
    out.name = b.name();
    out.content.key = std::move(b.out);
    return out;
}

}  // namespace gm20b
