// SPDX-License-Identifier: MIT
// A draw's fragment uniforms come from the form it draws, on raw::Device (raw_device_harness.h), read back as the methods
// it submitted. The set-0 entries a kept upload stands for (PackPipeline::memoryEntries) and the uniform buffers it binds
// are those of the pipeline formFor and prepare pick: a storage form's barrier twin, or a residual-0 variant of a pack
// state. Drawn twice or more with other fragment uniform contents at one binding, each draw binds, or uploads in its
// set, the bytes that draw bound.
#include "raw_device_harness.h"
#include "../raw/runtime/raw_record.h"
#include "../raw/runtime/raw_set_reads.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B3D.h"

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3, kFragment = 2, kSlot = 1, kResidualWord = 4;
constexpr uint32_t kFragmentGroup = 4, kFragmentProgram = 5, kRootSetWord = 0x38;
// MOV R0, c[1][0x0]: a program reading the first set-0 descriptor from the set it binds at constant buffer 1
constexpr uint64_t kReadSetEntry0 = 0x4c98ull << 48 | 1ull << 34;

struct Method {
    uint32_t method, value;
};

// The 3D subchannel's methods of `words`, in order (INC, NON_INC, ONE_INC and IMMD headers)
std::vector<Method> methods(const std::vector<uint32_t> &words)
{
    std::vector<Method> out;
    for (size_t i = 0; i < words.size();) {
        const uint32_t h = words[i++], op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                       method = (h & 0xfff) << 2;
        if (op == 4) {
            if (subc == 0)
                out.push_back({method, count});
            continue;
        }
        if (op != 1 && op != 3 && op != 5)
            continue;
        for (uint32_t k = 0; k < count && i < words.size(); k++, i++)
            if (subc == 0)
                out.push_back({method + 4 * (op == 1 ? k : op == 5 && k ? 1 : 0), words[i]});
    }
    return out;
}

std::vector<Method> submitted()
{
    std::vector<Method> out;
    for (const kernel_model::Job &job : kernel_model::model.jobs) {
        const std::vector<Method> m = methods(job.words);
        out.insert(out.end(), m.begin(), m.end());
    }
    return out;
}

// The address of each constant buffer bound at the fragment stage's slot `slot`, in order
std::vector<uint64_t> fragmentBinds(const std::vector<Method> &m, uint32_t slot)
{
    std::vector<uint64_t> out;
    uint32_t high = 0, low = 0;
    for (const Method &x : m)
        if (x.method == NVB197_SET_CONSTANT_BUFFER_SELECTOR_B)
            high = x.value;
        else if (x.method == NVB197_SET_CONSTANT_BUFFER_SELECTOR_C)
            low = x.value;
        else if (x.method == NVB197_BIND_GROUP_CONSTANT_BUFFER(kFragmentGroup) && (x.value & 1) &&
                 (x.value >> 4 & 0x1f) == slot)
            out.push_back(uint64_t(high) << 32 | low);
    return out;
}

// Each value root table word `offset` took (LOAD_CONSTANT_BUFFER from LOAD_CONSTANT_BUFFER_OFFSET), in order
std::vector<uint32_t> rootWordValues(const std::vector<Method> &m, uint32_t offset)
{
    std::vector<uint32_t> out;
    uint32_t at = 0;
    for (const Method &x : m)
        if (x.method == NVB197_LOAD_CONSTANT_BUFFER_OFFSET) {
            at = x.value;
        } else if (x.method >= NVB197_LOAD_CONSTANT_BUFFER(0) && x.method <= NVB197_LOAD_CONSTANT_BUFFER(15)) {
            if (at == offset)
                out.push_back(x.value);
            at += 4;
        }
    return out;
}

std::vector<uint32_t> valuesOf(const std::vector<Method> &m, uint32_t method)
{
    std::vector<uint32_t> out;
    for (const Method &x : m)
        if (x.method == method)
            out.push_back(x.value);
    return out;
}

// A uniform buffer entry of the fragment stage at set-0 byte 0, its residual in root push word kResidualWord
raw::PackEntry fragmentUniform()
{
    return {0, raw::EntryUbo, kFragment, kSlot, uint8_t((kResidualWord + 1) << 2), 16, 0};
}

void describe(raw::PackPipeline &p, uint64_t keyHash)
{
    p.keyHash = keyHash;
    p.flags = raw::PipelinePushMetadata;
    p.entries = 1;
    p.entry[0] = fragmentUniform();
    p.setBytes = 16;
    p.pushMask = 1u << kResidualWord;
    for (raw::PackUniformRange &u : p.uniform)
        u.entry = raw::kNoUniform;
}

