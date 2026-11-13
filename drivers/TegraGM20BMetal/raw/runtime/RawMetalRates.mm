// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"
#include "raw_object_pool.h"
#include "raw_resolve_rule.h"

#if SWITCH_RAW_DIAGNOSTICS
static uint64_t gPredRates[PredCount];  // the rates line's last gPredCbs (its timer thread only)

// ---- the rates line ----
static struct {
    uint64_t raw, m2v, encodeUs, draws, passes, copies, deferred, aborted, reasons[WhyCount], ringWaits, lastUs,
        switches, cpuOrdered, acquires, poolStalls, eventsReleased, heldReturns, signalsPublished, signalsAfterWork,
        waitsOnPromise, scheduledWaits, scheduledWaitUs, completedWaits, completedWaitUs, abortUs, replayUs,
        replayClean, rtUnaligned, rtMixed, predOnly[PredCount], splits, splitM2v, eventSplits, eventWhole, levelPasses;
} gRates;

#endif

bool raw_metal_texture_kept(id<MTLTexture> texture)
{
    return [(id)texture isKindOfClass:[RMTexture class]] && ((RMTexture *)texture)->_rawOk;
}

bool raw_metal_chains_on(void)
{
    return gRM != nullptr;
}

const void *raw_metal_texture_image(id<MTLTexture> texture, size_t *pitch)
{
    if (![(id)texture isKindOfClass:[RMTexture class]])
        return nullptr;
    RMTexture *t = (RMTexture *)texture;
    if (!t->_rawOk || !t->_raw.cpu || t->_raw.blockLinear)
        return nullptr;
    if (pitch)
        *pitch = t->_raw.pitch;
    return t->_raw.cpu;
}

