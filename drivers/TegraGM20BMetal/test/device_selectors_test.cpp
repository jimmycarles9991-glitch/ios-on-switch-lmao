// SPDX-License-Identifier: MIT
#include "device_harness.h"
#include "../pack_selectors.h"
#include "../mme.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include "drivers/TegraGM20BMetal/precompile/ca-selectors/ca_selector_layout.h"

namespace {

constexpr uint32_t kRGBA8 = 70, kTriangle = 3, kFragment = 2, kFragmentProgram = 5;
constexpr uint64_t kKey = 0x5e1ec7;
constexpr uint32_t kGeneric = 1, kTurned = 2, kSelected = 3, kDerived = 4, kSplit = 5, kPrologue = 6, kPrograms = 7;

std::array<uint8_t, 52> configBytes()
{
    const std::string hex =
        "0000000000000000000000000101ff000000000000000000000000000000ff000001030001000100000000000000000000003c003c";
    std::array<uint8_t, 52> value = {};
    for (size_t i = 0; i < value.size(); i++)
        value[i] = uint8_t(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
    return value;
}

std::vector<uint32_t> fragmentPrograms(const std::vector<uint32_t> &words)
{
    std::vector<uint32_t> out;
    std::vector<switch_gm20b_method> methods(words.size());
    const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
    EXPECT_GE(count, 0);
    for (long i = 0; i < count; i++)
        if (methods[i].subchannel == 0 && methods[i].method == NVB197_SET_PIPELINE_PROGRAM(kFragmentProgram) &&
            (out.empty() || out.back() != methods[i].value))
            out.push_back(methods[i].value);
    return out;
}

std::vector<uint32_t> fragmentPrograms(size_t firstJob = 0)
{
    std::vector<uint32_t> out;
    for (size_t j = firstJob; j < kernel_model::model.jobs.size(); j++)
        for (uint32_t program : fragmentPrograms(kernel_model::model.jobs[j].words))
            if (out.empty() || out.back() != program)
                out.push_back(program);
    return out;
}

std::vector<uint32_t> drawnPrograms(size_t firstJob)
{
    std::vector<uint32_t> out;
    uint32_t program = 0;
    bool known = false;
    const uint32_t first = NVB197_CALL_MME_MACRO(0), last = NVB197_CALL_MME_MACRO(127);
    for (size_t j = 0; j < kernel_model::model.jobs.size(); j++) {
        const auto &words = kernel_model::model.jobs[j].words;
        std::vector<switch_gm20b_method> methods(words.size());
        const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
        EXPECT_GE(count, 0);
        for (long i = 0; i < count; i++) {
            const auto &m = methods[i];
            if (m.subchannel != 0)
                continue;
            if (m.method == NVB197_SET_PIPELINE_PROGRAM(kFragmentProgram))
                program = m.value, known = true;
            const bool macro = m.method >= first && m.method <= last && (m.method - first) % 8 == 0 &&
                               gm20b::mme::countedAsDraw((m.method - first) / 8);
            if (j >= firstJob && (macro || m.method == NVB197_BEGIN)) {
                EXPECT_TRUE(known);
                out.push_back(program);
            }
        }
    }
    return out;
}

std::string selectorPack(bool baseEmpty = false)
{
    constexpr uint32_t kProgramsAt = 0x400, kPipelines = 0x1000, kVariants = 0x3000, kStrings = 0x3100, kBlob = 0x3200,
                       kSelectors = 0x3300, kSplits = 0x3400, kHeap = 0x4000, kSlotBytes = 0x100;
    std::vector<uint8_t> file(kHeap + kPrograms * kSlotBytes, 0);
    gm20b::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = gm20b::kPackVersion;
    h.programs = kPrograms;
    h.pipelines = 3;
    gm20b::PackProgram programs[kPrograms] = {};
    for (uint32_t i = 0; i < kPrograms; i++) {
        programs[i].heapOffset = kSlotBytes * i;
        programs[i].program = programs[i].heapOffset + 0x30;
        programs[i].bytes = kSlotBytes;
        programs[i].gprs = 8;
        programs[i].cbufs = i ? 2 : 1;
        programs[i].cbufType[0] = gm20b::CbufRoot;
        programs[i].cbufType[1] = gm20b::CbufUbo;
    }
    programs[kSplit].cbufs = 3;
    programs[kSplit].cbufType[2] = gm20b::CbufFrontier;
    gm20b::PackPipeline pipelines[3] = {};
    for (uint32_t i = 0; i < 3; i++) {
        gm20b::PackPipeline &pp = pipelines[i];
        pp.keyHash = kKey + i;
        pp.flags = gm20b::PipelinePushMetadata;
        gm20b::setEntryCount(pp, 1);
        pp.entry[0] = {0, gm20b::EntryUbo, kFragment, gm20b::kCaSelectorSlot, uint8_t(5 << 2), 16, 0};
        pp.setBytes = 16;
        pp.pushMask = 1u << 4;
        for (gm20b::PackUniformRange &u : pp.uniform)
            u.entry = gm20b::kNoUniform;
        pp.uniform[0] = {0, 1, 0, 0};
        pp.keyString = 1;
        pp.fs = kGeneric;
        pp.blobDwords = 2;
        pp.variantCount[1] = 1;
    }
    pipelines[0].strideTwin = kKey + 1;
    const gm20b::PackVariant variant = {gm20b::AssumeTurned, kTurned};
    const char strings[] = "\0samples=1 c0=70 ds=0/0 selectors";
    const uint32_t blob[] = {0x1a00, 0x1111};
    const auto put = [&](gm20b::PackSectionId section, uint32_t at, const void *bytes, size_t size) {
        h.section[section] = {at, size};
        memcpy(file.data() + at, bytes, size);
    };
    put(gm20b::PackPrograms, kProgramsAt, programs, sizeof programs);
    put(gm20b::PackPipelines, kPipelines, pipelines, sizeof pipelines);
    put(gm20b::PackVariants, kVariants, &variant, sizeof variant);
    put(gm20b::PackStrings, kStrings, strings, sizeof strings);
    put(gm20b::PackBlob, kBlob, blob, sizeof blob);
    const auto bytes = configBytes();
    const uint32_t id = gm20b::caSelectorId(bytes.data(), bytes.size());
    if (id) {
        const std::vector<uint32_t> selectors =
            baseEmpty ? std::vector<uint32_t>{1, 0, 1, 16, 1, gm20b::AssumeUniformAligned, id, kDerived}
                      : std::vector<uint32_t>{2,  0,         0, 24,
                                              1,  40,        1, gm20b::AssumeUniformAligned,
                                              id, kSelected, 1, gm20b::AssumeUniformAligned,
                                              id, kDerived};
        put(gm20b::PackSelectors, kSelectors, selectors.data(), selectors.size() * sizeof(uint32_t));
    }
    gm20b::PackSplit split = {};
    split.thresholdPixels = 1;
    split.whole = kGeneric;
    split.fs = kSplit;
    split.prologue = kPrologue;
    split.frontierBytes = 64;
    split.frontierCbuf = 2;
    split.uboCount = 1;
    split.uboCbuf[0] = 1;
    split.prologueCbufs = 1;
    split.batchVersion = 1;
    split.batchThreads = 32;
    split.qmd[18] = 32u << 16;
    split.qmd[19] = 0x10001;
    put(gm20b::PackSplits, kSplits, &split, sizeof split);
    h.section[gm20b::PackHeap] = {kHeap, kPrograms * kSlotBytes};
    memcpy(file.data(), &h, sizeof h);
    const char *dir = getenv("TEST_TMPDIR");
    const std::string path = std::string(dir ? dir : "/tmp") + "/selector-pack.bin";
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

class Selectors : public testing::Test {
protected:
    gm20b::Device dev;
    gm20b::Texture target;
    gm20b::Buffer buffer;
    gm20b::Pipeline *state = nullptr;
    std::array<uint8_t, 52> config = configBytes();

    void SetUp() override
    {
        kernel_model::model = {};
        std::string error;
        ASSERT_TRUE(dev.open(selectorPack().c_str(), 0, error, gm20b::Device::Role::Client)) << error;
        ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
        ASSERT_TRUE(dev.createBuffer(4096, buffer));
        state = dev.pipeline(kKey);
        ASSERT_NE(state, nullptr);
    }

    gm20b::RenderEncoder *encoder(gm20b::CommandBuffer &cb)
    {
        gm20b::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        gm20b::RenderEncoder *e = cb.render(pass);
        e->setPipeline(state);
        return e;
    }

    void draw(gm20b::RenderEncoder &e)
    {
        const char *why = nullptr;
        EXPECT_TRUE(e.draw(kTriangle, 0, 3, 1, 0, &why)) << (why ? why : "");
    }

    void submit(gm20b::CommandBuffer &cb, gm20b::RenderEncoder &e)
    {
        e.end();
        uint32_t sequence = 0;
        EXPECT_TRUE(cb.commit(sequence));
    }
};

TEST_F(Selectors, ImmutableBindingsSelectPreparedProgramsAndGpuBuffersRemainDynamic)
{
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    gm20b::RenderEncoder *e = encoder(*cb);
    e->setBytes(2, gm20b::kCaSelectorSlot, config.data(), uint32_t(config.size()));
    draw(*e);
    memcpy(buffer.cpu, config.data(), config.size());
    e->setBuffer(2, gm20b::kCaSelectorSlot, &buffer, 0);
    draw(*e);
    config[48] = 0xff;
    e->setBytes(2, gm20b::kCaSelectorSlot, config.data(), uint32_t(config.size()));
    draw(*e);
    config = configBytes();
    config[49] ^= 0xff;
    e->setBytes(2, gm20b::kCaSelectorSlot, config.data(), uint32_t(config.size()));
    draw(*e);
    submit(*cb, *e);
    const auto programs = fragmentPrograms();
    ASSERT_FALSE(programs.empty());
    if (gm20b::kCaSelectorConfigs) {
        ASSERT_EQ(programs.size(), 3u);
        EXPECT_NE(programs[0], programs[1]);
        EXPECT_EQ(programs[0], programs[2]);
    } else {
        EXPECT_EQ(programs.size(), 1u);
    }
}

// The config is read where the draw reads what its bindings moved, never as it is bound: bytes bound by address and
// written before the draw select the variant of what they hold at the draw
TEST_F(Selectors, TheConfigIsReadAtTheDrawNotAtTheBind)
{
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    gm20b::RenderEncoder *e = encoder(*cb);
    e->setBytes(2, gm20b::kCaSelectorSlot, config.data(), uint32_t(config.size()));
    draw(*e);
    memset(buffer.cpu, 0, config.size());
    e->setBytesAt(2, gm20b::kCaSelectorSlot, buffer.va, uint32_t(config.size()), buffer.cpu, nullptr);
    memcpy(buffer.cpu, config.data(), config.size());
    draw(*e);
    submit(*cb, *e);
    EXPECT_EQ(fragmentPrograms().size(), 1u) << "the second draw selected from the bytes as they were bound";
}

TEST_F(Selectors, UnrelatedUniformBindingsKeepThePreparedSelection)
{
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    gm20b::RenderEncoder *e = encoder(*cb);
    e->setBytes(2, gm20b::kCaSelectorSlot, config.data(), uint32_t(config.size()));
    draw(*e);
    config.fill(0xff);
    e->setBytes(2, gm20b::kCaSelectorSlot + 1, config.data(), uint32_t(config.size()));
    draw(*e);
    submit(*cb, *e);
    EXPECT_EQ(fragmentPrograms().size(), 1u);
}

TEST_F(Selectors, DerivedPipelineAndNonSelectorPipelineSwitchesUseTheirOwnPreparedTable)
{
    ASSERT_NE(state->strideTwin, nullptr);
    for (bool pipelineFirst : {false, true}) {
        SCOPED_TRACE(pipelineFirst);
        const size_t firstJob = kernel_model::model.jobs.size();
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        gm20b::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        gm20b::RenderEncoder *e = cb->render(pass);
        ASSERT_NE(e, nullptr);
        if (pipelineFirst)
            e->setPipeline(state);
        e->setBytes(2, gm20b::kCaSelectorSlot, config.data(), uint32_t(config.size()));
        for (uint32_t i = 0; i < 3; i++) {
            for (gm20b::Pipeline *p : {state, state->strideTwin, dev.pipeline(kKey + 2)}) {
                e->setPipeline(p);
                draw(*e);
            }
        }
        e->setPipeline(state);
        draw(*e);
        submit(*cb, *e);
        const auto programs = drawnPrograms(firstJob);
        ASSERT_EQ(programs.size(), 10u);
        if (gm20b::kCaSelectorConfigs) {
            EXPECT_NE(programs[0], programs[1]);
            EXPECT_NE(programs[1], programs[2]);
            for (size_t i = 3; i < programs.size(); i++)
                EXPECT_EQ(programs[i], programs[i % 3]);
        } else {
            for (uint32_t program : programs)
                EXPECT_EQ(program, programs[0]);
        }
    }
}

TEST_F(Selectors, ADrawnBarrierFormSelectsFromItsOwnPreparedTable)
{
    ASSERT_NE(state->strideTwin, nullptr);
    state->storageBits = 1;
    state->barrierTwin = state->strideTwin;
    dev.nextForms.barrierFetch = true;
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    gm20b::RenderEncoder *e = encoder(*cb);
    e->setBytes(2, gm20b::kCaSelectorSlot, config.data(), uint32_t(config.size()));
    draw(*e);
    e->setPipeline(state->strideTwin);
    draw(*e);
    e->setPipeline(dev.pipeline(kKey + 2));
    draw(*e);
    submit(*cb, *e);
    EXPECT_EQ(fragmentPrograms().size(), gm20b::kCaSelectorConfigs ? 2u : 1u);
}

TEST(SelectorForms, AnEmptyBaseTablePreparesTheBindingsOfItsSpecializedBarrierForm)
{
    kernel_model::model = {};
    gm20b::Device dev;
    std::string error;
    ASSERT_TRUE(dev.open(selectorPack(true).c_str(), 0, error, gm20b::Device::Role::Client)) << error;
    gm20b::Texture target;
    ASSERT_TRUE(dev.createTexture(64, 64, kRGBA8, target));
    gm20b::Pipeline *base = dev.pipeline(kKey), *barrier = dev.pipeline(kKey + 1), *plain = dev.pipeline(kKey + 2);
    ASSERT_TRUE(base && barrier && plain);
    ASSERT_EQ(base->selectorCount, 0u);
    base->strideTwin = nullptr;
    base->storageBits = 1;
    base->barrierTwin = barrier;
    dev.nextForms.barrierFetch = true;
    const auto config = configBytes();
    for (bool pipelineFirst : {false, true}) {
        SCOPED_TRACE(pipelineFirst);
        const size_t firstJob = kernel_model::model.jobs.size();
        gm20b::CommandBuffer *cb = dev.begin();
        ASSERT_NE(cb, nullptr);
        gm20b::PassDesc pass;
        pass.color[0].texture = &target;
        pass.count = 1;
        gm20b::RenderEncoder *e = cb->render(pass);
        ASSERT_NE(e, nullptr);
        e->setPipeline(pipelineFirst ? base : plain);
        e->setBytes(2, gm20b::kCaSelectorSlot, config.data(), uint32_t(config.size()));
        const char *why = nullptr;
        for (uint32_t i = 0; i < 3; i++) {
            e->setPipeline(plain);
            e->setPipeline(base);
            ASSERT_TRUE(e->draw(kTriangle, 0, 3, 1, 0, &why)) << (why ? why : "");
        }
        e->end();
        uint32_t sequence = 0;
        ASSERT_TRUE(cb->commit(sequence));
        const auto programs = drawnPrograms(firstJob);
        ASSERT_EQ(programs.size(), 3u);
        if (gm20b::kCaSelectorConfigs) {
            gm20b::Pipeline *selected =
                dev.variant(*barrier, gm20b::AssumeUniformAligned, gm20b::caSelectorId(config.data(), config.size()));
            ASSERT_TRUE(selected->selectorVariant);
            const auto expected = fragmentPrograms(selected->methods);
            ASSERT_EQ(expected.size(), 1u);
            for (uint32_t program : programs)
                EXPECT_EQ(program, expected[0]);
        } else {
            for (uint32_t program : programs)
                EXPECT_EQ(program, programs[0]);
        }
    }
}

TEST_F(Selectors, KnownConfigUsesItsWholeSpecializationAndUnknownConfigKeepsTheGenericSplit)
{
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    cb->forceSplit(true);
    gm20b::RenderEncoder *e = encoder(*cb);
    e->setBytes(2, gm20b::kCaSelectorSlot, config.data(), uint32_t(config.size()));
    draw(*e);
    EXPECT_EQ(cb->splitRecords().size(), gm20b::kCaSelectorConfigs ? 0u : 1u);
    config[48] = 0xff;
    e->setBytes(2, gm20b::kCaSelectorSlot, config.data(), uint32_t(config.size()));
    draw(*e);
    EXPECT_EQ(cb->splitRecords().size(), gm20b::kCaSelectorConfigs ? 1u : 2u);
    submit(*cb, *e);
}

TEST_F(Selectors, ASelectorDoesNotReplaceTheRequiredTurnedFragmentProgram)
{
    target.rotation = 1;
    gm20b::CommandBuffer *cb = dev.begin();
    ASSERT_NE(cb, nullptr);
    gm20b::RenderEncoder *e = encoder(*cb);
    e->setBytes(2, gm20b::kCaSelectorSlot, config.data(), uint32_t(config.size()));
    draw(*e);
    submit(*cb, *e);
    const auto programs = fragmentPrograms();
    ASSERT_EQ(programs.size(), 1u);
    gm20b::Pipeline *turned = dev.variant(*state, gm20b::AssumeTurned);
    EXPECT_NE(turned, state);
    EXPECT_FALSE(turned->selectorVariant);
    EXPECT_EQ(programs, fragmentPrograms(turned->methods));
}

}  // namespace
