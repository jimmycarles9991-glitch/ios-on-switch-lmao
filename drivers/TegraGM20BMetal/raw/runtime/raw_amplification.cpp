// SPDX-License-Identifier: MIT
// A render encoder's vertex amplification and bind strides (raw_gpu.h RenderEncoder). A state of vertex amplification is
// m2v's variant of one draw per amplification (bind/graphics.rs amplification_draws): each draw is drawn once per
// amplification, in order, its id in the state's AmplificationId push word, as Metal renders every primitive of one
// amplification before the next's. The GPU repeats it: one call of an amplify macro (raw_mme.h) per Metal draw, which
// writes each amplification's words from the scratch words the encoder keeps (the count where it changes, the state's
// words where the bound state's differ) and draws, a pixel barrier between two amplifications of a state that fetches its
// own colour through the texture unit. That state's draw of more than one primitive is drawn a run of primitives at a
// time with barriers between: it is drawn once per amplification. Its view mappings are the state's words too: the
// direct compile makes an amplifying state as for a layered pass with viewport offsets, so its vertex program writes
// Layer and ViewportIndex from the amplification's AmplificationLayerOffset and AmplificationViewportOffset words below
// the pass's length and the encoder's count (Pipeline::amplificationLayerWord, amplificationViewportWord), as Metal adds a
// mapping's offsets. A state of a dynamic stride reads its bind's stride: its vertex stream's at each draw, a pulled
// layout's from its VertexBufferStride word. A bind stride past the stream's field draws the state's stride twin, which
// pulls those layouts in the shader.
#include <algorithm>
#include "raw_gpu.h"

