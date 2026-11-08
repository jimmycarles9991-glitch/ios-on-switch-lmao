// SPDX-License-Identifier: MIT
#import "RawMetalInternal.h"

static uint64_t gPredRates[PredCount];  // the rates line's last gPredCbs (its timer thread only)

// ---- the rates line ----
static struct {
    uint64_t raw, m2v, encodeUs, draws, passes, copies, deferred, aborted, reasons[WhyCount], ringWaits, lastUs,
        switches, cpuOrdered, acquires, poolStalls, eventsReleased, heldReturns, scheduledWaits, scheduledWaitUs,
        completedWaits, completedWaitUs, abortUs, replayUs, replayClean, rtUnaligned, rtMixed, predOnly[PredCount],
        splits, splitM2v, levelPasses;
} gRates;

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
    const uint64_t aborts = s.aborted.load() - gRates.aborted, segs = s.splitM2v.load() - gRates.splitM2v,
                   m2vs = f + segs ? f + segs : 1;
    const raw::Compiler::Stats made = gRM->completion ? gRM->completion->stats() : raw::Compiler::Stats();
    char diag[1024];
    snprintf(diag, sizeof diag,
             " sp=%llu/%llu abortenc=%.0fus/a replay=%.0fus/m rclean=%.0fKB/m rt128=%llu(pitch %u va&255 %u) "
             "rtmix=%llu lvl=%llu states=%llu/%llu/%llu/%llu/%.0fms atcreate=%llu/%.0fus comp=%llu/%lluKB/f%llu "
             "zbc=%llu/%llu why=%s",
             (unsigned long long)sp, (unsigned long long)segs,
             (s.abortUs.load() - gRates.abortUs) / double(aborts ? aborts : 1),
             (s.replayUs.load() - gRates.replayUs) / double(m2vs),
             (s.replayClean.load() - gRates.replayClean) / 1024.0 / double(m2vs),
             (unsigned long long)(k.rtUnaligned.load() - gRates.rtUnaligned), k.rtUnalignedPitch.load(),
             k.rtUnalignedVa.load(), (unsigned long long)(k.rtMixed.load() - gRates.rtMixed),
             (unsigned long long)(k.levelPasses.load() - gRates.levelPasses), (unsigned long long)made.hits,
             (unsigned long long)made.misses, (unsigned long long)made.refusals, (unsigned long long)made.failures,
             made.makeUs / 1000.0, (unsigned long long)made.lookupHits,
             made.lookupHits ? double(made.lookupUs) / double(made.lookupHits) : 0.0,
             (unsigned long long)k.compBackings.load(), (unsigned long long)(k.compBytes.load() >> 10),
             (unsigned long long)k.compFailed.load(), (unsigned long long)k.zbcHits.load(),
             (unsigned long long)k.zbcMisses.load(), *preds ? preds : "-");
#if SWITCH_FRAME_TIMELINE
    {
        const size_t used = strlen(diag);
        snprintf(diag + used, sizeof diag - used,
                 " rsplit=%.0f/%.0f/%.0fus/m rmax=%lluus rdraw=%.1f/m tw=t%llu/%lluus/b%llu/o%llu/f%llu sbf=%llu"
                 " fb=%llu/%llu/%llu/%llu(%s) clr=%llu/%llu so=%llu/%llu/%llu/%lluus",
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
                 (unsigned long long)k.selfOverlapSplit.exchange(0), (unsigned long long)k.selfOverlapRuns.exchange(0),
                 (unsigned long long)k.selfOverlapUnbounded.exchange(0),
                 (unsigned long long)(k.selfOverlapNs.exchange(0) / 1000));
    }