size_t raw_metal_rates(char *out, size_t size)
{
#if SWITCH_RAW_DIAGNOSTICS
    if (!gRM || !size)
        return 0;
    Stats &s = gRM->stats;
    const uint64_t raw = s.raw.load(), m2v = s.m2v.load(), now = nowUs();
    const uint64_t c = raw - gRates.raw, f = m2v - gRates.m2v, sp = s.splits.load() - gRates.splits;
    if (!c && !f && !sp)
        return 0;
    const double secs = gRates.lastUs ? (now - gRates.lastUs) / 1e6 : 5.0;
    char reasons[160] = "";
    size_t at = 0;
    for (int r = 1; r < WhyCount; r++) {
        const uint64_t n = s.reasons[r].load() - gRates.reasons[r];
        if (n && at < sizeof reasons)
            at += snprintf(reasons + at, sizeof reasons - at, "%s%s:%llu", at ? "," : "", kReasonNames[r],
                           (unsigned long long)n);
        gRates.reasons[r] = s.reasons[r].load();
    }
    raw::Device::Stats &k = gRM->raw->stats;
    const double per = c + sp ? double(c + sp) : 1.0;  // the raw path's command buffers, the split ones included
    // diagnostics (r10-rawsweep): per predicate, the command buffers that hit it / those it alone sent to m2v, the ten most hit
    char preds[256] = "";
    {
        uint64_t cbs[PredCount], only[PredCount];
        for (int i = 0; i < PredCount; i++) {
            const uint64_t now = gPredCbs[i].load(std::memory_order_relaxed),
                           o = gPredOnly[i].load(std::memory_order_relaxed);
            cbs[i] = now - gPredRates[i];
            only[i] = o - gRates.predOnly[i];
            gPredRates[i] = now;
            gRates.predOnly[i] = o;
        }
        size_t pat = 0;
        for (int shown = 0; shown < 10; shown++) {
            int best = 0;
            for (int i = 1; i < PredCount; i++)
                if (cbs[i] > cbs[best])
                    best = i;
            if (!best || !cbs[best] || pat >= sizeof preds)
                break;
            pat += snprintf(preds + pat, sizeof preds - pat, "%s%s:%llu/%llu", pat ? "," : "", kPredNames[best],
                            (unsigned long long)cbs[best], (unsigned long long)only[best]);
            cbs[best] = 0;
        }
    }
    // replay= and rclean= per m2v submission: whole command buffers and split ones' m2v segments
    const uint64_t evSplits = s.eventSplits.load() - gRates.eventSplits,
                   evWhole = s.eventWhole.load() - gRates.eventWhole;
    const uint64_t aborts = s.aborted.load() - gRates.aborted, segs = s.splitM2v.load() - gRates.splitM2v,
                   m2vs = f + segs ? f + segs : 1;
    const raw::Compiler::Stats made = gRM->completion ? gRM->completion->stats() : raw::Compiler::Stats();
    char diag[1024];
    snprintf(
        diag, sizeof diag,
        " sp=%llu/%llu evsp=%llu/%llu abortenc=%.0fus/a replay=%.0fus/m rclean=%.0fKB/m "
        "rt128=%llu(pitch %u va&255 %u) rtmix=%llu lvl=%llu states=%llu/%llu/%llu/%llu/%.0fms "
        "atcreate=%llu/%.0fus/s%llu/o%llu swait=%.0fms comp=%llu/%lluKB/v%llu/m%llu/f%llu/x%llu bk=%llu/%lluMB/r%llu "
        "tfree=%lluMB zbc=%llu/%llu "
        "split=%llu rsv=t%llu/%llu,x%llu/%llu,p%llu/%llu/%llu why=%s",
        (unsigned long long)sp, (unsigned long long)segs, (unsigned long long)evSplits, (unsigned long long)evWhole,
        (s.abortUs.load() - gRates.abortUs) / double(aborts ? aborts : 1),
        (s.replayUs.load() - gRates.replayUs) / double(m2vs),
        (s.replayClean.load() - gRates.replayClean) / 1024.0 / double(m2vs),
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
        (unsigned long long)k.resolveRoute[raw::ResolveByTwod][0].exchange(0),
        (unsigned long long)k.resolveRoute[raw::ResolveByTwod][1].exchange(0),
        (unsigned long long)k.resolveRoute[raw::ResolveByComplement][0].exchange(0),
        (unsigned long long)k.resolveRoute[raw::ResolveByComplement][1].exchange(0),
        (unsigned long long)k.resolveRoute[raw::ResolveByPass][0].exchange(0),
        (unsigned long long)k.resolveRoute[raw::ResolveByPass][1].exchange(0),
        (unsigned long long)k.resolveRoute[raw::ResolveByPass][2].exchange(0), *preds ? preds : "-");
#if SWITCH_FRAME_TIMELINE
    {
        const size_t used = strlen(diag);
        snprintf(diag + used, sizeof diag - used,
                 " rsplit=%.0f/%.0f/%.0fus/m rmax=%lluus rdraw=%.1f/m tw=t%llu/%lluus/b%llu/o%llu/f%llu sbf=%llu"
                 " fb=%llu/%llu/%llu/%llu(%s) clr=%llu/%llu zero=%llu",
                 (s.replaySlotUs.exchange(0) / double(m2vs)), (s.replayTwinUs.exchange(0) / double(m2vs)),
                 (s.replayCommitUs.exchange(0) / double(m2vs)), (unsigned long long)s.replayMaxUs.exchange(0),
                 s.replayDraws.exchange(0) / double(m2vs), (unsigned long long)s.twinTextures.exchange(0),
                 (unsigned long long)s.twinTextureUs.exchange(0), (unsigned long long)s.twinBuffers.exchange(0),
                 (unsigned long long)s.twinOthers.exchange(0), (unsigned long long)s.twinForwards.exchange(0),
                 (unsigned long long)s.scheduledByFinish.exchange(0), (unsigned long long)k.fetchBarriers.exchange(0),
                 (unsigned long long)k.fetchBarriersElided.exchange(0),
                 (unsigned long long)k.fetchBarriersAnyColor.exchange(0),
                 (unsigned long long)k.fetchBarriersInterlocked.exchange(0),
                 raw::Device::fetchBarrierName(gRM->raw->fetchBarrier.load(std::memory_order_relaxed)),
                 (unsigned long long)k.clears.exchange(0), (unsigned long long)k.clearsElided.exchange(0),
                 (unsigned long long)s.zeroAnswers.exchange(0));
    }
#endif
    gRates.abortUs = s.abortUs.load();
    gRates.splits = s.splits.load();
    gRates.splitM2v = s.splitM2v.load();
    gRates.eventSplits = s.eventSplits.load();
    gRates.eventWhole = s.eventWhole.load();
    gRates.replayUs = s.replayUs.load();
    gRates.replayClean = s.replayClean.load();
    gRates.rtUnaligned = k.rtUnaligned.load();
    gRates.rtMixed = k.rtMixed.load();
    gRates.levelPasses = k.levelPasses.load();
    const raw::ResourceSet::Counts sets = gRM->raw->setCounts();
    const int n = snprintf(
        out, size,
        "raw %.0fs c=%llu(%.1f/s) m2v=%llu%s%s%s sw=%llu cpu=%llu acq=%llu enc=%.0fus/c d=%.1f p=%.1f b=%.2f "
        "defer=%llu abort=%llu rw=%llu pool=%lluMB/%llu grow=%llu imp=%llu/%lluMB clean=%.1fKB/c ce=%llu/%.0fKB "
        "ev=%llu/%llu sig=%llu/%llu/%llu ws=%llu/%lluus wc=%llu/%lluus here=%llu set_refusals=%u set_rebuilds=%u%s",
        secs, (unsigned long long)c, c / secs, (unsigned long long)f, *reasons ? "(" : "", reasons, *reasons ? ")" : "",
        (unsigned long long)(s.switches.load() - gRates.switches),
        (unsigned long long)(s.cpuOrdered.load() - gRates.cpuOrdered),
        (unsigned long long)(s.acquires.load() - gRates.acquires), (s.encodeUs.load() - gRates.encodeUs) / per,
        (s.draws.load() - gRates.draws) / per, (s.passes.load() - gRates.passes) / per,
        (s.copies.load() - gRates.copies) / per, (unsigned long long)(s.deferred.load() - gRates.deferred),
        (unsigned long long)(s.aborted.load() - gRates.aborted),
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
        (unsigned long long)(s.completedWaitUs.load() - gRates.completedWaitUs),
        (unsigned long long)(gRM->completion ? gRM->completion->compiledHere() : 0), sets.refusals, sets.rebuilds,
        diag);
    gRates.raw = raw;
    gRates.m2v = m2v;
    gRates.encodeUs = s.encodeUs.load();
    gRates.draws = s.draws.load();
    gRates.passes = s.passes.load();
    gRates.copies = s.copies.load();
    gRates.deferred = s.deferred.load();
    gRates.aborted = s.aborted.load();
    gRates.switches = s.switches.load();
    gRates.cpuOrdered = s.cpuOrdered.load();
    gRates.acquires = s.acquires.load();
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
size_t raw_metal_fetch_rates(char *out, size_t size)
{
    if (!gRM || !size)
        return 0;
    const raw::FetchCounts &k = gRM->raw->stats.fetch;
    constexpr size_t kTotals = 6, kAll = kTotals + 2 * raw::kFetchRefusals + raw::kStorageFallbacks;
    static uint64_t last[kAll];
    uint64_t now[kAll] = {k.sequential.load(),        k.sequentialTriangles.load(), k.interlocked.load(),
                          k.interlockedBlends.load(), k.fetchFree[0].load(),        k.fetchFree[1].load()};
    for (size_t r = 0; r < raw::kFetchRefusals; ++r) {
        now[kTotals + r] = k.refusedAtRecord[r].load();
        now[kTotals + raw::kFetchRefusals + r] = k.refusedAtEncode[r].load();
    }
    for (size_t r = 0; r < raw::kStorageFallbacks; ++r)
        now[kTotals + 2 * raw::kFetchRefusals + r] = k.storageFallback[r].load();
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
    char record[96], encode[112], storage[112];
    reasons(d + kTotals, raw::kFetchRefusals, raw::kFetchRefusalNames, record, sizeof record);
    reasons(d + kTotals + raw::kFetchRefusals, raw::kFetchRefusals, raw::kFetchRefusalNames, encode, sizeof encode);
    reasons(d + kTotals + 2 * raw::kFetchRefusals, raw::kStorageFallbacks, raw::kStorageFallbackNames, storage,
            sizeof storage);
    const int n =
        snprintf(out, size, "fetch seq=%llu/%llut ilk=%llu/%llub r2=%llu r3=%llu rec=%s enc=%s sto=%s",
                 (unsigned long long)d[0], (unsigned long long)d[1], (unsigned long long)d[2], (unsigned long long)d[3],
                 (unsigned long long)d[4], (unsigned long long)d[5], record, encode, storage);
    return n > 0 ? std::min(size_t(n), size - 1) : 0;
}

size_t raw_metal_encode_rates(char *out, size_t size)
{
    using raw::EncodePart;
    if (!gRM || !size)
        return 0;
    const raw::EncodeTotals &k = gRM->raw->stats.encode;
    static uint64_t lastEncodes, lastWall, lastTicks[raw::kEncodeParts], lastCalls[raw::kEncodeParts];
    const uint64_t encodes = k.encodes.load(std::memory_order_relaxed) - lastEncodes;
    if (!encodes)
        return 0;
    lastEncodes += encodes;
    const double tickUs = 1.0 / raw::encodeTicksPerUs(), usPerTick = tickUs / double(encodes);
    static uint64_t lastSelf[raw::kEncodeParts], lastCounts[raw::kEncodeCounts], lastMallocEncodes;
    double us[raw::kEncodeParts], self[raw::kEncodeParts], calls[raw::kEncodeParts], counts[raw::kEncodeCounts];
    for (size_t i = 0; i < raw::kEncodeParts; ++i) {
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
    for (size_t i = 0; i < raw::kEncodeCounts; ++i) {
        const uint64_t n = k.counts[i].load(std::memory_order_relaxed);
        counts[i] = double(n - lastCounts[i]) / double(encodes);
        lastCounts[i] = n;
    }
    const double mallocs = mallocEncodes
                               ? counts[size_t(raw::EncodeCount::Mallocs)] * double(encodes) / double(mallocEncodes)
                               : -1.0,
                 frees = mallocEncodes
                             ? counts[size_t(raw::EncodeCount::Frees)] * double(encodes) / double(mallocEncodes)
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
    for (size_t i = 0; i < raw::kEncodeParts; ++i)
        spans += calls[i];
    const double spanNs = double(raw::spanCostTicksPer1024()) * tickUs * 1000.0 / 1024.0;
    const double longestPrepareUs =
        double(gRM->raw->stats.encode.longestPrepare.exchange(0, std::memory_order_relaxed)) * tickUs;
    // per encode: state segment binds, binds of the segment the channel held, and the inline words the binds replaced;
    // per window: command buffers whose structure was the last one's, one of the three before, or new (raw_replay_shape.h)
    raw::Device::Stats &st = gRM->raw->stats;
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
    auto events = [&](raw::EncodeCount c) {
        return counts[size_t(c)];
    };
    const double draws = count(EncodePart::Draw);
    const int head = snprintf(
        out, size,
        "encode n=%llu wall=%.0f res=%.0f loop=%.0f state=%.0f pass=%.0f splan=%.0f fuse=%.0f pend=%.0f/%.1f "
        "draw=%.0f/%.1f prep=%.0f geo=%.0f desc=%.0f bar=%.0f slaunch=%.0f/%.1f blit=%.0f comp=%.0f tail=%.0f "
        "commit=%.0f prolog=%.0f exec=%.0f/%.1f uses=%.0f pushes=%.0f kern=%.0f/%.1f gacq=%.0f/%.1f grel=%.0f/%.1f "
        "pmax=%.0f use=%.0f/%.1f uni=%.0f/%.1f bind=%.0f var=%.0f pprol=%.0f slots=%.0f/%.0f spans=%.0f@%.0fns "
        "seg=%.1f/%.1f segw=%.0f replay=%llu/%llu/%llu",
        (unsigned long long)encodes, wallUs, part(EncodePart::Resources), part(EncodePart::Loop),
        part(EncodePart::State), part(EncodePart::Pass), part(EncodePart::SplitPlan), part(EncodePart::Fusion),
        part(EncodePart::PassEnd), count(EncodePart::PassEnd), part(EncodePart::Draw), count(EncodePart::Draw),
        part(EncodePart::Prepare), part(EncodePart::Geometry), part(EncodePart::Descriptors),
        part(EncodePart::Barriers), part(EncodePart::SplitLaunch), count(EncodePart::SplitLaunch),
        part(EncodePart::Blit), part(EncodePart::Compute), part(EncodePart::Tail), part(EncodePart::Commit),
        part(EncodePart::Prologues), part(EncodePart::Exec), count(EncodePart::Exec), execUses, execPushes,
        part(EncodePart::Kernel), count(EncodePart::Kernel), part(EncodePart::GeometryAcquire),
        count(EncodePart::GeometryAcquire), part(EncodePart::GeometryRelease), count(EncodePart::GeometryRelease),
        longestPrepareUs, part(EncodePart::Use), count(EncodePart::Use), part(EncodePart::Uniforms),
        count(EncodePart::Uniforms), part(EncodePart::Bind), part(EncodePart::Variant), part(EncodePart::PassPrologue),
        slotsCompared, slotsInTables, spans, spanNs, segBinds, segSkips, segWords, (unsigned long long)same,
        (unsigned long long)recent, (unsigned long long)fresh);
    if (head <= 0 || size_t(head) >= size)
        return head > 0 ? size - 1 : 0;
    // self times: walk= the loop's own (the plan walk between its parts), res/tail= the phases', state/draw/prep/
    // pend= theirs without the parts nested in them; apply= the planned bindings applied, tex= a binding table's texture
    // and sampler entries, retire= the facade's after the encode; per command buffer the events of
    // raw_encode_profile.h EncodeCount (mal/free -1: not counted), applied= per draw, useadd= the uses that reached
    // the job's list
    const int tail = snprintf(
        out + head, size - head,
        " walk=%.0f rself=%.0f tself=%.0f sself=%.0f dself=%.0f pself=%.0f eself=%.0f apply=%.0f tex=%.0f retire=%.0f "
        "pbeg=%.0f "
        "mal=%.1f free=%.1f lk=%.1f snd=%.1f applied=%.2f/d useadd=%.1f/%.1f descb=%.1f descu=%.1f geov=%.1f",
        own(EncodePart::Loop), own(EncodePart::Resources), own(EncodePart::Tail), own(EncodePart::State),
        own(EncodePart::Draw), own(EncodePart::Prepare), own(EncodePart::PassEnd), part(EncodePart::Apply),
        part(EncodePart::Textures), part(EncodePart::Retire), part(EncodePart::PassBegin), mallocs, frees,
        events(raw::EncodeCount::Locks), events(raw::EncodeCount::Sends),
        draws ? events(raw::EncodeCount::Applied) / draws : 0.0, events(raw::EncodeCount::UseAdds),
        events(raw::EncodeCount::Uses), events(raw::EncodeCount::DescriptorBuilds),
        events(raw::EncodeCount::DescriptorUploads), events(raw::EncodeCount::GeometryVertices));
    size_t n = size_t(head) + (tail > 0 ? size_t(tail) : 0);
    // the pooled encoders' blocks made and freed since the process began (raw_object_pool.h): with none freed, made is
    // the most of each kind this process had alive at once
    if (n < size - 1) {
        const int pools = snprintf(out + n, size - n, " encmade=%llu/%llu/%llu encfreed=%llu/%llu/%llu",
                                   (unsigned long long)rawPoolMade([RMRenderEncoder class]),
                                   (unsigned long long)rawPoolMade([RMBlitEncoder class]),
                                   (unsigned long long)rawPoolMade([RMComputeEncoder class]),
                                   (unsigned long long)rawPoolFreed([RMRenderEncoder class]),
                                   (unsigned long long)rawPoolFreed([RMBlitEncoder class]),
                                   (unsigned long long)rawPoolFreed([RMComputeEncoder class]));
        n += pools > 0 ? size_t(pools) : 0;
    }
    return std::min(n, size - 1);
}

size_t raw_metal_fetch_keys(char *out, size_t size)
{
    if (!gRM || !size)
        return 0;
    raw::FetchCounts &k = gRM->raw->stats.fetch;
    size_t used = 0;
    bool any = false;
    auto keys = [&](const char *name, raw::KeyTally &tally) {
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
    keys("rec", k.recordKeys);
    keys("enc", k.encodeKeys);
    keys("sto", k.fallbackKeys);
    keys("ilk", k.interlockKeys);
    keys("gen", k.genericKeys);
    return any ? used : 0;
}

// The stall line (r7p8: the compositor stopped committing for good with nothing in the log): every 5 s, on a timer of its own,
// what the raw path has pending, read without waiting (a lock another thread holds is reported held, not waited for). Once
// nothing was committed or completed over two ticks while work is pending or one of its locks stays held, one line names it,
// once per process. Nothing else: an idle screen has nothing pending and says nothing.
// An untimed shared event wait waits as Metal's GPU would, without end: one line every 10 s for each still waiting after
// 10 s, naming the event, the value it waits for and the value it has
static void rawEventWaitTick()
{
    const uint64_t now = nowUs();
    std::unique_lock<raw::Mutex> h(gHoldersLock, std::try_to_lock);
    if (!h.owns_lock())
        return;
    for (auto &[cb, note] : gEventWaits) {
        if (now - note.sinceUs < 10000000 || now - note.saidUs < 10000000)
            continue;
        const uint64_t reached = [(__bridge id<MTLSharedEvent>)note.event signaledValue];
        if (reached >= note.value)
            continue;
        note.saidUs = now;
        os_log(rawLog(),
               "raw: still waiting on shared event %p for value %llu (it has %llu) for %llu s, queue %{public}s",
               note.event, (unsigned long long)note.value, (unsigned long long)reached,
               (unsigned long long)((now - note.sinceUs) / 1000000), cb->_queue.label.UTF8String ?: "-");
    }
}

void rawStallTick()
{
    rawEventWaitTick();
    static uint64_t lastCommits = ~0ull, lastDone = ~0ull;
    static int still = 0;
    static bool said = false;
    if (said || !gRM)
        return;
    RawState &r = *gRM;
    Stats &s = r.stats;
    const uint64_t commits = s.raw.load() + s.m2v.load() + s.splits.load(), done = s.finished.load();
    const bool moved = commits != lastCommits || done != lastDone;
    lastCommits = commits;
    lastDone = done;
    if (moved) {
        still = 0;
        return;
    }
    const long waiting = long(r.waitingCommitted.load(std::memory_order_relaxed)), inflight = long(r.inflight.size()),
               m2vPending = long(r.m2vPending.load(std::memory_order_relaxed)),
               m2vUnscheduled = long(r.m2vUnscheduled.load(std::memory_order_relaxed));
    bool holders = false;
    {
        std::unique_lock<raw::Mutex> h(gHoldersLock, std::try_to_lock);
        holders = h.owns_lock();
    }
    const bool device = r.raw->lockFree();
    const uint32_t submitted = r.raw->submitted(), completed = r.raw->completed();
    const bool pending = waiting != 0 || inflight != 0 || m2vPending != 0 || m2vUnscheduled != 0 || !holders ||
                         !device || submitted != completed;
    if (!pending) {
        still = 0;
        return;
    }
    if (++still < 2)
        return;
    said = true;
    os_log_error(rawLog(),
                 "raw: stall: nothing committed or completed for 10 s with work pending: waiting %ld, "
                 "inflight %ld, m2v pending %ld unscheduled %ld, locks free: "
                 "holders %d device %d, seqno submitted %u completed %u, commits %llu completed %llu",
                 waiting, inflight, m2vPending, m2vUnscheduled, int(holders), int(device), submitted, completed,
                 (unsigned long long)commits, (unsigned long long)done);
}
#endif

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// The fetch barrier's way, switched from the Mac within one boot (`idevicenotificationproxy -u <udid> post
// com.brunostjohn.switch.debug.fetch-barrier-<ps|psonly|none>`), to measure what the barrier costs: the shipped barrier
// and texture invalidate (ps) against the barrier alone (psonly) and nothing (none). Measurement only: psonly and none
// let a fetching draw read colors still being written. wfi, HW Test's ui.glass_ab, goes back to ps after a minute
// unless another way was posted since
void watchFetchBarrierWay(raw::Device *dev)
{
    using Way = raw::Device::FetchBarrier;
    static std::atomic<uint32_t> posted{0};
    for (const Way way : {Way::PixelShader, Way::PixelShaderOnly, Way::None, Way::WaitForIdle}) {
        char name[96];
        snprintf(name, sizeof name, "com.brunostjohn.switch.debug.fetch-barrier-%s",
                 raw::Device::fetchBarrierName(way));
        int token;
        notify_register_dispatch(name, &token, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
            dev->fetchBarrier.store(way, std::memory_order_relaxed);
            const uint32_t at = posted.fetch_add(1, std::memory_order_relaxed) + 1;
            os_log(rawLog(), "raw: fetch barrier way %{public}s (debug notification)",
                   raw::Device::fetchBarrierName(way));
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

// The keyline probe armed (raw_gpu.h armKeylineProbe) in every process by HW Test's ui.pocket_keyline before it draws
// (`idevicenotificationproxy -u <udid> post com.brunostjohn.switch.debug.keyline-probe` from the Mac)
void watchKeylineProbe()
{
    int token;
    notify_register_dispatch("com.brunostjohn.switch.debug.keyline-probe", &token,
                             dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
                                 raw::armKeylineProbe();
                                 os_log(rawLog(), "raw: keyline probe armed (debug notification)");
                             });
}
#endif