namespace raw {
namespace {

constexpr uint32_t kRootPushBytes = 0x700;  // the root table's push words, m2v's push ranges
constexpr uint32_t kAmplificationWords = mme::kAmplificationScratchWords + mme::kAmplifyCallWords;

}  // namespace

void RenderEncoder::setVertexStride(uint32_t slot, uint32_t stride)
{
    if (slot < 31)
        vertexStrides_[slot] = stride;
}

void RenderEncoder::setAmplification(uint32_t count, const uint32_t (&viewports)[kAmplificationMaps],
                                     const uint32_t (&layers)[kAmplificationMaps])
{
    const bool same = std::max(1u, count) == amplifications_ &&
                      std::equal(std::begin(viewports), std::end(viewports), amplificationViewports_) &&
                      std::equal(std::begin(layers), std::end(layers), amplificationLayers_);
    amplificationScratchDirty_ |= !same;
    amplifications_ = std::max(1u, count);
    std::copy(std::begin(viewports), std::end(viewports), amplificationViewports_);
    std::copy(std::begin(layers), std::end(layers), amplificationLayers_);
    amplificationDirty_ = true;
    amplificationMapsWide_ = false;
    for (uint32_t a = 0; a < std::min(amplifications_, kAmplificationMaps); a++)
        amplificationMapsWide_ |= ((layers[a] | viewports[a]) >> SWITCH_GM20B_AMP_MAP_VIEWPORT_SHIFT) != 0;
}

const char *RenderEncoder::drawnWordsRefusal(const PackPipeline &r) const
{
    const uint32_t most = std::max<uint32_t>(1, r.amplificationMax);
    if (amplifications_ > most)
        return "a vertex amplification count above the state's maxVertexAmplificationCount";
    if (amplificationMapsWide_)
        return "a vertex amplification view mapping's offset past the 16 bits of its scratch word";
    for (uint32_t k = 1; k < kPackVertexStreams && r.vertexStreams[k - 1]; k++)
        if (streamSlot(r.vertexStreams[k - 1]) >= 31)
            return "a vertex stream of no Metal buffer";
    if (bindStridePastStream(r))
        return "a vertex buffer bind stride past the vertex stream's 12-bit stride, of a state with no stride twin";
    return nullptr;
}

bool RenderEncoder::bindStridePastStream(const PackPipeline &r) const
{
    if (r.vertexStrideDynamic && vertexStride(r.vertexSlot) > kStreamStrideField)
        return true;
    for (uint32_t k = 1; k < kPackVertexStreams && r.vertexStreams[k - 1]; k++) {
        const uint8_t stream = r.vertexStreams[k - 1];
        if ((stream & kStreamBindStride) && vertexStride(streamSlot(stream)) > kStreamStrideField)
            return true;
    }
    return false;
}

void RenderEncoder::icbAmplification(uint32_t a)
{
    const Pipeline &pl = *prepared_;
    amplificationDirty_ = true;
    const uint32_t m = a < kAmplificationMaps ? a : 0;
    const std::pair<uint8_t, uint32_t> words[] = {{pl.rec->amplificationIdWord, a},
                                                  {pl.amplificationLayerWord, amplificationLayers_[m]},
                                                  {pl.amplificationViewportWord, amplificationViewports_[m]}};
    for (const auto &[word, value] : words) {
        if (!word || word > 32 || cb_->rootPush_[word - 1] == value)
            continue;
        if (p_.room() < 8)
            reserve();
        p_.m(NVB197_LOAD_CONSTANT_BUFFER_OFFSET, kRootPushBytes + 4 * (word - 1u));
        p_.nonInc(NVB197_LOAD_CONSTANT_BUFFER(0), &value, 1);
        cb_->rootPush_[word - 1] = value;
    }
}

bool RenderEncoder::runsPerAmplification(uint32_t primitive, uint32_t count, uint32_t instances, bool indexed)
{
    const Pipeline *pl = pipeline_ ? formFor(pipeline_) : nullptr;
    return pl && fetchesOwnColor(pl->fetchBits, pl->writeBits, pl->storageBits) &&
           !singlePrimitive({primitive, count, instances, false, indexed});
}

void RenderEncoder::amplificationScratch(Push &p, const Pipeline &pl)
{
    if (amplificationScratchDirty_) {
        mme::amplification(p, amplifications_, amplificationLayers_, amplificationViewports_);
        amplificationScratchDirty_ = false;
    }
    const PackPipeline &r = *pl.rec;
    const auto byte = [](uint8_t word) {
        return word ? kRootPushBytes + 4 * (word - 1u) : 0;
    };
    mme::AmplificationWords words;
    words.id = byte(r.amplificationIdWord);
    words.count = byte(r.amplificationCountWord);
    words.viewport = byte(pl.amplificationViewportWord);
    words.layer = pl.amplificationLayerWord ? byte(pl.amplificationLayerWord) : SWITCH_GM20B_AMP_LAYER_FIRST;
    words.barrier = fetchesOwnColor(pl.fetchBits, pl.writeBits, pl.storageBits);
    if (!words.same(heldAmplification_)) {
        mme::amplificationWords(p, words);
        heldAmplification_ = words;
    }
}

void RenderEncoder::amplifyCall(const Pipeline &pl, bool indexed, uint32_t first, uint32_t count, uint32_t instances,
                                uint32_t root, uint32_t baseInstance)
{
    if (p_.room() < kAmplificationWords)
        reserve();
    amplificationScratch(p_, pl);
    mme::amplify(p_, params_, indexed, first, count, instances, root, baseInstance);
    heldAmplifyWords(pl);
}

void RenderEncoder::heldAmplifyWords(const Pipeline &pl)
{
    const PackPipeline &r = *pl.rec;
    const uint32_t last = std::min(amplifications_, kAmplificationMaps) - 1;
    const std::pair<uint8_t, uint32_t> words[] = {{r.amplificationIdWord, amplifications_ - 1},
                                                  {r.amplificationCountWord, amplifications_},
                                                  {pl.amplificationLayerWord, amplificationLayers_[last]},
                                                  {pl.amplificationViewportWord, amplificationViewports_[last]}};
    for (const auto &[word, value] : words)
        if (word && word <= 32)
            cb_->rootPush_[word - 1] = value;
}

void RenderEncoder::drawnWords(const Pipeline &pl, uint32_t (&want)[32])
{
    const PackPipeline &r = *pl.rec;
    // an amplify call and an indirect command buffer's execute write each amplification's own words themselves
    const uint32_t m = amplification_ < kAmplificationMaps ? amplification_ : 0;
    const std::pair<uint8_t, uint32_t> amplified[] = {{r.amplificationIdWord, amplification_},
                                                      {pl.amplificationLayerWord, amplificationLayers_[m]},
                                                      {pl.amplificationViewportWord, amplificationViewports_[m]}};
    for (const auto &[word, value] : amplified)
        if (word && word <= 32)
            want[word - 1] = amplifies() ? cb_->rootPush_[word - 1] : value;
    amplificationDirty_ = false;
    if (r.amplificationCountWord && r.amplificationCountWord <= 32)
        want[r.amplificationCountWord - 1] = amplifications_;
    for (const auto &pulled : r.pulledStride)
        if (pulled[1] && pulled[1] <= 32)
            want[pulled[1] - 1] = vertexStride(pulled[0]);
    if (r.arrayLengthWord && r.arrayLengthWord <= 32)
        want[r.arrayLengthWord - 1] = pass_.layers;
    if (r.viewportCountWord && r.viewportCountWord <= 32)
        want[r.viewportCountWord - 1] = views_.viewports;
}

}  // namespace raw
