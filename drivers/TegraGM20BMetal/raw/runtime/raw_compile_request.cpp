// SPDX-License-Identifier: MIT
// See raw_compile_request.h.
#include "raw_compile_request.h"
#include "raw_cache_io.h"

#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <cstring>

namespace raw {
namespace {

constexpr char kRequestMagic[8] = {'S', 'W', 'C', 'R', 'E', 'Q', '0', '2'};
constexpr char kNameMagic[8] = {'S', 'W', 'R', 'A', 'W', 'N', 'M', '2'};

void writeFields(Writer &w, const PipelineFields &f)
{
    w.value(f.samples);
    w.value(f.topology);
    w.value(f.depthFormat);
    w.value(f.stencilFormat);
    w.value(f.rasterization);
    for (const ColorFields &c : f.color)
        w.bytes(&c, sizeof c);
    for (const AttributeFields &a : f.attributes)
        w.bytes(&a, sizeof a);
}

void readFields(Reader &r, PipelineFields &f)
{
    f.samples = r.value<uint64_t>();
    f.topology = r.value<uint64_t>();
    f.depthFormat = r.value<uint64_t>();
    f.stencilFormat = r.value<uint64_t>();
    f.rasterization = r.value<uint64_t>();
    for (ColorFields &c : f.color)
        r.bytes(&c, sizeof c);
    for (AttributeFields &a : f.attributes)
        r.bytes(&a, sizeof a);
}

void writeKernel(Writer &w, const KernelRequest &k)
{
    w.value(k.maxThreads);
    w.value(uint8_t(k.multipleOfWidth));
    w.value(uint8_t(k.regionMode));
    w.bytes(k.threads, sizeof k.threads);
    w.bytes(k.memory, sizeof k.memory);
    w.list(k.textures);
    w.list(k.samplers);
}

void readKernel(Reader &r, KernelRequest &k)
{
    k.maxThreads = r.value<uint32_t>();
    k.multipleOfWidth = r.value<uint8_t>() != 0;
    k.regionMode = r.value<uint8_t>() != 0;
    r.bytes(k.threads, sizeof k.threads);
    r.bytes(k.memory, sizeof k.memory);
    k.textures = r.list<NmbDirectBoundTexture>();
    k.samplers = r.list<NmbDirectBoundSampler>();
}

bool knownTag(const CompileRequest &r, size_t index)
{
    const std::string &tag = r.stages[index].tag;
    if (r.kind == RequestKind::Kernel)
        return tag == "k";
    if (index < 2)
        return tag == (index ? "f" : "v");
    return tag == "vl" || tag == "vp" || tag == "fl" || tag == "fp";
}

bool sane(const CompileRequest &r)
{
    const bool kernel = r.kind == RequestKind::Kernel;
    if ((kernel ? r.stages.size() != 1 : r.stages.size() < 2) || r.stages.size() > kRequestStageLimit ||
        r.libraries.empty() || r.libraries.size() > kRequestLibraryLimit || r.cubic.samplers.size() > 16 ||
        r.cubic.textures.size() > 16 || r.kernel.textures.size() > kRequestBindLimit ||
        r.kernel.samplers.size() > kRequestBindLimit || r.key.empty() || (kernel && r.form != RequestForm::Primary))
        return false;
    for (size_t i = 0; i < r.stages.size(); ++i)
        if (!knownTag(r, i) || r.stages[i].library >= r.libraries.size() || r.stages[i].function.empty())
            return false;
    for (const RequestLibrary &l : r.libraries)
        if (l.system ? !l.bytes.empty() || !systemLibraryPath(l.path) : l.bytes.empty())
            return false;
    return true;
}

void hashText(CC_SHA256_CTX &c, const std::string &text)
{
    const uint32_t size = uint32_t(text.size());
    CC_SHA256_Update(&c, &size, sizeof size);
    CC_SHA256_Update(&c, text.data(), CC_LONG(text.size()));
}

template <class T> void hashValue(CC_SHA256_CTX &c, const T &value)
{
    CC_SHA256_Update(&c, &value, sizeof value);
}

}  // namespace

NmbDirectDispatch KernelRequest::dispatch() const
{
    NmbDirectDispatch d = {};
    memcpy(d.threads, threads, sizeof d.threads);
    memcpy(d.memory, memory, sizeof d.memory);
    d.textures = textures.data();
    d.textureCount = uint32_t(textures.size());
    d.samplers = samplers.data();
    d.samplerCount = uint32_t(samplers.size());
    d.regionMode = regionMode;
    return d;
}

std::string encodeRequest(const CompileRequest &r)
{
    Writer w;
    w.bytes(kRequestMagic, sizeof kRequestMagic);
    w.value(r.stack);
    w.value(uint8_t(r.kind));
    w.value(uint8_t(r.form));
    w.text(r.key);
    writeFields(w, r.fields);
    w.value(uint32_t(r.stages.size()));
    for (const RequestStage &s : r.stages) {
        w.text(s.tag);
        w.text(s.function);
        w.text(s.constants);
        w.text(s.name);
        w.value(uint8_t(s.specialized));
        w.value(s.library);
    }
    w.value(uint32_t(r.libraries.size()));
    for (const RequestLibrary &l : r.libraries) {
        w.text(l.path);
        w.value(uint8_t(l.system));
        w.text(l.bytes);
    }
    w.value(uint32_t(r.cubic.samplers.size()));
    for (const CubicBinds::Sampler &s : r.cubic.samplers) {
        const uint8_t packed[6] = {s.slot, s.s, s.t, s.r, s.border, uint8_t(s.normalized)};
        w.bytes(packed, sizeof packed);
    }
    w.value(uint32_t(r.cubic.textures.size()));
    for (const CubicBinds::Texture &t : r.cubic.textures) {
        w.value(t.slot);
        w.value(t.format);
    }
    w.value(r.fragmentTests);
    writeKernel(w, r.kernel);
    return w.out;
}

bool decodeRequest(const std::string &bytes, CompileRequest &out)
{
    if (bytes.size() > kRequestLimit)
        return false;
    Reader r(bytes);
    char magic[sizeof kRequestMagic];
    r.bytes(magic, sizeof magic);
    CompileRequest q;
    q.stack = r.value<uint64_t>();
    const uint8_t kind = r.value<uint8_t>(), form = r.value<uint8_t>();
    q.kind = kind ? RequestKind::Kernel : RequestKind::Render;
    q.form = form ? RequestForm::Split : RequestForm::Primary;
    q.key = r.text();
    readFields(r, q.fields);
    const uint32_t stages = r.value<uint32_t>();
    if (!r.ok() || memcmp(magic, kRequestMagic, sizeof magic) || kind > 1 || form > 1 || stages > kRequestStageLimit)
        return false;
    q.stages.resize(stages);
    for (RequestStage &s : q.stages) {
        s.tag = r.text();
        s.function = r.text();
        s.constants = r.text();
        s.name = r.text();
        s.specialized = r.value<uint8_t>() != 0;
        s.library = r.value<uint32_t>();
    }
    const uint32_t libraries = r.value<uint32_t>();
    if (!r.ok() || libraries > kRequestLibraryLimit)
        return false;
    q.libraries.resize(libraries);
    for (RequestLibrary &l : q.libraries) {
        l.path = r.text();
        l.system = r.value<uint8_t>() != 0;
        l.bytes = r.text();
    }
    const uint32_t samplers = r.value<uint32_t>();
    if (!r.ok() || samplers > 16)
        return false;
    for (uint32_t i = 0; i < samplers; ++i) {
        uint8_t packed[6] = {};
        r.bytes(packed, sizeof packed);
        q.cubic.samplers.push_back({packed[0], packed[1], packed[2], packed[3], packed[4], packed[5] != 0});
    }
    const uint32_t textures = r.value<uint32_t>();
    if (!r.ok() || textures > 16)
        return false;
    for (uint32_t i = 0; i < textures; ++i) {
        const uint8_t slot = r.value<uint8_t>();
        q.cubic.textures.push_back({slot, r.value<uint32_t>()});
    }
    q.fragmentTests = r.value<uint8_t>();
    readKernel(r, q.kernel);
    if (!r.ok() || r.at() != bytes.size() || !sane(q))
        return false;
    out = std::move(q);
    return true;
}

Digest sha256(const void *bytes, size_t size)
{
    Digest out{};
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    for (size_t at = 0; at < size;) {
        const size_t chunk = std::min<size_t>(size - at, 1u << 30);
        CC_SHA256_Update(&c, static_cast<const uint8_t *>(bytes) + at, CC_LONG(chunk));
        at += chunk;
    }
    CC_SHA256_Final(out.data(), &c);
    return out;
}

Digest programSlotName(const void *slot, size_t size)
{
    static constexpr char kDomain[] = "GM20B program slot";
    Digest out{};
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    CC_SHA256_Update(&c, kDomain, CC_LONG(sizeof kDomain));
    for (size_t at = 0; at < size;) {
        const size_t chunk = std::min<size_t>(size - at, 1u << 30);
        CC_SHA256_Update(&c, static_cast<const uint8_t *>(slot) + at, CC_LONG(chunk));
        at += chunk;
    }
    CC_SHA256_Final(out.data(), &c);
    return out;
}

LibraryIdentity identityOf(const RequestLibrary &library)
{
    LibraryIdentity id;
    id.system = library.system;
    id.digest = library.system ? sha256(library.path.data(), library.path.size())
                               : sha256(library.bytes.data(), library.bytes.size());
    return id;
}

Digest storeName(const CacheKey &k, const CompileRequest &r, const std::vector<LibraryIdentity> &libraries)
{
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    CC_SHA256_Update(&c, kNameMagic, sizeof kNameMagic);
    hashValue(c, k.stack);
    CC_SHA256_Update(&c, k.vertexAir, sizeof k.vertexAir);
    CC_SHA256_Update(&c, k.fragmentAir, sizeof k.fragmentAir);
    hashText(c, k.key);
    hashValue(c, uint8_t(r.kind));
    hashValue(c, uint32_t(r.stages.size()));
    for (const RequestStage &s : r.stages) {
        hashText(c, s.tag);
        hashText(c, s.function);
        hashText(c, s.constants);
        hashText(c, s.name);
        hashValue(c, uint8_t(s.specialized));
        const LibraryIdentity none;
        const LibraryIdentity &l = s.library < libraries.size() ? libraries[s.library] : none;
        hashValue(c, uint8_t(l.system));
        CC_SHA256_Update(&c, l.digest.data(), CC_LONG(l.digest.size()));
    }
    if (r.kind == RequestKind::Render) {
        hashValue(c, r.fields.rasterization);
    } else {
        Writer w;
        writeKernel(w, r.kernel);
        hashText(c, w.out);
    }
    Digest out{};
    CC_SHA256_Final(out.data(), &c);
    return out;
}

CacheKey kernelCacheKey(uint64_t stack, const std::string &computeKey)
{
    CacheKey k;
    k.stack = stack;
    k.key = "kernel | " + computeKey;
    return k;
}

}  // namespace raw
