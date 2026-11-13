#include "hw_test_app/bench/bench_records.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <tuple>
#include "drivers/TegraGM20BMetal/raw/runtime/raw_owned.h"

namespace bench {
namespace {

constexpr uint32_t kSetCtWrite0 = 0x1a00;  // SET_CT_WRITE(0)
constexpr uint32_t kCtWriteRgba = 0x1111;  // its R, G, B and A enables
constexpr uint32_t kFloat4 = 31;           // MTLVertexFormatFloat4

// tools/benchloop/protocol.py METHOD_RANGES
constexpr raw::MethodRange kAllowed[] = {
    {0x02E0, 0x02E4}, {0x02F4, 0x02F8}, {0x0754, 0x0758}, {0x0D9C, 0x0DA0}, {0x0DD0, 0x0DD8}, {0x0DF0, 0x0DF8},
    {0x0F40, 0x0F54}, {0x0F90, 0x0F94}, {0x0FAC, 0x0FB0}, {0x0FDC, 0x0FE0}, {0x10CC, 0x10D0}, {0x10E0, 0x10F4},
    {0x113C, 0x1140}, {0x1140, 0x1144}, {0x1220, 0x1224}, {0x1224, 0x1228}, {0x12E0, 0x12E8}, {0x131C, 0x132C},
    {0x133C, 0x1360}, {0x1360, 0x1380}, {0x13A8, 0x13AC}, {0x1480, 0x1500}, {0x15B8, 0x15BC}, {0x19C0, 0x19C4},
    {0x1A00, 0x1A20}, {0x1E00, 0x1F00}, {0x2600, 0x2604},
};

struct Color {
    unsigned format = 0, mask = 0, blend = 0;
};

bool color(const char *key, unsigned index, Color &c)
{
    char tag[8];
    snprintf(tag, sizeof tag, " c%u=", index);
    const char *at = strstr(key, tag);
    return at && sscanf(at + strlen(tag), "%u:%u:%u", &c.format, &c.mask, &c.blend) == 3;
}

unsigned colors(const char *key)
{
    unsigned n = 0;
    for (Color c; n < 8 && color(key, n, c);)
        n++;
    return n;
}

bool plainEntries(const raw::PackPipeline &p)
{
    for (uint32_t k = 0; k < p.entries && k < raw::kPackEntries; k++) {
        const raw::PackEntry &e = p.entry[k];
        const bool ubo = e.kind == raw::EntryUbo && !raw::bufferStorage(e);
        if (!ubo && e.kind != raw::EntryTexture && e.kind != raw::EntrySamplerOnly)
            return false;
    }
    return true;
}

bool positionFirst(const raw::PackPipeline &p, const char *key)
{
    unsigned format = 0, offset = 0, buffer = 0, stride = 0, step = 0;
    const char *a0 = strstr(key, " a0=");
    return a0 && sscanf(a0, " a0=%u:%u:%u:%u:%u", &format, &offset, &buffer, &stride, &step) == 5 &&
           format == kFloat4 && offset == 0 && buffer == p.vertexSlot && stride == p.vertexStride && step == 1;
}

// Its first set-0 texture whose sampler is the draw's (a slot, not constexpr)
const raw::PackEntry *sampledTexture(const raw::PackPipeline &p)
{
    for (uint32_t k = 0; k < p.entries && k < raw::kPackEntries; k++)
        if (p.entry[k].kind == raw::EntryTexture && p.entry[k].samplerKind == raw::SamplerSlot)
            return &p.entry[k];
    return nullptr;
}

// A bound TEX (opcode bits 63..59 11000) or TLD (63..56 0xdc) instruction word's texture index: bits 36..48. TEX's
// level mode, offset and depth flags sit among its opcode bits (NAK sm50.rs), so only the fixed ones are compared
constexpr uint64_t kTexIndexMask = 0x1fffull << 36;

bool boundFetch(uint64_t word)
{
    return word >> 59 == 0x18 || word >> 56 == 0xdc;
}

template <typename Visit> void fetchWords(std::vector<uint8_t> &code, Visit visit)
{
    for (size_t at = 0; at + 8 <= code.size(); at += 8) {
        if (at / 8 % 4 == 0)
            continue;  // a bundle's scheduling word
        uint64_t word;
        memcpy(&word, code.data() + at, 8);
        if (boundFetch(word) && visit(word))
            memcpy(code.data() + at, &word, 8);
    }
}

bool plainState(const raw::PackPipeline &p, const char *key)
{
    Color c0;
    return !strncmp(key, "samples=1 ", 10) && strstr(key, " ds=0/0 ") && color(key, 0, c0) && c0.mask == 15 &&
           !c0.blend && !(p.flags & (raw::PipelineFetches | raw::PipelineBarrierForm | raw::PipelineStorageMetadata)) &&
           !p.storageMask && !p.shaderTests.flags && !p.blendColorWord && !p.halfBlendColorWord &&
           p.vertexSlot != raw::kNoSlot && p.vertexStride >= 16 && plainEntries(p) && positionFirst(p, key);
}

raw::CapturedProgram captured(const raw::Pack &pack, uint32_t index)
{
    const raw::PackProgram &p = pack.section<raw::PackProgram>(raw::PackPrograms)[index];
    const uint8_t *heap = pack.section<uint8_t>(raw::PackHeap);
    raw::CapturedProgram c;
    c.id = "bench-pack-" + std::to_string(index);
    c.image.assign(heap + p.program, heap + p.heapOffset + (p.dataBytes ? p.dataOffset : p.bytes));
    if (p.dataBytes)
        c.data.assign(heap + p.heapOffset + p.dataOffset, heap + p.heapOffset + p.dataOffset + p.dataBytes);
    c.cbufTypes.assign(p.cbufType, p.cbufType + std::min<uint8_t>(p.cbufs, sizeof p.cbufType));
    c.gprs = p.gprs;
    c.boundTextures = p.boundTextures;
    return c;
}

std::string programId(const Program &p)
{
    std::vector<uint8_t> all = p.sph;
    all.insert(all.end(), p.code.begin(), p.code.end());
    all.insert(all.end(), p.data.begin(), p.data.end());
    char id[40];
    snprintf(id, sizeof id, "bench-%016llx-%u-%u", (unsigned long long)raw::Pack::fnv(all.data(), all.size()), p.gprs,
             p.dataBound);
    return id;
}

// "c0=<format>:" of the key with the target's format
bool retarget(std::string &key, uint32_t format)
{
    const size_t at = key.find(" c0=");
    const size_t colon = at == std::string::npos ? at : key.find(':', at);
    if (colon == std::string::npos)
        return false;
    key.replace(at + 4, colon - at - 4, std::to_string(format));
    return true;
}

std::string tag(unsigned colour)
{
    return " c" + std::to_string(colour) + "=";
}

// Shader data in whole 256-byte steps, as NVK uploads it and the pack's dataBytes always are
std::vector<uint8_t> padded(std::vector<uint8_t> data)
{
    data.resize((data.size() + 0xff) & ~size_t(0xff));
    return data;
}

void setWord(std::vector<uint32_t> &blob, uint32_t method, uint32_t value)
{
    bool found = false;
    for (size_t k = 0; k + 1 < blob.size(); k += 2)
        if (blob[k] == method) {
            blob[k + 1] = value;
            found = true;
        }
    if (!found)
        blob.insert(blob.end(), {method, value});
}

void setQmd(uint32_t *qmd, unsigned hi, unsigned lo, uint32_t v)
{
    for (unsigned b = lo; b <= hi; b++, v >>= 1)
        qmd[b / 32] = (qmd[b / 32] & ~(1u << (b % 32))) | (v & 1) << (b % 32);
}

}  // namespace

bool methodAllowed(uint32_t method)
{
    if (method % 4 || raw::ownedMethod(method))
        return false;
    return std::any_of(std::begin(kAllowed), std::end(kAllowed), [&](const raw::MethodRange &r) {
        return method >= r.begin && method < r.end;
    });
}

std::string withoutOtherColours(std::string key)
{
    for (unsigned c = 1; c < 8; c++) {
        const size_t at = key.find(tag(c));
        if (at == std::string::npos)
            continue;
        const size_t end = key.find(' ', at + 1);
        key.erase(at, end == std::string::npos ? std::string::npos : end - at);
    }
    return key;
}

const raw::PackPipeline *fragmentTemplate(const raw::Pack &pack)
{
    const raw::PackPipeline *pipelines = pack.section<raw::PackPipeline>(raw::PackPipelines);
    const raw::PackPipeline *best = nullptr;
    auto rank = [&](const raw::PackPipeline &p) {
        return std::make_tuple(colors(pack.string(p.keyString)), p.entries, p.keyHash);
    };
    for (uint32_t i = 0; i < pack.header()->pipelines; i++) {
        const raw::PackPipeline &p = pipelines[i];
        if (plainState(p, pack.string(p.keyString)) && (!best || rank(p) < rank(*best)))
            best = &p;
    }
    return best;
}

const raw::PackPipeline *textureTemplate(const raw::Pack &pack)
{
    const raw::PackPipeline *pipelines = pack.section<raw::PackPipeline>(raw::PackPipelines);
    const raw::PackPipeline *best = nullptr;
    auto rank = [&](const raw::PackPipeline &p) {
        return std::make_tuple(colors(pack.string(p.keyString)), p.entries, p.keyHash);
    };
    for (uint32_t i = 0; i < pack.header()->pipelines; i++) {
        const raw::PackPipeline &p = pipelines[i];
        if (plainState(p, pack.string(p.keyString)) && sampledTexture(p) && (!best || rank(p) < rank(*best)))
            best = &p;
    }
    return best;
}

bool samplesTexture(const Program &p)
{
    std::vector<uint8_t> code = p.code;
    bool found = false;
    fetchWords(code, [&](uint64_t &word) {
        found |= (word & kTexIndexMask) == uint64_t(kTextureIndexPlaceholder) << 36;
        return false;
    });
    return found;
}

const raw::PackPipeline *fragmentTemplate(const raw::Pack &pack, uint64_t key)
{
    const raw::PackPipeline *pipelines = pack.section<raw::PackPipeline>(raw::PackPipelines);
    for (uint32_t i = 0; i < pack.header()->pipelines; i++)
        if (pipelines[i].keyHash == key)
            return plainState(pipelines[i], pack.string(pipelines[i].keyString)) ? &pipelines[i] : nullptr;
    return nullptr;
}

const raw::PackKernel *kernelTemplate(const raw::Pack &pack)
{
    const raw::PackKernel *kernels = pack.section<raw::PackKernel>(raw::PackKernels);
    const raw::PackProgram *programs = pack.section<raw::PackProgram>(raw::PackPrograms);
    const raw::PackKernel *best = nullptr;
    for (uint64_t i = 0; i < pack.bytes(raw::PackKernels) / sizeof(raw::PackKernel); i++) {
        const raw::PackKernel &k = kernels[i];
        const raw::PackProgram &p = programs[k.program];
        if (p.cbufs && raw::cbufKind(p.cbufType[0]) == raw::CbufRoot && (k.qmd[20] & 1) &&
            (!best || std::tie(k.entries, k.keyHash) < std::tie(best->entries, best->keyHash)))
            best = &k;
    }
    return best;
}

bool fragmentRecord(const raw::Pack &pack, const raw::PackPipeline &tpl, const Program &fs, uint32_t format,
                    uint64_t key, const Methods &methods, const Methods &restore, raw::RuntimeRecord &r,
                    std::string &why, bool firstColourOnly)
{
    r = raw::RuntimeRecord();
    r.pipeline = tpl;
    r.pipeline.keyHash = key;
    r.keyString = pack.string(tpl.keyString);
    if (firstColourOnly)
        r.keyString = withoutOtherColours(r.keyString);
    if (!retarget(r.keyString, format)) {
        why = "the template's key names no first colour";
        return false;
    }
    r.programs[0] = captured(pack, tpl.vs);
    raw::CapturedProgram &p = r.programs[1];
    p.id = programId(fs);
    p.image = fs.sph;
    p.image.insert(p.image.end(), fs.code.begin(), fs.code.end());
    p.data = padded(fs.data);
    if (fs.dataBound)
        p.data.resize(fs.dataBound);
    p.cbufTypes = {raw::CbufRoot};
    if (!fs.data.empty())
        p.cbufTypes.push_back(raw::CbufShaderData);
    p.gprs = uint8_t(fs.gprs);
    if (samplesTexture(fs)) {
        const raw::PackEntry *texture = sampledTexture(tpl);
        if (!texture) {
            why = "the program samples a texture and the template has none with a sampler slot";
            return false;
        }
        // the program's bound fetches read the texture's handle where the set-0 entry holds it, set 0 a buffer of its own
        std::vector<uint8_t> code(p.image.begin() + 80, p.image.end());
        fetchWords(code, [&](uint64_t &word) {
            if ((word & kTexIndexMask) != uint64_t(kTextureIndexPlaceholder) << 36)
                return false;
            word = (word & ~kTexIndexMask) | uint64_t(texture->offset / 4) << 36;
            return true;
        });
        std::copy(code.begin(), code.end(), p.image.begin() + 80);
        p.cbufTypes.push_back(raw::CbufSet);
        p.boundTextures = uint8_t(p.cbufTypes.size());
    }
    const uint32_t *blob = pack.section<uint32_t>(raw::PackBlob) + tpl.blob;
    r.blob.assign(blob, blob + tpl.blobDwords);
    // The key's write mask is Metal's: a state whose format its pixel program packs into one word (BGR10_XR, the
    // device's template) has the hardware write R alone
    setWord(r.blob, kSetCtWrite0, kCtWriteRgba);
    r.pipeline.blobDwords = uint32_t(r.blob.size());
    for (const Methods *list : {&methods, &restore})
        for (const auto &[m, v] : *list)
            if (!methodAllowed(m)) {
                char line[64];
                snprintf(line, sizeof line, "method %#x is not one the bench may set", m);
                why = line;
                return false;
            }
    r.extras = methods;
    r.restore = restore;
    const uint32_t *samplers = pack.section<uint32_t>(raw::PackSamplers);
    for (uint32_t i = 0; i < pack.header()->samplers; i++) {
        std::array<uint32_t, 8> words;
        memcpy(words.data(), samplers + 8 * i, sizeof words);
        r.samplers.push_back(words);
    }
    return true;
}

bool kernelRecord(const raw::PackKernel &tpl, const Program &kernel, const uint32_t threads[3], raw::ComputeRecord &r,
                  std::string &why)
{
    if (kernel.data.size() > 0x10000) {
        why = "shader data past a constant buffer's 64 KiB";
        return false;
    }
    r = raw::ComputeRecord();
    r.code = kernel.code;
    r.data = padded(kernel.data);
    r.kernel = tpl;
    r.kernel.keyHash = 0;
    r.kernel.entries = 0;
    r.kernel.regionWord = -1;
    r.kernel.setBytes = 0;
    r.kernel.pushMask = 0;
    uint32_t *qmd = r.kernel.qmd;
    setQmd(qmd, 1503, 1496, kernel.gprs);  // REGISTER_COUNT
    setQmd(qmd, 561, 544, 0);              // SHARED_MEMORY_SIZE
    setQmd(qmd, 1463, 1440, 0);            // SHADER_LOCAL_MEMORY_LOW_SIZE
    setQmd(qmd, 1495, 1472, 0);            // SHADER_LOCAL_MEMORY_HIGH_SIZE
    setQmd(qmd, 607, 592, threads[0]);     // CTA_THREAD_DIMENSION0..2
    setQmd(qmd, 623, 608, threads[1]);
    setQmd(qmd, 639, 624, threads[2]);
    setQmd(qmd, 647, 640, kernel.data.empty() ? 1 : 3);  // CONSTANT_BUFFER_VALID: the root table, the shader data
    if (!kernel.data.empty())
        setQmd(qmd, 1055, 1039, uint32_t(r.data.size()));  // CONSTANT_BUFFER_SIZE(1)
    r.cbufType[0] = raw::CbufRoot;
    r.cbufType[1] = kernel.data.empty() ? 0 : raw::CbufShaderData;
    r.cbufs = kernel.data.empty() ? 1 : 2;
    r.keyString = "bench kernel";
    return true;
}

}  // namespace bench
