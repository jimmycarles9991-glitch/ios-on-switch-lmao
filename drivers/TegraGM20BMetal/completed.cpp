// SPDX-License-Identifier: MIT
// Device::addRecord (gpu.h): a state the runtime made a record of (record.h), drawn from then on as the pack's states are.
// Device::ownPipeline: one of the driver's own states (own_states.h) by its name.
#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>
#include <os/log.h>
#include "gpu.h"
#include "dynamic_push.h"
#include "guest_bases.h"
#include "record.h"
#include "slm.h"
#include "storage.h"

namespace gm20b {

// lock_ held: a program's slot laid out as the pack lays one (programSlot), once per program and constant buffer layout (one code
// can read its uniform buffers at another state's set-0 offsets), placed as the pack's programs are (heapPlace). The next
// submission's prologue invalidates the shader and constant caches before any draw reads it.
bool Device::placeCompleted(const CapturedProgram &p, PlacedProgram &out)
{
    const std::string key = programSlotKey(p);
    if (const auto it = completedPrograms_.find(key); it != completedPrograms_.end()) {
        out = it->second.placed;
        return true;
    }
    return p.image.size() >= sizeof CompletedProgram::header && placeCompletedSlot(completedSlot(p), out);
}

CompletedSlot Device::completedSlot(const CapturedProgram &p)
{
    CompletedSlot slot;
    slot.key = programSlotKey(p);
    slot.bytes = programSlot(p, slot.info);
    slot.farUbos = p.farUbos;
    memcpy(slot.header, p.image.data(), std::min(sizeof slot.header, p.image.size()));
    return slot;
}

// lock_ held
bool Device::placeCompletedSlot(const CompletedSlot &slot, PlacedProgram &out)
{
    if (const auto it = completedPrograms_.find(slot.key); it != completedPrograms_.end()) {
        out = it->second.placed;
        return true;
    }
    uint64_t at = 0;
    if (!slmNeed(slmPerWarpOfHeader(slot.header)) || !heapPlace(slot.bytes.data(), uint32_t(slot.bytes.size()), at))
        return false;
    CompletedProgram &placed = completedPrograms_[slot.key];
    placed.info = slot.info;
    placed.info.heapOffset = uint32_t(at);
    memcpy(placed.header, slot.header, sizeof placed.header);
    placed.farUbos = slot.farUbos;
    placed.placed = {&placed.info, uint32_t(at) + slot.info.program,
                     slot.info.dataBytes ? heap_.va + at + slot.info.dataOffset : 0, placed.header,
                     placed.farUbos.empty() ? nullptr : placed.farUbos.data()};
    SWITCH_METAL_DIAGNOSTIC(stats.programsPlaced++);
    out = placed.placed;
    return true;
}

bool Device::constexprSampler(const std::array<uint32_t, 8> &words, uint32_t &index)
{
    const uint32_t *pack = pack_.section<uint32_t>(PackSamplers);
    for (uint32_t i = 0; i < pack_.header()->samplers; i++)
        if (!memcmp(pack + 8 * i, words.data(), sizeof words)) {
            index = i;
            return true;
        }
    auto known = completedSamplers_.find(words);
    if (known == completedSamplers_.end()) {
        uint32_t tsc = 0;
        if (constexprCount_ == kConstexprSamplers || !takeTsc(tsc))
            return false;
        memcpy(tsc_.cpu + 32 * tsc, words.data(), 32);
        noteTscWrite();
        constexprTsc_[constexprCount_] = tsc;
        constexprWord0_[constexprCount_] = words[0];
        known = completedSamplers_.emplace(words, uint8_t(constexprCount_++)).first;
    }
    index = known->second;
    return true;
}

// lock_ held: a runtime record's split offered to its state (RuntimeRecord::split; the prologue's image an 80-byte header of
// zeros before its code), its programs laid out as heap slots for the draw that first takes it
void Device::offerCompletedSplit(Completed &c, const RuntimeRecord &r, const PlacedProgram &vs, bool caTransform)
{
    const CapturedProgram &prologue = r.splitPrograms[1];
    if (prologue.image.size() <= 80 || r.splitPrograms[0].image.size() < sizeof CompletedProgram::header)
        return;
    SplitSource source;
    source.rec = r.splitRecord;
    source.fsSlot = programSlot(r.splitPrograms[0], source.fsInfo);
    source.prologueSlot.assign(prologue.image.begin() + 80, prologue.image.end());
    source.prologueSlot.resize((source.prologueSlot.size() + 0xFF) & ~size_t(0xFF));
    source.prologueDataAt = uint32_t(source.prologueSlot.size());
    source.prologueDataBytes = uint32_t(prologue.data.size());
    source.prologueSlot.insert(source.prologueSlot.end(), prologue.data.begin(), prologue.data.end());
    source.pp = &c.record;
    source.blob = c.blob.data();
    source.far = c.far.empty() ? nullptr : c.far.data();
    source.keyString = c.keyString.c_str();
    source.vs = vs;
    source.caTransform = caTransform;
    offerSplit(c.pipeline, std::move(source));
}

Pipeline *Device::addRecord(const RuntimeRecord &r, bool caTransform, std::string &why, bool *retry)
{
    ensurePackWindow();
    if (retry)
        *retry = false;
    const bool tess = tessellates(r.pipeline), pixel = pixelStage(r.pipeline);
    for (const CapturedProgram *program : {&r.programs[0], &r.programs[1], &r.tessPrograms[0], &r.tessPrograms[1]}) {
        if ((!tess && (program == &r.tessPrograms[0] || program == &r.tessPrograms[1])) ||
            (!pixel && program == &r.programs[1]))
            continue;
        if (program->image.size() < sizeof CompletedProgram::header) {
            why = "missing shader program header";
            return nullptr;
        }
    }
    if (!usableDynamicPush(r.pipeline, r.keyString.c_str())) {
        why = "missing or invalid dynamic blend push metadata";
        return nullptr;
    }
    if (!usableGuestBases(r.pipeline)) {
        why = "its buffers' Metal address words are not this runtime's";
        return nullptr;
    }
    Pipeline *strideTwin = nullptr;
    if (r.strideTwin && !(strideTwin = addRecord(*r.strideTwin, caTransform, why, retry))) {
        why = "its stride twin: " + why;
        return nullptr;
    }
    std::lock_guard<std::mutex> l(lock_);
    const uint64_t key = r.pipeline.keyHash;
    if (const auto it = completed_.find(key); it != completed_.end())
        return &it->second->pipeline;
    PlacedProgram vs, fs = noPixelProgram(), tessPlaced[2];
    if (!placeCompleted(r.programs[0], vs) || (pixel && !placeCompleted(r.programs[1], fs)) ||
        (tess &&
         (!placeCompleted(r.tessPrograms[0], tessPlaced[0]) || !placeCompleted(r.tessPrograms[1], tessPlaced[1])))) {
        stats.heapFull++;
        why = "the shader heap is full";
        if (retry)
            *retry = true;
        return nullptr;
    }
    Completed c;
    c.record = r.pipeline;
    c.keyString = r.keyString;
    c.blob = r.blob;
    c.spill = r.spill;
    c.far = r.far;
    c.vs = vs;
    c.fs = fs;
    c.layout = layoutOf(r);
    if (c.far.size() != farWords(entryCount(c.record))) {
        why = "a record whose far entries are not its entries'";
        return nullptr;
    }
    for (const auto &[m, v] : r.extras) {
        c.blob.push_back(m);
        c.blob.push_back(v);
    }
    c.record.blobDwords = uint32_t(c.blob.size());
    if (!usableStorageMetadata(c.record, c.blob.data(), c.record.blobDwords, c.far.empty() ? nullptr : c.far.data())) {
        why = "missing or invalid ordered storage attachment metadata";
        return nullptr;
    }
    bool unknownSampler = false;
    if (!placeConstexprSamplers(
            c.record, c.far, r.samplers,
            [&](const std::array<uint32_t, 8> &words, uint32_t &index) {
                return constexprSampler(words, index);
            },
            unknownSampler)) {
        why = unknownSampler ? "a constexpr sampler the record holds no header of" : "no room for its sampler headers";
        if (retry)
            *retry = !unknownSampler;
        return nullptr;
    }
    Completed &stored = completedStore_.emplace_back(std::move(c));
    build(stored.pipeline, stored.record, vs, fs, stored.blob.data(), stored.keyString.c_str(), caTransform,
          stored.spill.size() == stored.record.spillWords[0] + stored.record.spillWords[1] ? stored.spill.data()
                                                                                           : nullptr,
          stored.far.empty() ? nullptr : stored.far.data(), tess ? tessPlaced : nullptr);
    std::vector<uint32_t> pairs;
    for (const auto &[m, v] : r.restore)
        pairs.insert(pairs.end(), {m, v});
    const std::vector<uint32_t> restored = restoreWords(pairs.data(), uint32_t(pairs.size()));
    stored.pipeline.restore.insert(stored.pipeline.restore.end(), restored.begin(), restored.end());
    stored.pipeline.strideTwin = strideTwin;
    if (r.split && !stored.record.storageMask && pixel && !tess)
        offerCompletedSplit(stored, r, vs, caTransform);
    for (uint32_t i = r.pipeline.variantCount[0]; i < r.variants.size(); i++) {
        if (!(r.variants[i].assume & AssumeTurned))
            continue;
        if (r.variants[i].program.image.size() < sizeof CompletedProgram::header) {
            stored.pipeline.turnRefused = true;
            break;
        }
        TurnedSource &t = stored.turnedSource;
        t.fs = completedSlot(r.variants[i].program);
        t.pp = &stored.record;
        t.blob = stored.blob.data();
        t.far = stored.far.empty() ? nullptr : stored.far.data();
        t.spills = stored.spill.size() == stored.record.spillWords[0] + stored.record.spillWords[1]
                       ? stored.spill.data()
                       : nullptr;
        t.keyString = stored.keyString.c_str();
        t.vs = vs;
        t.tess[0] = tessPlaced[0];
        t.tess[1] = tessPlaced[1];
        t.tessellates = tess;
        t.caTransform = caTransform;
        t.turned = &stored.turned;
        stored.pipeline.turnedSource = &t;
        stored.pipeline.turnedVariant = true;
        break;
    }
    stored.pipeline.own = isOwnState(key);
    offerCompletedVariants(stored, r);
    completed_[key] = &stored;
    return &stored.pipeline;
}

bool Device::attachFullForm(const RuntimeRecord &full, std::string &why)
{
    ensurePackWindow();
    const RuntimeRecord *forms[2] = {&full, full.strideTwin.get()};
    const RecordLayout layouts[2] = {layoutOf(full), forms[1] ? layoutOf(*forms[1]) : RecordLayout()};
    std::lock_guard<gm20b::Mutex> l(lock_);
    Completed *placed[2] = {};
    for (uint32_t k = 0; k < 2 && forms[k]; k++) {
        const auto it = completed_.find(forms[k]->pipeline.keyHash);
        if (it == completed_.end() || (k && &it->second->pipeline != placed[0]->pipeline.strideTwin)) {
            why = k ? "no stride twin of its key is placed" : "no state of its key is placed";
            return false;
        }
        if (const char *part = layoutDifference(it->second->layout, layouts[k])) {
            why = std::string(k ? "its stride twin is" : "it is") + " of another " + part + " than its placed record";
            return false;
        }
        placed[k] = it->second;
    }
    if (placed[0]->pipeline.strideTwin && !forms[1]) {
        why = "no stride twin in its full form";
        return false;
    }
    for (uint32_t k = 0; k < 2 && placed[k]; k++) {
        if (forms[k]->split && !placed[k]->record.storageMask && pixelStage(placed[k]->record) &&
            !tessellates(placed[k]->record))
            offerCompletedSplit(*placed[k], *forms[k], placed[k]->vs, placed[k]->pipeline.caTransform);
        offerCompletedVariants(*placed[k], *forms[k]);
    }
    return true;
}

Pipeline *Device::makeTurned(Pipeline &pl)
{
    SWITCH_METAL_ENCODE_LOCK(l, lock_);
    if (Pipeline *made = pl.turnedTwin)
        return made;
    const TurnedSource *t = pl.turnedSource;
    if (!t)
        return nullptr;
    PlacedProgram fs;
    if (!placeCompletedSlot(t->fs, fs)) {
        stats.heapFull++;
        os_log_error(gm20b::log(),
                     "SwitchMetal: a turned variant found no room in the shader heap or local memory: this turned draw "
                     "is refused");
        return nullptr;
    }
    Pipeline &turned = *t->turned;
    build(turned, *t->pp, t->vs, fs, t->blob, t->keyString, t->caTransform, t->spills, t->far,
          t->tessellates ? t->tess : nullptr);
    turned.restore = pl.restore;
    turned.strideTwin = pl.strideTwin;
    __atomic_store_n(&pl.turnedTwin, &turned, __ATOMIC_RELEASE);
    return &turned;
}

namespace {

// Pack::fnv of `text` continued from `h`
constexpr uint64_t fnvMore(uint64_t h, std::string_view text)
{
    for (const char c : text)
        h = (h ^ uint8_t(c)) * 0x100000001b3ull;
    return h;
}

// The key a pack holds own state `pair` under (packKeyHash): its name, raw.<its vertex function's name less its last
// word>.<its fragment's>
constexpr uint64_t ownStateKey(std::string_view pair)
{
    const size_t fragmentAt = pair.find(" | f ");
    const uint64_t prefix = fnvMore(0xcbf29ce484222325ull, "raw.");
    const uint64_t family = fnvMore(prefix, pair.substr(0, pair.rfind('_', fragmentAt)));
    return fnvMore(fnvMore(family, "."), pair.substr(fragmentAt + 5));
}

constexpr std::array<uint64_t, std::size(kOwnStates)> ownStateKeys()
{
    std::array<uint64_t, std::size(kOwnStates)> keys{};
    for (size_t i = 0; i < keys.size(); i++) {
        const uint64_t key = ownStateKey(kOwnStates[i]);
        size_t at = i;
        for (; at && keys[at - 1] > key; at--)
            keys[at] = keys[at - 1];
        keys[at] = key;
    }
    return keys;
}

// ascending, as own_ holds their states
constexpr std::array<uint64_t, std::size(kOwnStates)> kOwnStateKeys = ownStateKeys();

}  // namespace

bool isOwnState(uint64_t keyHash)
{
    return std::binary_search(kOwnStateKeys.begin(), kOwnStateKeys.end(), keyHash);
}

Pipeline *Device::ownPipeline(const char *name)
{
    ensurePackWindow();
    if (!name)
        return nullptr;
    const uint64_t key = Pack::fnv(name, strlen(name));
    const auto at = std::lower_bound(kOwnStateKeys.begin(), kOwnStateKeys.end(), key);
    if (at == kOwnStateKeys.end() || *at != key)
        return nullptr;
    std::atomic<Pipeline *> &own = own_[size_t(at - kOwnStateKeys.begin())];
    Pipeline *pl = own.load(std::memory_order_acquire);
    if (!pl && (pl = pipeline(key)))
        own.store(pl, std::memory_order_release);
    return pl;
}

}  // namespace gm20b
