// SPDX-License-Identifier: MIT
// See compile_request.h.
#include "compile_request.h"
#include "cache_io.h"

#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <cstring>
#include <type_traits>

namespace gm20b {
namespace {

// Any change to a field's bytes or meaning takes the next magic: the compiler's request log (compiler/compiler_log.h)
// keeps requests across builds and replays them, so a request of another layout must be refused whole
constexpr char kRequestMagic[8] = {'S', 'W', 'C', 'R', 'E', 'Q', '0', '7'};
constexpr char kNameMagic[8] = {'S', 'W', 'R', 'A', 'W', 'N', 'M', '2'};
// A cubic bind's sampler flag byte: bit 0 normalized, bit 1 the vertex stage, no other; its texture's slot byte's top
// bit: the vertex stage. Each stage binds up to 16 samplers
constexpr uint8_t kCubicNormalizedBit = 1, kCubicVertexBit = 2, kCubicVertexSlot = 0x80;
constexpr uint32_t kCubicBindLimit = 32;

// A struct of plain values with no padding, written and read as its bytes: every field it has goes over the wire
template <class T> void writeWhole(Writer &w, const T &v)
{
    static_assert(std::has_unique_object_representations_v<T>, "a field or padding the request would not carry");
    w.bytes(&v, sizeof v);
}

template <class T> void readWhole(Reader &r, T &v)
{
    static_assert(std::has_unique_object_representations_v<T>, "a field or padding the request would not carry");
    r.bytes(&v, sizeof v);
}

void writeDispatch(Writer &w, const KernelRequest &k)
{
    w.value(k.maxThreads);
    w.value(uint8_t(k.multipleOfWidth));
    w.value(uint8_t(k.regionMode));
    w.bytes(k.threads, sizeof k.threads);
    w.bytes(k.memory, sizeof k.memory);
    w.list(k.textures);
    w.list(k.samplers);
}

void writeKernel(Writer &w, const KernelRequest &k)
{
    writeDispatch(w, k);
    writeWhole(w, k.stageIn);
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
    readWhole(r, k.stageIn);
}

void writeVariant(Writer &w, const RenderVariant &v)
{
    w.value(uint32_t(v.cubic.samplers.size()));
    for (const CubicBinds::Sampler &s : v.cubic.samplers) {
        const uint8_t packed[6] = {
            s.slot, s.s, s.t, s.r, s.border, uint8_t(s.normalized | (s.stage == kCubicVertex ? kCubicVertexBit : 0))};
        w.bytes(packed, sizeof packed);
    }
    w.value(uint32_t(v.cubic.textures.size()));
    for (const CubicBinds::Texture &t : v.cubic.textures) {
        w.value(uint8_t(t.slot | (t.stage == kCubicVertex ? kCubicVertexSlot : 0)));
        w.value(t.format);
    }
    w.value(v.fragmentTests);
    w.value(v.xrSlots);
    w.value(uint32_t(v.written.size()));
    for (const WrittenTexture &t : v.written) {
        w.value(t.slot);
        w.value(t.format);
    }
}

// False: more binds than a variant has, or a sampler flag this layout does not define
bool readVariant(Reader &r, RenderVariant &v)
{
    const uint32_t samplers = r.value<uint32_t>();
    if (!r.ok() || samplers > kCubicBindLimit)
        return false;
    for (uint32_t i = 0; i < samplers; ++i) {
        uint8_t packed[6] = {};
        r.bytes(packed, sizeof packed);
        if (packed[5] & ~(kCubicNormalizedBit | kCubicVertexBit))
            return false;
        const uint8_t stage = packed[5] & kCubicVertexBit ? kCubicVertex : kCubicFragment;
        v.cubic.samplers.push_back(
            {packed[0], packed[1], packed[2], packed[3], packed[4], (packed[5] & kCubicNormalizedBit) != 0, stage});
    }
    const uint32_t textures = r.value<uint32_t>();
    if (!r.ok() || textures > kCubicBindLimit)
        return false;
    for (uint32_t i = 0; i < textures; ++i) {
        const uint8_t slot = r.value<uint8_t>();
        const uint8_t stage = slot & kCubicVertexSlot ? kCubicVertex : kCubicFragment;
        v.cubic.textures.push_back({uint8_t(slot & ~kCubicVertexSlot), r.value<uint32_t>(), stage});
    }
    v.fragmentTests = r.value<uint8_t>();
    v.xrSlots = r.value<uint16_t>();
    const uint32_t written = r.value<uint32_t>();
    if (!r.ok() || written > kWrittenSlots)
        return false;
    for (uint32_t i = 0; i < written; ++i) {
        const uint8_t slot = r.value<uint8_t>();
        v.written.push_back({slot, r.value<uint32_t>()});
    }
    return r.ok();
}

bool knownTag(const CompileRequest &r, size_t index)
{
    const std::string &tag = r.stages[index].tag;
    if (r.kind == RequestKind::Kernel || r.kind == RequestKind::Facts || computeReflection(r))
        return index == 0 ? tag == "k" : tag == "kl" || tag == "kp" || tag == "kb" || tag == "kg";
    if (functionAttributes(r))
        return tag == "a";
    if (bufferEncoders(r))
        return tag == "b";
    if (index == 0)
        return tag == "v";
    if (index == 1 && tag == "f")
        return true;
    return tag == "vl" || tag == "vp" || tag == "fl" || tag == "fp";
}

bool knownCodeGen(uint32_t type)
{
    return type == kCodeGenSource || type == kCodeGenStitch || type == kCodeGenImageFilters ||
           type == kCodeGenSpecialize;
}

bool saneLibrary(const CompileRequest &r)
{
    return r.stages.empty() && r.libraries.empty() && r.variant.name().empty() && r.kernel.textures.empty() &&
           r.kernel.samplers.empty() && !r.key.empty() && r.form == RequestForm::Primary && !r.codeGen.empty() &&
           knownCodeGen(r.codeGenType);
}

bool sane(const CompileRequest &r)
{
    if (r.kind == RequestKind::Library)
        return saneLibrary(r);
    const bool kernel = r.kind == RequestKind::Kernel || r.kind == RequestKind::Facts || computeReflection(r) ||
                        functionAttributes(r) || bufferEncoders(r);
    if (r.stages.empty() || ((functionAttributes(r) || bufferEncoders(r)) && r.stages.size() != 1) ||
        r.stages.size() > kRequestStageLimit || r.libraries.empty() || r.libraries.size() > kRequestLibraryLimit ||
        r.kernel.textures.size() > kRequestBindLimit || r.kernel.samplers.size() > kRequestBindLimit || r.key.empty() ||
        ((kernel || r.kind == RequestKind::Reflection) && r.form != RequestForm::Primary))
        return false;
    for (size_t i = 0; i < r.stages.size(); ++i)
        if (!knownTag(r, i) || r.stages[i].library >= r.libraries.size() || r.stages[i].function.empty() ||
            (r.stages[i].tag != "kg" && !r.stages[i].group.empty()))
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

size_t fragmentStage(const CompileRequest &r)
{
    return r.stages.size() > 1 && r.stages[1].tag == "f" ? 1 : kNoStage;
}

size_t firstLinked(const CompileRequest &r)
{
    return fragmentStage(r) == kNoStage ? 1 : 2;
}

std::string encodeRequest(const CompileRequest &r)
{
    Writer w;
    w.bytes(kRequestMagic, sizeof kRequestMagic);
    w.value(r.stack);
    w.value(uint8_t(r.kind));
    w.value(uint8_t(r.form));
    w.text(r.key);
    writeWhole(w, r.fields);
    w.value(uint32_t(r.stages.size()));
    for (const RequestStage &s : r.stages) {
        w.text(s.tag);
        w.text(s.function);
        w.text(s.constants);
        w.text(s.name);
        w.value(uint8_t(s.specialized));
        w.value(s.library);
        w.text(s.group);
    }
    w.value(uint32_t(r.libraries.size()));
    for (const RequestLibrary &l : r.libraries) {
        w.text(l.path);
        w.value(uint8_t(l.system));
        w.text(l.bytes);
    }
    writeVariant(w, r.variant);
    writeKernel(w, r.kernel);
    if (r.kind == RequestKind::Library) {
        w.value(r.codeGenType);
        w.text(r.codeGen);
    }
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
    q.kind = RequestKind(kind);
    q.form = form ? RequestForm::Split : RequestForm::Primary;
    q.key = r.text();
    readWhole(r, q.fields);
    const uint32_t stages = r.value<uint32_t>();
    if (!r.ok() || memcmp(magic, kRequestMagic, sizeof magic) || kind > uint8_t(RequestKind::Reflection) || form > 1 ||
        stages > kRequestStageLimit)
        return false;
    q.stages.resize(stages);
    for (RequestStage &s : q.stages) {
        s.tag = r.text();
        s.function = r.text();
        s.constants = r.text();
        s.name = r.text();
        s.specialized = r.value<uint8_t>() != 0;
        s.library = r.value<uint32_t>();
        s.group = r.text();
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
    if (!readVariant(r, q.variant))
        return false;
    readKernel(r, q.kernel);
    if (q.kind == RequestKind::Library) {
        q.codeGenType = r.value<uint32_t>();
        q.codeGen = r.text();
    }
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
        if (s.tag == "kg")
            hashText(c, s.group);
        const LibraryIdentity none;
        const LibraryIdentity &l = s.library < libraries.size() ? libraries[s.library] : none;
        hashValue(c, uint8_t(l.system));
        CC_SHA256_Update(&c, l.digest.data(), CC_LONG(l.digest.size()));
    }
    if (r.kind == RequestKind::Render) {
        hashValue(c, r.fields.rasterization);
    } else {
        // the stage input is bound by the key already: a kernel keeps the name it was stored under before requests
        // carried it
        Writer w;
        writeDispatch(w, r.kernel);
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

bool computeReflection(const CompileRequest &r)
{
    return r.kind == RequestKind::Reflection && !r.stages.empty() && r.stages[0].tag == "k";
}

CacheKey computeReflectionKey(uint64_t stack, const std::string &stateKey)
{
    CacheKey k;
    k.stack = stack;
    k.key = "reflection | " + stateKey;
    return k;
}

bool functionAttributes(const CompileRequest &r)
{
    return r.kind == RequestKind::Reflection && !r.stages.empty() && r.stages[0].tag == "a";
}

CacheKey functionAttributesCacheKey(uint64_t stack, const std::string &functionKey)
{
    CacheKey k;
    k.stack = stack;
    k.key = "attributes | " + functionKey;
    return k;
}

bool bufferEncoders(const CompileRequest &r)
{
    return r.kind == RequestKind::Reflection && !r.stages.empty() && r.stages[0].tag == "b";
}

CacheKey bufferEncodersCacheKey(uint64_t stack, const std::string &functionKey)
{
    CacheKey k;
    k.stack = stack;
    k.key = "buffer encoders | " + functionKey;
    return k;
}

CacheKey factsCacheKey(uint64_t stack, const std::string &stateKey)
{
    CacheKey k;
    k.stack = stack;
    k.key = "facts | " + stateKey;
    return k;
}

CacheKey libraryCacheKey(uint64_t stack, uint32_t type, const std::string &bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    const Digest digest = sha256(bytes.data(), bytes.size());
    std::string hex;
    for (const uint8_t b : digest) {
        hex += kDigits[b >> 4];
        hex += kDigits[b & 15];
    }
    CacheKey k;
    k.stack = stack;
    k.key = "library | " + std::to_string(type) + " | " + hex;
    return k;
}

}  // namespace gm20b