#endif
    gRates.abortUs = s.abortUs.load();
    gRates.splits = s.splits.load();
    gRates.splitM2v = s.splitM2v.load();
    gRates.replayUs = s.replayUs.load();
    gRates.replayClean = s.replayClean.load();
    gRates.rtUnaligned = k.rtUnaligned.load();
    gRates.rtMixed = k.rtMixed.load();
    gRates.levelPasses = k.levelPasses.load();
    const int n = snprintf(
        out, size,
        "raw %.0fs c=%llu(%.1f/s) m2v=%llu%s%s%s sw=%llu cpu=%llu acq=%llu enc=%.0fus/c d=%.1f p=%.1f b=%.2f "
        "defer=%llu abort=%llu rw=%llu pool=%lluMB/%llu grow=%llu imp=%llu/%lluMB clean=%.1fKB/c ce=%llu/%.0fKB "
        "ev=%llu/%llu ws=%llu/%lluus wc=%llu/%lluus%s",
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
        (unsigned long long)(s.scheduledWaits.load() - gRates.scheduledWaits),
        (unsigned long long)(s.scheduledWaitUs.load() - gRates.scheduledWaitUs),
        (unsigned long long)(s.completedWaits.load() - gRates.completedWaits),
        (unsigned long long)(s.completedWaitUs.load() - gRates.completedWaitUs), diag);
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
    gRates.scheduledWaits = s.scheduledWaits.load();
    gRates.scheduledWaitUs = s.scheduledWaitUs.load();
    gRates.completedWaits = s.completedWaits.load();
    gRates.completedWaitUs = s.completedWaitUs.load();
    gRates.lastUs = now;
    return n > 0 ? std::min(size_t(n), size - 1) : 0;
}

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// The stall line (r7p8: the compositor stopped committing for good with nothing in the log): every 5 s, on a timer of its own,
// what the raw path has pending, read without waiting (a lock another thread holds is reported held, not waited for). Once
// nothing was committed or completed over two ticks while work is pending or one of its locks stays held, one line names it,
// once per process. Nothing else: an idle screen has nothing pending and says nothing.
void rawStallTick()
{
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
    long waiting = -1, inflight = -1, m2vPending = -1, m2vUnscheduled = -1;
    unsigned segNext = 0, segs = 0, reason = 0;
    {
        std::unique_lock<std::mutex> a(r.submit, std::try_to_lock);
        if (a.owns_lock()) {
            waiting = long(r.waiting.size());
            if (!r.waiting.empty()) {
                RMCommandBuffer *f = r.waiting.front();
                segNext = unsigned(f->_segNext);
                segs = unsigned(f->_segs.size());
                reason = unsigned(f->_reason);
            }
        }
    }
    {
        std::unique_lock<std::mutex> b(r.doneLock, std::try_to_lock);
        if (b.owns_lock())
            inflight = long(r.inflight.size());
    }
    {
        std::unique_lock<std::mutex> m(r.m2vLock, std::try_to_lock);
        if (m.owns_lock()) {
            m2vPending = long(r.m2vPending);
            m2vUnscheduled = long(r.m2vUnscheduled);
        }
    }
    bool holders = false;
    {
        std::unique_lock<std::mutex> h(gHoldersLock, std::try_to_lock);
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
                 "raw: stall: nothing committed or completed for 10 s with work pending: waiting %ld (front segment "
                 "%u/%u reason %u), inflight %ld, m2v pending %ld unscheduled %ld (-1: its lock held), locks free: "
                 "holders %d device %d, seqno submitted %u completed %u, commits %llu completed %llu",
                 waiting, segNext, segs, reason, inflight, m2vPending, m2vUnscheduled, int(holders), int(device),
                 submitted, completed, (unsigned long long)commits, (unsigned long long)done);
}
#endif

#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
// The fetch barrier's way, switched from the Mac within one boot (`idevicenotificationproxy -u <udid> post
// com.brunostjohn.switch.debug.fetch-barrier-<ps|psonly|none>`), to measure what the barrier costs: the shipped barrier and
// texture invalidate (ps) against the barrier alone (psonly) and nothing (none). Measurement only: psonly and none let a
// fetching draw read colors still being written
void watchFetchBarrierWay(raw::Device *dev)
{
    using Way = raw::Device::FetchBarrier;
    for (const Way way : {Way::PixelShader, Way::PixelShaderOnly, Way::None}) {
        char name[96];
        snprintf(name, sizeof name, "com.brunostjohn.switch.debug.fetch-barrier-%s",
                 raw::Device::fetchBarrierName(way));
        int token;
        notify_register_dispatch(name, &token, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
            dev->fetchBarrier.store(way, std::memory_order_relaxed);
            os_log(rawLog(), "raw: fetch barrier way %{public}s (debug notification)",
                   raw::Device::fetchBarrierName(way));
        });
    }
}
#endif
