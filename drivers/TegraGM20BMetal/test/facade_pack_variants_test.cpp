// SPDX-License-Identifier: MIT
// A packed state's variant (pack.h PipelineVariantRecord) past the pack's tier 0 is placed when a draw first selects its
// name, on the placer's thread (TegraGM20BMetalPipelineVariants.mm): its state's creation places none, and the selecting
// thread places nothing. writePack sizes no tier 0, so every record here is past it; device_pack_variants_inline_test has
// the tier-0 case.
#include "facade_harness.h"
#include "../TegraGM20BMetalPipelineVariants.h"
#include "../creation_cost.h"

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <mutex>

namespace {

using namespace harness;
using RenderVariant = gm20b::Variant<gm20b::Pipeline>;

constexpr char kVariant[] = " | tests=2";

// The pack at `path` (writePack's, of one state) with a variant record of its state under the same key, over the same
// programs: the strings section takes the variant's name, the pipelines section moves to the end to hold both records,
// and the state's fast key names both
std::string withVariant(const std::string &path)
{
    std::vector<uint8_t> file;
    {
        std::ifstream in(path, std::ios::binary);
        file.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    gm20b::PackHeader h;
    memcpy(&h, file.data(), sizeof h);
    gm20b::PackSection &strings = h.section[gm20b::PackStrings];
    const uint32_t nameAt = uint32_t(strings.bytes);
    memcpy(file.data() + strings.offset + nameAt, kVariant, sizeof kVariant);
    strings.bytes += sizeof kVariant;
    gm20b::PackPipeline pp[2];
    memcpy(&pp[0], file.data() + h.section[gm20b::PackPipelines].offset, sizeof pp[0]);
    pp[1] = pp[0];
    pp[1].flags |= gm20b::PipelineVariantRecord;
    pp[1].variantName = nameAt;
    file.resize((file.size() + 7) & ~size_t(7));
    h.section[gm20b::PackPipelines] = {file.size(), sizeof pp};
    h.pipelines = 2;
    file.insert(file.end(), reinterpret_cast<const uint8_t *>(pp), reinterpret_cast<const uint8_t *>(pp + 2));
    gm20b::PackFastKey fast;
    memcpy(&fast, file.data() + h.section[gm20b::PackFastKeys].offset, sizeof fast);
    fast.records = 2;
    memcpy(file.data() + h.section[gm20b::PackFastKeys].offset, &fast, sizeof fast);
    memcpy(file.data(), &h, sizeof h);
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char *>(file.data()), std::streamsize(file.size()));
    return path;
}

// Whether `variant` settled within the deadline; the observer runs on the placer's thread
bool settles(const std::shared_ptr<RenderVariant> &variant)
{
    struct Settled {
        std::mutex lock;
        std::condition_variable changed;
        bool done = false;
    };

    const auto settled = std::make_shared<Settled>();
    variant->observe([settled] {
        {
            std::lock_guard<std::mutex> l(settled->lock);
            settled->done = true;
        }
        settled->changed.notify_all();
    });
    std::unique_lock<std::mutex> l(settled->lock);
    return settled->changed.wait_for(l, std::chrono::seconds(10), [&] {
        return settled->done || variant->state() != RenderVariant::State::Pending;
    });
}

TEST(FacadePackVariants, AVariantIsPlacedOnThePlacerWhenADrawFirstSelectsIt)
{
    id<MTLDevice> device = open(withVariant(writePack(packKey(MTLPixelFormatBGRA8Unorm))));
    ASSERT_TRUE(device != nil);
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:nil];
    ASSERT_TRUE(library != nil);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:nil];
    ASSERT_TRUE(state != nil);
    __unsafe_unretained SwitchMetalPipeline *p = (SwitchMetalPipeline *)state;
    ASSERT_TRUE(p->_packed);
    EXPECT_TRUE(p->_hasPackVariants);
    EXPECT_TRUE(p->_variants.empty()) << "the state's creation placed a variant no draw selected";

    const auto variant = std::make_shared<RenderVariant>();
    gm20b::RenderVariant forms;
    forms.fragmentTests = 2;
    ASSERT_EQ(forms.name(), kVariant);
    dispatch_suspend(gm20bVariantPlacer());
    const uint64_t builds = gm20b::tCreationWork.builds;
    gm20bPlaceVariant(p, variant, forms);
    EXPECT_EQ(gm20b::tCreationWork.builds, builds) << "the selecting thread built a pipeline";
    EXPECT_EQ(variant->state(), RenderVariant::State::Pending);
    dispatch_resume(gm20bVariantPlacer());

    ASSERT_TRUE(settles(variant)) << "the placer did not settle the variant";
    ASSERT_EQ(variant->state(), RenderVariant::State::Ready);
    EXPECT_TRUE(variant->value() != nullptr);
    EXPECT_TRUE(variant->value() != p->_raw) << "the variant's record is its own, not the state's";
}

}  // namespace
