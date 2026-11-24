// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalPassCache.h"
#import "TegraGM20BMetalStatesCost.h"
#include "ca_catch_all.h"
#include "object_pool.h"
#include "resolve_rule.h"

#if SWITCH_METAL_DIAGNOSTICS
// ---- the rates line ----
static struct {
    uint64_t raw, encodeUs, draws, passes, copies, deferred, ringWaits, lastUs, poolStalls, eventsReleased, heldReturns,
        signalsPublished, signalsAfterWork, waitsOnPromise, scheduledWaits, scheduledWaitUs, completedWaits,
        completedWaitUs, rtUnaligned, rtMixed, levelPasses;
} gRates;

#endif

bool metal_texture_kept(id<MTLTexture> texture)
{
    return [(id)texture isKindOfClass:[SwitchMetalTexture class]] && ((SwitchMetalTexture *)texture)->_gm20bOk;
}

bool metal_chains_on(void)
{
    return gRM != nullptr;
}

const void *metal_texture_image(id<MTLTexture> texture, size_t *pitch)
{
    if (![(id)texture isKindOfClass:[SwitchMetalTexture class]])
        return nullptr;
    SwitchMetalTexture *t = (SwitchMetalTexture *)texture;
    if (!t->_gm20bOk || t->_raw.blockLinear || !t->_raw.cpu)
        return nullptr;
    if (pitch)
        *pitch = t->_raw.pitch;
    return t->_raw.cpu;
}