// A state of one fragment uniform buffer whose fragment program reads it as `fragmentCbufs` and code `code` say
raw::RuntimeRecord record(uint64_t keyHash, std::vector<uint8_t> fragmentCbufs, uint64_t code = 0)
{
    raw::RuntimeRecord r;
    r.keyString = "samples=1 c0=70 ds=0/0 form " + std::to_string(keyHash);
    for (raw::CapturedProgram &p : r.programs) {
        p.image.assign(80 + 64, 0);
        p.gprs = 8;
        p.cbufTypes = {raw::CbufRoot};
    }
    r.programs[1].cbufTypes = std::move(fragmentCbufs);
    memcpy(r.programs[1].image.data() + 80 + 8, &code, sizeof code);
    describe(r.pipeline, keyHash);
    const raw::CapturedProgram &fs = r.programs[1];
    r.pipeline.memoryEntries = raw::programSetReads(fs.image.data() + 80, fs.image.size() - 80, fs.cbufTypes.data(),
                                                    uint32_t(fs.cbufTypes.size()), fs.boundTextures, r.pipeline);
    r.blob = {0x1a00, 0x1111};
    return r;
}

class DrawnFormUniforms : public harness::DeviceTest {
protected:
    raw::Texture target;
    raw::Buffer buffer;
    raw::CommandBuffer *cb = nullptr;
    raw::RenderEncoder *e = nullptr;

    void SetUp() override
    {
        DeviceTest::SetUp();
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createBuffer(4096, buffer));
        ASSERT_EQ(buffer.va % 256, 0u);
        for (uint32_t i = 0; i < 1024; i++)
            memcpy(buffer.cpu + 4 * i, &i, 4);
    }

    raw::Pipeline *add(const raw::RuntimeRecord &r)
    {
        std::string why;
        raw::Pipeline *pl = dev.addRecord(r, false, why);
        EXPECT_NE(pl, nullptr) << why;
        return pl;
    }

    void begin(raw::Pipeline *pl)
    {
        cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        raw::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        e = cb->render(pass);
        ASSERT_NE(e, nullptr);
        e->setPipeline(pl);
    }

    // A draw of the fragment uniform buffer bound at `offset` into the test's buffer
    void drawAt(uint64_t offset)
    {
        e->setBuffer(kFragment, kSlot, &buffer, offset);
        const char *why = nullptr;
        EXPECT_TRUE(e->draw(kTriangle, 0, 3, 1, 0, &why)) << (why ? why : "");
    }

    void end()
    {
        e->end();
        uint32_t seqno = 0;
        EXPECT_TRUE(cb->commit(seqno));
    }
};

// The storage form reads nothing of the buffer, so it keeps its set; the twin binds the buffer itself
TEST_F(DrawnFormUniforms, ABarrierTwinBindsEachDrawsOwnUniformBuffer)
{
    raw::Pipeline *storage = add(record(0xf0a, {raw::CbufRoot}));
    raw::Pipeline *twin = add(record(0xf0b, {raw::CbufRoot, raw::CbufUbo}));
    ASSERT_TRUE(storage && twin);
    EXPECT_EQ(storage->memoryEntries, 0u);
    storage->storageBits = 1;
    storage->barrierTwin = twin;
    dev.nextForms.barrierFetch = true;
    begin(storage);
    drawAt(0);
    drawAt(256);
    end();
    EXPECT_EQ(fragmentBinds(submitted(), kSlot), (std::vector<uint64_t>{buffer.va, buffer.va + 256}));
}

// A twin whose program reads the buffer's descriptor from set 0 has the entry in its mask, so each draw uploads a set
// that names its own buffer
TEST_F(DrawnFormUniforms, ABarrierTwinReadingTheSetUploadsItForEachDraw)
{
    raw::Pipeline *storage = add(record(0xf1a, {raw::CbufRoot}));
    raw::Pipeline *twin = add(record(0xf1b, {raw::CbufRoot, raw::CbufSet}, kReadSetEntry0));
    ASSERT_TRUE(storage && twin);
    EXPECT_EQ(twin->memoryEntries, 1u);
    storage->storageBits = 1;
    storage->barrierTwin = twin;
    dev.nextForms.barrierFetch = true;
    begin(storage);
    const uint64_t offsets[] = {0, 256};
    for (uint64_t offset : offsets)
        drawAt(offset);
    end();
    const std::vector<Method> m = submitted();
    const std::vector<uint32_t> low = rootWordValues(m, kRootSetWord), high = rootWordValues(m, kRootSetWord + 4);
    ASSERT_EQ(low.size(), 2u);
    ASSERT_EQ(high.size(), 2u);
    EXPECT_NE(low[0], low[1]);
    for (size_t i = 0; i < 2; i++) {
        const uint64_t setVa = uint64_t(high[i]) << 32 | low[i];
        const uint8_t *words = raw::DeviceHarness::upload(dev, setVa, 16);
        ASSERT_NE(words, nullptr);
        uint64_t va = 0;
        memcpy(&va, words, 8);
        EXPECT_EQ(va, buffer.va + offsets[i]) << i;
    }
}

constexpr uint64_t kVariantKey = 0xf2a;
constexpr uint32_t kGenericProgram = 1, kVariantProgram = 2;