size_t metal_rates(char *out, size_t size)
{
#if SWITCH_METAL_DIAGNOSTICS
    if (!gRM || !size)
        return 0;
    Stats &s = gRM->stats;
    const uint64_t raw = s.raw.load(), now = nowUs();
    const uint64_t c = raw - gRates.raw;
    if (!c)
        return 0;
    const double secs = gRates.lastUs ? (now - gRates.lastUs) / 1e6 : 5.0;
    gm20b::Device::Stats &k = gRM->raw->stats;
    const double per = double(c);
    const gm20b::Compiler::Stats made = gRM->completion ? gRM->completion->stats() : gm20b::Compiler::Stats();
    char diag[1024];
    snprintf(
        diag, sizeof diag,
        " rt128=%llu(pitch %u va&255 %u) rtmix=%llu lvl=%llu states=%llu/%llu/%llu/%llu/%.0fms "
        "atcreate=%llu/%.0fus/s%llu/o%llu swait=%.0fms comp=%llu/%lluKB/v%llu/m%llu/f%llu/x%llu bk=%llu/%lluMB/r%llu "
        "tfree=%lluMB zbc=%llu/%llu "
        "split=%llu rsv=t%llu/%llu,x%llu/%llu,p%llu/%llu/%llu",
        (unsigned long long)(k.rtUnaligned.load() - gRates.rtUnaligned), k.rtUnalignedPitch.load(),
        k.rtUnalignedVa.load(), (unsigned long long)(k.rtMixed.load() - gRates.rtMixed),
        (unsigned long long)(k.levelPasses.load() - gRates.levelPasses), (unsigned long long)made.hits,
        (unsigned long long)made.misses, (unsigned long long)made.refusals, (unsigned long long)made.failures,
        made.makeUs / 1000.0, (unsigned long long)made.lookupHits,
        made.lookupHits ? double(made.lookupUs) / double(made.lookupHits) : 0.0, (unsigned long long)made.seedHits,
        (unsigned long long)made.ownHits, made.waitUs / 1000.0, (unsigned long long)k.compHeld.load(),
        (unsigned long long)(k.compHeldBytes.load() >> 10), (unsigned long long)k.compViews.load(),
        (unsigned long long)k.compMade.load(), (unsigned long long)k.compFailed.load(),
        (unsigned long long)k.compMisses.load(), (unsigned long long)k.backings.load(),
        (unsigned long long)(k.backingBytes.load() >> 20), (unsigned long long)k.retiredBackings.load(),
        (unsigned long long)(k.tiledFreeBytes.load() >> 20), (unsigned long long)k.zbcHits.load(),
        (unsigned long long)k.zbcMisses.load(), (unsigned long long)k.splitPrologues.exchange(0),
        (unsigned long long)k.resolveRoute[gm20b::ResolveByTwod][0].exchange(0),
        (unsigned long long)k.resolveRoute[gm20b::ResolveByTwod][1].exchange(0),
        (unsigned long long)k.resolveRoute[gm20b::ResolveByComplement][0].exchange(0),
        (unsigned long long)k.resolveRoute[gm20b::ResolveByComplement][1].exchange(0),
        (unsigned long long)k.resolveRoute[gm20b::ResolveByPass][0].exchange(0),
        (unsigned long long)k.resolveRoute[gm20b::ResolveByPass][1].exchange(0),
        (unsigned long long)k.resolveRoute[gm20b::ResolveByPass][2].exchange(0));
    {
        // render state creations the pack held and every other, since the device opened (TegraGM20BMetalStatesCost.mm)
        uint64_t packHits = 0, packMisses = 0;
        gm20bPackCreations(packHits, packMisses);
        const size_t used = strlen(diag);
        snprintf(diag + used, sizeof diag - used, " pack=%llu/%llu", (unsigned long long)packHits,
                 (unsigned long long)packMisses);
    }
    {
        const size_t used = strlen(diag);
        snprintf(diag + used, sizeof diag - used, " cag=%llu rtsync=%llu/%lluus",
                 (unsigned long long)gm20b::gCaCatchAllDraws.load(std::memory_order_relaxed),
                 (unsigned long long)gm20b::gRenderSyncCreations.load(std::memory_order_relaxed),
                 (unsigned long long)gm20b::gRenderSyncMaxUs.load(std::memory_order_relaxed));
    }
    {  // since open: heaps' arenas made and restored, untouched host pages imported (zero-filled on the importer)
        const size_t used = strlen(diag);
        snprintf(diag + used, sizeof diag - used,
                 " heaps=%llu/%llu hz=%lluKB park=c%llu/%lluKB(%llu/%llu/%llu),h%llu/%lluKB(%llu/%llu/%llu)",
                 (unsigned long long)k.arenasMade.load(), (unsigned long long)k.arenaRestores.load(),
                 (unsigned long long)(k.hostZeroedBytes.load() >> 10), (unsigned long long)k.parkedChunks.load(),
                 (unsigned long long)(k.parkedChunkBytes.load() >> 10), (unsigned long long)k.chunkParks.load(),
                 (unsigned long long)k.chunkUnparksResident.load(), (unsigned long long)k.chunkUnparksReclaimed.load(),
                 (unsigned long long)k.parkedHeaps.load(), (unsigned long long)(k.parkedHeapBytes.load() >> 10),
                 (unsigned long long)k.heapParks.load(), (unsigned long long)k.heapUnparksResident.load(),
                 (unsigned long long)k.heapUnparksReclaimed.load());
    }
#if SWITCH_FRAME_TIMELINE
    {
        const size_t used = strlen(diag);
        snprintf(diag + used, sizeof diag - used, " sbf=%llu fb=%llu/%llu/%llu/%llu(%s) clr=%llu/%llu zero=%llu",
                 (unsigned long long)s.scheduledByFinish.exchange(0), (unsigned long long)k.fetchBarriers.exchange(0),
                 (unsigned long long)k.fetchBarriersElided.exchange(0),
                 (unsigned long long)k.fetchBarriersAnyColor.exchange(0),
                 (unsigned long long)k.fetchBarriersInterlocked.exchange(0),
                 gm20b::Device::fetchBarrierName(gRM->raw->fetchBarrier.load(std::memory_order_relaxed)),
                 (unsigned long long)k.clears.exchange(0), (unsigned long long)k.clearsElided.exchange(0),
                 (unsigned long long)s.zeroAnswers.exchange(0));
    }
#endif
    gRates.rtUnaligned = k.rtUnaligned.load();
    gRates.rtMixed = k.rtMixed.load();
    gRates.levelPasses = k.levelPasses.load();
    const gm20b::ResourceSet::Counts sets = gRM->raw->setCounts();
    const gm20b::Device::Trimmed kept = gRM->raw->keptImports();
    const SwitchMetalPassTrim passes = gm20bPassHeld();
    const int n = snprintf(
        out, size,
        "gm20b %.0fs c=%llu(%.1f/s) enc=%.0fus/c d=%.1f p=%.1f b=%.2f "
        "defer=%llu rw=%llu pool=%lluMB/%llu grow=%llu imp=%llu/%lluMB clean=%.1fKB/c ce=%llu/%.0fKB "
        "ev=%llu/%llu sig=%llu/%llu/%llu ws=%llu/%lluus wc=%llu/%lluus set_refusals=%u set_rebuilds=%u "
        "kept=%zu/%lluKB passes=%zu/%zu hdr=%u%s",
        secs, (unsigned long long)c, c / secs, (s.encodeUs.load() - gRates.encodeUs) / per,
        (s.draws.load() - gRates.draws) / per, (s.passes.load() - gRates.passes) / per,
        (s.copies.load() - gRates.copies) / per, (unsigned long long)(s.deferred.load() - gRates.deferred),
        (unsigned long long)(k.ringWaits.load() - gRates.ringWaits), (unsigned long long)(k.poolBytes.load() >> 20),
        (unsigned long long)k.poolChunks.load(), (unsigned long long)(k.poolStalls.load() - gRates.poolStalls),
        (unsigned long long)k.imports.load(), (unsigned long long)(k.importBytes.load() >> 20),
        k.cleanBytes.exchange(0) / 1024.0 / per, (unsigned long long)k.ceOps.exchange(0),
        k.ceBytes.exchange(0) / 1024.0, (unsigned long long)(s.eventsReleased.load() - gRates.eventsReleased),
        (unsigned long long)(s.heldReturns.load() - gRates.heldReturns),
        (unsigned long long)(s.signalsPublished.load() - gRates.signalsPublished),
        (unsigned long long)(s.signalsAfterWork.load() - gRates.signalsAfterWork),
        (unsigned long long)(s.waitsOnPromise.load() - gRates.waitsOnPromise),
        (unsigned long long)(s.scheduledWaits.load() - gRates.scheduledWaits),
        (unsigned long long)(s.scheduledWaitUs.load() - gRates.scheduledWaitUs),
        (unsigned long long)(s.completedWaits.load() - gRates.completedWaits),
        (unsigned long long)(s.completedWaitUs.load() - gRates.completedWaitUs), sets.refusals, sets.rebuilds,
        kept.keptImports, (unsigned long long)(kept.keptImportBytes >> 10), passes.passes, passes.textures,
        gRM->raw->headersLeft(), diag);
    gRates.raw = raw;
    gRates.encodeUs = s.encodeUs.load();
    gRates.draws = s.draws.load();
    gRates.passes = s.passes.load();
    gRates.copies = s.copies.load();
    gRates.deferred = s.deferred.load();
    gRates.poolStalls = k.poolStalls.load();
    gRates.ringWaits = k.ringWaits.load();
    gRates.eventsReleased = s.eventsReleased.load();
    gRates.heldReturns = s.heldReturns.load();
    gRates.signalsPublished = s.signalsPublished.load();
    gRates.signalsAfterWork = s.signalsAfterWork.load();
    gRates.waitsOnPromise = s.waitsOnPromise.load();
    gRates.scheduledWaits = s.scheduledWaits.load();
    gRates.scheduledWaitUs = s.scheduledWaitUs.load();
    gRates.completedWaits = s.completedWaits.load();
    gRates.completedWaitUs = s.completedWaitUs.load();
    gRates.lastUs = now;
    return n > 0 ? std::min(size_t(n), size - 1) : 0;
#else
    (void)out;
    (void)size;
    return 0;
#endif
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// "fetch seq=S/Tt ilk=I/Bb r2=F r3=G rec=<reason>:n,... enc=<reason>:n,... sto=<reason>:n,...": drawn a triangle at a time
// (the triangles), inside the pixel interlock (B of them with the fixed-function blend computed in the shader), by their
// state's fetch-free form through the ROP's blend (r2) or its iterated blend (r3), refused as recorded and at encoding by
// reason, and storage forms left for their barrier form by reason ("-" for none).
// The plugin's rates timer only (its 5 s windows).
size_t metal_fetch_rates(char *out, size_t size)
{
    if (!gRM || !size)
        return 0;
    const gm20b::FetchCounts &k = gRM->raw->stats.fetch;
    constexpr size_t kTotals = 6, kAll = kTotals + gm20b::kFetchRefusals + gm20b::kStorageFallbacks;
    static uint64_t last[kAll];
    uint64_t now[kAll] = {k.sequential.load(),        k.sequentialTriangles.load(), k.interlocked.load(),
                          k.interlockedBlends.load(), k.fetchFree[0].load(),        k.fetchFree[1].load()};
    for (size_t r = 0; r < gm20b::kFetchRefusals; ++r)
        now[kTotals + r] = k.refusedAtEncode[r].load();
    for (size_t r = 0; r < gm20b::kStorageFallbacks; ++r)
        now[kTotals + gm20b::kFetchRefusals + r] = k.storageFallback[r].load();
    uint64_t d[kAll];
    bool any = false;
    for (size_t i = 0; i < kAll; ++i) {
        d[i] = now[i] - last[i];
        last[i] = now[i];
        any |= d[i] != 0;
    }
    if (!any)
        return 0;
    auto reasons = [&](const uint64_t *counts, size_t kinds, const char *const *names, char *at, size_t room) {
        size_t used = 0;
        for (size_t r = 1; r < kinds && used < room; ++r)
            if (counts[r])
                used += size_t(snprintf(at + used, room - used, "%s%s:%llu", used ? "," : "", names[r],
                                        (unsigned long long)counts[r]));
        if (!used)
            snprintf(at, room, "-");
    };
    char encode[112], storage[112];
    reasons(d + kTotals, gm20b::kFetchRefusals, gm20b::kFetchRefusalNames, encode, sizeof encode);
    reasons(d + kTotals + gm20b::kFetchRefusals, gm20b::kStorageFallbacks, gm20b::kStorageFallbackNames, storage,
            sizeof storage);
    const int n =
        snprintf(out, size, "fetch seq=%llu/%llut ilk=%llu/%llub r2=%llu r3=%llu enc=%s sto=%s",
                 (unsigned long long)d[0], (unsigned long long)d[1], (unsigned long long)d[2], (unsigned long long)d[3],
                 (unsigned long long)d[4], (unsigned long long)d[5], encode, storage);
    return n > 0 ? std::min(size_t(n), size - 1) : 0;
}

size_t metal_encode_rates(char *out, size_t size)
{
    using gm20b::EncodePart;
    if (!gRM || !size)
        return 0;
    const gm20b::EncodeTotals &k = gRM->raw->stats.encode;
    static uint64_t lastEncodes, lastWall, lastTicks[gm20b::kEncodeParts], lastCalls[gm20b::kEncodeParts];
    const uint64_t encodes = k.encodes.load(std::memory_order_relaxed) - lastEncodes;
    if (!encodes)
        return 0;
    lastEncodes += encodes;
    const double tickUs = 1.0 / gm20b::encodeTicksPerUs(), usPerTick = tickUs / double(encodes);
    static uint64_t lastSelf[gm20b::kEncodeParts], lastCounts[gm20b::kEncodeCounts], lastMallocEncodes;
    double us[gm20b::kEncodeParts], self[gm20b::kEncodeParts], calls[gm20b::kEncodeParts], counts[gm20b::kEncodeCounts];
    for (size_t i = 0; i < gm20b::kEncodeParts; ++i) {
        const uint64_t t = k.ticks[i].load(std::memory_order_relaxed), n = k.calls[i].load(std::memory_order_relaxed),
                       s = k.self[i].load(std::memory_order_relaxed);
        us[i] = double(t - lastTicks[i]) * usPerTick;
        self[i] = double(s - lastSelf[i]) * usPerTick;
        calls[i] = double(n - lastCalls[i]) / double(encodes);
        lastTicks[i] = t;
        lastSelf[i] = s;
        lastCalls[i] = n;
    }
    const uint64_t mallocEncodes = k.mallocEncodes.load(std::memory_order_relaxed) - lastMallocEncodes;
    lastMallocEncodes += mallocEncodes;
    for (size_t i = 0; i < gm20b::kEncodeCounts; ++i) {
        const uint64_t n = k.counts[i].load(std::memory_order_relaxed);
        counts[i] = double(n - lastCounts[i]) / double(encodes);
        lastCounts[i] = n;
    }
    const double mallocs = mallocEncodes
                               ? counts[size_t(gm20b::EncodeCount::Mallocs)] * double(encodes) / double(mallocEncodes)
                               : -1.0,
                 frees = mallocEncodes
                             ? counts[size_t(gm20b::EncodeCount::Frees)] * double(encodes) / double(mallocEncodes)
                             : -1.0;
    const uint64_t wall = k.wallTicks.load(std::memory_order_relaxed);
    const double wallUs = double(wall - lastWall) * usPerTick;
    lastWall = wall;
    static uint64_t lastExecUses, lastExecPushes;
    const uint64_t usesNow = k.execUses.load(std::memory_order_relaxed),
                   pushesNow = k.execPushes.load(std::memory_order_relaxed);
    const double execs = calls[size_t(EncodePart::Exec)] * double(encodes),
                 execUses = execs ? double(usesNow - lastExecUses) / execs : 0.0,
                 execPushes = execs ? double(pushesNow - lastExecPushes) / execs : 0.0;
    lastExecUses = usesNow;
    lastExecPushes = pushesNow;
    double spans = 0;
    for (size_t i = 0; i < gm20b::kEncodeParts; ++i)
        spans += calls[i];
    const double spanNs = double(gm20b::spanCostTicksPer1024()) * tickUs * 1000.0 / 1024.0;
    const double longestPrepareUs =
        double(gRM->raw->stats.encode.longestPrepare.exchange(0, std::memory_order_relaxed)) * tickUs;
    // per encode: state segment binds, binds of the segment the channel held, and the inline words the binds replaced;
    // per window: command buffers whose structure was the last one's, one of the three before, or new (replay_shape.h)
    gm20b::Device::Stats &st = gRM->raw->stats;
    auto since = [](const std::atomic<uint64_t> &v, uint64_t &last) {
        const uint64_t now = v.load(std::memory_order_relaxed), d = now - last;
        last = now;
        return d;
    };
    static uint64_t lastBinds, lastSkips, lastWords, lastSame, lastRecent, lastFresh, lastCompared, lastInTables;
    const double segBinds = double(since(st.segmentBinds, lastBinds)) / double(encodes),
                 segSkips = double(since(st.segmentSkips, lastSkips)) / double(encodes),
                 segWords = double(since(st.segmentWords, lastWords)) / double(encodes);
    const uint64_t same = since(st.replay.same, lastSame), recent = since(st.replay.recent, lastRecent),
                   fresh = since(st.replay.fresh, lastFresh);
    const double slotsCompared = double(since(st.slotsCompared, lastCompared)) / double(encodes),
                 slotsInTables = double(since(st.slotsInTables, lastInTables)) / double(encodes);
    auto part = [&](EncodePart p) {
        return us[size_t(p)];
    };
    auto count = [&](EncodePart p) {
        return calls[size_t(p)];
    };
    auto own = [&](EncodePart p) {
        return self[size_t(p)];
    };
    auto events = [&](gm20b::EncodeCount c) {
        return counts[size_t(c)];
    };
    const double draws = count(EncodePart::Draw);
    const int head = snprintf(
        out, size,
        "encode n=%llu wall=%.0f res=%.0f loop=%.0f state=%.0f pass=%.0f splan=%.0f fuse=%.0f pend=%.0f/%.1f "
        "draw=%.0f/%.1f prep=%.0f geo=%.0f desc=%.0f bar=%.0f slaunch=%.0f/%.1f blit=%.0f comp=%.0f tail=%.0f "
        "commit=%.0f prolog=%.0f exec=%.0f/%.1f uses=%.0f pushes=%.0f kern=%.0f/%.1f "
        "pmax=%.0f use=%.0f/%.1f uni=%.0f/%.1f bind=%.0f var=%.0f pprol=%.0f slots=%.0f/%.0f spans=%.0f@%.0fns "
        "seg=%.1f/%.1f segw=%.0f replay=%llu/%llu/%llu",
        (unsigned long long)encodes, wallUs, part(EncodePart::Resources), part(EncodePart::Loop),
        part(EncodePart::State), part(EncodePart::Pass), part(EncodePart::SplitPlan), part(EncodePart::Fusion),
        part(EncodePart::PassEnd), count(EncodePart::PassEnd), part(EncodePart::Draw), count(EncodePart::Draw),
        part(EncodePart::Prepare), part(EncodePart::Geometry), part(EncodePart::Descriptors),
        part(EncodePart::Barriers), part(EncodePart::SplitLaunch), count(EncodePart::SplitLaunch),
        part(EncodePart::Blit), part(EncodePart::Compute), part(EncodePart::Tail), part(EncodePart::Commit),
        part(EncodePart::Prologues), part(EncodePart::Exec), count(EncodePart::Exec), execUses, execPushes,
        part(EncodePart::Kernel), count(EncodePart::Kernel), longestPrepareUs, part(EncodePart::Use),
        count(EncodePart::Use), part(EncodePart::Uniforms), count(EncodePart::Uniforms), part(EncodePart::Bind),
        part(EncodePart::Variant), part(EncodePart::PassPrologue), slotsCompared, slotsInTables, spans, spanNs,
        segBinds, segSkips, segWords, (unsigned long long)same, (unsigned long long)recent, (unsigned long long)fresh);
    if (head <= 0 || size_t(head) >= size)
        return head > 0 ? size - 1 : 0;
    // self times: walk= the loop's own (the plan walk between its parts), res/tail= the phases', state/draw/prep/
    // pend= theirs without the parts nested in them; apply= the planned bindings applied, tex= a binding table's texture
    // and sampler entries, retire= the facade's after the encode; per command buffer the events of
    // encode_profile.h EncodeCount (mal/free -1: not counted), applied= per draw, useadd= the uses that reached
    // the job's list
    const int tail = snprintf(
        out + head, size - head,
        " walk=%.0f rself=%.0f tself=%.0f sself=%.0f dself=%.0f pself=%.0f eself=%.0f apply=%.0f tex=%.0f retire=%.0f "
        "pbeg=%.0f "
        "mal=%.1f free=%.1f lk=%.1f snd=%.1f applied=%.2f/d useadd=%.1f/%.1f descb=%.1f descu=%.1f geov=%.1f",
        own(EncodePart::Loop), own(EncodePart::Resources), own(EncodePart::Tail), own(EncodePart::State),
        own(EncodePart::Draw), own(EncodePart::Prepare), own(EncodePart::PassEnd), part(EncodePart::Apply),
        part(EncodePart::Textures), part(EncodePart::Retire), part(EncodePart::PassBegin), mallocs, frees,
        events(gm20b::EncodeCount::Locks), events(gm20b::EncodeCount::Sends),
        draws ? events(gm20b::EncodeCount::Applied) / draws : 0.0, events(gm20b::EncodeCount::UseAdds),
        events(gm20b::EncodeCount::Uses), events(gm20b::EncodeCount::DescriptorBuilds),
        events(gm20b::EncodeCount::DescriptorUploads), events(gm20b::EncodeCount::GeometryVertices));
    size_t n = size_t(head) + (tail > 0 ? size_t(tail) : 0);
    // the pooled encoders' blocks made and freed since the process began (object_pool.h): with none freed, made is
    // the most of each kind this process had alive at once
    if (n < size - 1) {
        const int pools = snprintf(out + n, size - n, " encmade=%llu/%llu/%llu encfreed=%llu/%llu/%llu",
                                   (unsigned long long)gm20bPoolMade([SwitchMetalRenderEncoder class]),
                                   (unsigned long long)gm20bPoolMade([SwitchMetalBlitEncoder class]),
                                   (unsigned long long)gm20bPoolMade([SwitchMetalComputeEncoder class]),
                                   (unsigned long long)gm20bPoolFreed([SwitchMetalRenderEncoder class]),
                                   (unsigned long long)gm20bPoolFreed([SwitchMetalBlitEncoder class]),
                                   (unsigned long long)gm20bPoolFreed([SwitchMetalComputeEncoder class]));
        n += pools > 0 ? size_t(pools) : 0;
    }
    return std::min(n, size - 1);
}

size_t metal_fetch_keys(char *out, size_t size)
{
    if (!gRM || !size)
        return 0;
    gm20b::FetchCounts &k = gRM->raw->stats.fetch;
    size_t used = 0;
    bool any = false;
    auto keys = [&](const char *name, gm20b::KeyTally &tally) {
        const auto top = tally.take(8);
        any |= !top.empty();
        used += size_t(snprintf(out + used, size - used, " %s=", name));
        for (size_t i = 0; i < top.size() && used < size; i++)
            used += size_t(snprintf(out + used, size - used, "%s%016llx:%llu", i ? "," : "",
                                    (unsigned long long)top[i].first, (unsigned long long)top[i].second));
        if (top.empty() && used < size)
            used += size_t(snprintf(out + used, size - used, "-"));
        used = std::min(used, size - 1);
    };
    used = size_t(snprintf(out, size, "fetch keys"));
    keys("enc", k.encodeKeys);
    keys("sto", k.fallbackKeys);
    keys("ilk", k.interlockKeys);
    keys("gen", k.genericKeys);
    return any ? used : 0;
}

// The stall line (r7p8: the compositor stopped committing for good with nothing in the log): every 5 s, on a timer of its own,
// what the driver has pending, read without waiting (a lock another thread holds is reported held, not waited for). Once
// nothing was committed or completed over two ticks while work is pending or one of its locks stays held, one line names it,
// once per process. Nothing else: an idle screen has nothing pending and says nothing.
// An untimed shared event wait waits as Metal's GPU would, without end: one line every 10 s for each still waiting after
// 10 s, naming the event, the value it waits for and the value it has
static void gm20bEventWaitTick()
{
    const uint64_t now = nowUs();
    std::unique_lock<gm20b::Mutex> h(gHoldersLock, std::try_to_lock);
    if (!h.owns_lock())
        return;
    for (auto &[cb, note] : gEventWaits) {
        if (now - note.sinceUs < 10000000 || now - note.saidUs < 10000000)
            continue;
        const uint64_t reached = [(__bridge id<MTLSharedEvent>)note.event signaledValue];
        if (reached >= note.value)
            continue;
        note.saidUs = now;
        os_log(
            gm20bLog(),
            "SwitchMetal: still waiting on shared event %p for value %llu (it has %llu) for %llu s, queue %{public}s",
            note.event, (unsigned long long)note.value, (unsigned long long)reached,
            (unsigned long long)((now - note.sinceUs) / 1000000), cb->_queue.label.UTF8String ?: "-");
    }
}

void gm20bStallTick()
{
    gm20bEventWaitTick();
    gm20bMemoryTick();
    static uint64_t lastCommits = ~0ull, lastDone = ~0ull;
    static int still = 0;
    static bool said = false;
    if (said || !gRM)
        return;
    GM20BState &r = *gRM;
    Stats &s = r.stats;
    const uint64_t commits = s.raw.load(), done = s.finished.load();
    const bool moved = commits != lastCommits || done != lastDone;
    lastCommits = commits;
    lastDone = done;
    if (moved) {
        still = 0;
        return;
    }
    const long waiting = long(r.waitingCommitted.load(std::memory_order_relaxed)), inflight = long(r.inflight.size());
    bool holders = false;
    {
        std::unique_lock<gm20b::Mutex> h(gHoldersLock, std::try_to_lock);
        holders = h.owns_lock();
    }
    const bool device = r.raw->lockFree();
    const uint32_t submitted = r.raw->submitted(), completed = r.raw->completed();
    const bool pending = waiting != 0 || inflight != 0 || !holders || !device || submitted != completed;
    if (!pending) {
        still = 0;
        return;
    }
    if (++still < 2)
        return;
    said = true;
    os_log_error(gm20bLog(),
                 "SwitchMetal: stall: nothing committed or completed for 10 s with work pending: waiting %ld, "
                 "inflight %ld, locks free: holders %d device %d, seqno submitted %u completed %u, commits %llu "
                 "completed %llu",
                 waiting, inflight, int(holders), int(device), submitted, completed, (unsigned long long)commits,
                 (unsigned long long)done);
}
#endif

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// The fetch barrier's way, switched from the Mac within one boot (`idevicenotificationproxy -u <udid> post
// com.brunostjohn.switch.debug.fetch-barrier-<ps|psonly|none>`), to measure what the barrier costs: the shipped barrier
// and texture invalidate (ps) against the barrier alone (psonly) and nothing (none). Measurement only: psonly and none
// let a fetching draw read colors still being written. wfi, HW Test's ui.glass_ab, goes back to ps after a minute
// unless another way was posted since
void watchFetchBarrierWay(gm20b::Device *dev)
{
    using Way = gm20b::Device::FetchBarrier;
    static std::atomic<uint32_t> posted{0};
    for (const Way way : {Way::PixelShader, Way::PixelShaderOnly, Way::None, Way::WaitForIdle}) {
        char name[96];
        snprintf(name, sizeof name, "com.brunostjohn.switch.debug.fetch-barrier-%s",
                 gm20b::Device::fetchBarrierName(way));
        int token;
        notify_register_dispatch(name, &token, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
            dev->fetchBarrier.store(way, std::memory_order_relaxed);
            const uint32_t at = posted.fetch_add(1, std::memory_order_relaxed) + 1;
            os_log(gm20bLog(), "SwitchMetal: fetch barrier way %{public}s (debug notification)",
                   gm20b::Device::fetchBarrierName(way));
            if (way != Way::WaitForIdle)
                return;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC),
                           dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                               if (posted.load(std::memory_order_relaxed) == at)
                                   dev->fetchBarrier.store(Way::PixelShader, std::memory_order_relaxed);
                           });
        });
    }
}

// The keyline probe armed (pipeline.h armKeylineProbe) in every process by HW Test's ui.pocket_keyline before it draws
// (`idevicenotificationproxy -u <udid> post com.brunostjohn.switch.debug.keyline-probe` from the Mac)
void watchKeylineProbe()
{
    int token;
    notify_register_dispatch("com.brunostjohn.switch.debug.keyline-probe", &token,
                             dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
                                 gm20b::armKeylineProbe();
                                 os_log(gm20bLog(), "SwitchMetal: keyline probe armed (debug notification)");
                             });
}
#endif