// A pack of one state whose fragment program has a variant for a residual-0 uniform buffer (raw_pack.h
// AssumeUniformAligned), the generic program and the variant both reading the fragment buffer at constant buffer 1
std::string variantPack()
{
    constexpr uint32_t kPrograms = 0x400, kPipelines = 0x500, kVariants = 0x800, kStrings = 0x900, kBlob = 0xa00,
                       kHeap = 0x1000, kSlotBytes = 0x100;
    std::vector<uint8_t> file(kHeap + 3 * kSlotBytes, 0);
    raw::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = raw::kPackVersion;
    h.programs = 3;
    h.pipelines = 1;
    raw::PackProgram programs[3] = {};
    for (uint32_t i = 0; i < 3; i++) {
        programs[i].heapOffset = kSlotBytes * i;
        programs[i].program = programs[i].heapOffset + 0x30;
        programs[i].bytes = kSlotBytes;
        programs[i].gprs = 8;
        programs[i].cbufs = i ? 2 : 1;
        programs[i].cbufType[0] = raw::CbufRoot;
        programs[i].cbufType[1] = raw::CbufUbo;
    }
    raw::PackPipeline pp = {};
    describe(pp, kVariantKey);
    pp.keyString = 1;
    pp.vs = 0;
    pp.fs = kGenericProgram;
    pp.blobDwords = 2;
    pp.uniform[0] = {0, 1, 0, 0};
    pp.variantCount[1] = 1;
    const raw::PackVariant variant = {raw::AssumeUniformAligned, kVariantProgram};
    const char strings[] = "\0samples=1 c0=70 ds=0/0 variant";
    const uint32_t blob[] = {0x1a00, 0x1111};
    const auto put = [&](raw::PackSectionId id, uint32_t at, const void *bytes, size_t n) {
        h.section[id] = {at, n};
        memcpy(file.data() + at, bytes, n);
    };
    put(raw::PackPrograms, kPrograms, programs, sizeof programs);
    put(raw::PackPipelines, kPipelines, &pp, sizeof pp);
    put(raw::PackVariants, kVariants, &variant, sizeof variant);
    put(raw::PackStrings, kStrings, strings, sizeof strings);
    put(raw::PackBlob, kBlob, blob, sizeof blob);
    h.section[raw::PackHeap] = {kHeap, 3 * kSlotBytes};
    memcpy(file.data(), &h, sizeof h);
    const char *dir = getenv("TEST_TMPDIR");
    const std::string path = std::string(dir ? dir : "/tmp") + "/variant-pack.bin";
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

class VariantUniforms : public testing::Test {
protected:
    raw::Device dev;
    raw::Texture target;
    raw::Buffer buffer;

    void SetUp() override
    {
        kernel_model::model = {};
        std::string err;
        ASSERT_TRUE(dev.open(variantPack().c_str(), 0, err, raw::Device::Role::Client)) << err;
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createBuffer(4096, buffer));
        ASSERT_EQ(buffer.va % 256, 0u);
    }
};

// An aligned binding draws the variant, an offset one the generic program, an aligned one the variant again: each
// draw binds its buffer's 256-byte base, and the root's residual word (0 until written) moves to its offset past it
TEST_F(VariantUniforms, AResidualZeroVariantSwapBindsEachDrawsOwnUniformBuffer)
{
    raw::Pipeline *pl = dev.pipeline(kVariantKey);
    ASSERT_NE(pl, nullptr);
    raw::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    raw::PassDesc pass;
    pass.color[0].texture = &target;
    pass.count = 1;
    raw::RenderEncoder *e = cb->render(pass);
    ASSERT_NE(e, nullptr);
    e->setPipeline(pl);
    const uint64_t offsets[] = {0, 16, 256};
    for (uint64_t offset : offsets) {
        e->setBuffer(kFragment, kSlot, &buffer, offset);
        const char *why = nullptr;
        EXPECT_TRUE(e->draw(kTriangle, 0, 3, 1, 0, &why)) << (why ? why : "");
    }
    e->end();
    uint32_t seqno = 0;
    EXPECT_TRUE(cb->commit(seqno));
    const std::vector<Method> m = submitted();
    std::vector<uint32_t> programs;
    for (uint32_t v : valuesOf(m, NVB197_SET_PIPELINE_PROGRAM(kFragmentProgram)))
        if (programs.empty() || programs.back() != v)
            programs.push_back(v);
    ASSERT_EQ(programs.size(), 3u);
    EXPECT_NE(programs[0], programs[1]);
    EXPECT_EQ(programs[0], programs[2]);
    EXPECT_EQ(rootWordValues(m, 0x700 + 4 * kResidualWord), (std::vector<uint32_t>{16, 0}));
    EXPECT_EQ(fragmentBinds(m, kSlot), (std::vector<uint64_t>{buffer.va, buffer.va, buffer.va + 256}));
}

}  // namespace
