// SPDX-License-Identifier: BSD-2-Clause-Patent
// The display stream on SwitchHost1xChannels.cpp's channel and submit lock.
namespace {
int displayProducerStatus(const switch_gpu_backing_token &token)
{
    const int physical = switch_gpu_backing_token_physical_status(&token, 0);
    return physical < 0 ? physical : switch_gpu_backing_token_status(&token);
}

void displaySignaled(switch_host1x_fence *fence, int failure)
{
    auto *stream = reinterpret_cast<SwitchHost1xDisplayStream *>(fence);
    IOLock *lock = gChannels[TEGRA_HOST1X_CHANNEL_DISPLAY].lock;
    IOLockLock(lock);
    int status = stream->status ? stream->status : failure;
    for (uint32_t i = 0; i < stream->count && !status; ++i) {
        const int result = displayProducerStatus(stream->producers[i]);
        if (result < 0)
            status = result;
    }
    auto *completed = stream->completed;
    void *context = stream->context;
    switch_gpu_bridge *bridge = stream->bridge;
    stream->bridge = nullptr;
    if (gDisplayStream == stream)
        gDisplayStream = nullptr;
    IOLockUnlock(lock);
    if (bridge) {
        // The bridge may hold the producers' last reference: their reason is read before it goes.
        const int produced = switch_gpu_bridge_status(bridge);
        const int why = status || produced < 0 ? switch_gpu_bridge_why(bridge) : 0;
        stream->bridgeWhy = why;
        if (why < 0)
            status = why;
        else if (!status && produced < 0)
            status = produced;
        switch_gpu_bridge_release(&bridge);
    }
    completed(context, status);
}

// released: a syncpoint whose producer failed; the stream is affected when it waits on it, directly or through its bridge.
// A jobless bridge's syncpoint is its producer's, shared by every slot on it: only a stream whose own threshold the GPU
// has not reached waits on this release (read before releaseLocked's CPU increments), as VIC's and NVDEC's
// producerCancelled match.
void displayCancelLocked(bool force = false, uint32_t released = 0)
{
    auto *stream = gDisplayStream;
    if (!stream || stream->status ||
        static_cast<int32_t>(tegra_host1x_syncpt_read(stream->syncpt) - stream->fence.threshold) >= 0)
        return;
    if (!force) {
        bool affected = stream->bridge && released && stream->bridgeSyncpt == released &&
                        static_cast<int32_t>(tegra_host1x_syncpt_read(released) - stream->bridgeThreshold) < 0;
        for (uint32_t i = 0; i < stream->count && !affected; ++i)
            affected = switch_gpu_backing_token_physical_status(&stream->producers[i], 0) < 0;
        if (!affected)
            return;
    }
    teardownLocked(TEGRA_HOST1X_CHANNEL_DISPLAY);
    stream->restore = force;
    __atomic_store_n(&stream->status, SwitchHost1xDisplayStream::kCancelled, __ATOMIC_RELEASE);
    restartLocked(TEGRA_HOST1X_CHANNEL_DISPLAY);
    releaseLocked(stream->syncpt, stream->fence.threshold);
}

}  // namespace

#define SWITCH_HOST1X_DISPLAY_CHANNEL_DEFINED 1

void switch_host1x_display_channel_stopped_locked()
{
    const auto *stream = gDisplayStream;
    if (!stream || stream->status)
        return;
    const uint32_t at = tegra_host1x_syncpt_read(stream->syncpt);
    if (static_cast<int32_t>(at - stream->fence.threshold) >= 0)
        return;
    char host[200];
    tegra_host1x_describe_channel(TEGRA_HOST1X_CHANNEL_DISPLAY, host, sizeof(host));
    panic("SwitchDisplay: host1x stopped windows B and C's stream on an illegal opcode (CMDPROC_STAT %#x): an accepted "
          "swap cannot present; syncpoint %u at %u of %u, bridge %u at %u of %u, %u engine producers; %s",
          syncRead(SYNC_CMDPROC_STAT), stream->syncpt, at, stream->fence.threshold, stream->bridgeSyncpt,
          stream->bridgeSyncpt ? tegra_host1x_syncpt_read(stream->bridgeSyncpt) : 0u, stream->bridgeThreshold,
          stream->count, host);
}

void switch_host1x_channels_release_failed(uint32_t id, uint32_t threshold)
{
    IOLock *lock = gChannels[TEGRA_HOST1X_CHANNEL_DISPLAY].lock;
    IOLockLock(lock);
    displayCancelLocked(false, id);
    releaseLocked(id, threshold);
    IOLockUnlock(lock);
}

int switch_host1x_display_start(SwitchHost1xDisplayStream *stream)
{
    if (!stream)
        return TEGRA_HOST1X_EINVAL;
    if (stream->syncpt)
        return 0;
    const int started = tegra_host1x_start();
    return started ? started : tegra_host1x_syncpt_alloc(&stream->syncpt);
}

int switch_host1x_display_submit(SwitchHost1xDisplayStream *stream, const switch_gpu_backing_token *producers,
                                 uint32_t count, const uint32_t *registers, const uint32_t *values, uint32_t stores,
                                 uint32_t update, uint32_t request, void (*completed)(void *, int), void *context,
                                 switch_gpu_bridge *bridge)
{
    if (!stream || !stream->syncpt || (count && !producers) || (!count && !bridge) ||
        count > SwitchHost1xDisplayStream::kProducers || !registers || !values || !stores || stores > 4 || !completed ||
        !validChannel(TEGRA_HOST1X_CHANNEL_DISPLAY))
        return TEGRA_HOST1X_EINVAL;
    uint32_t bridgeSyncpt = 0, bridgeThreshold = 0;
    if (bridge)
        switch_gpu_bridge_syncpoint(bridge, &bridgeSyncpt, &bridgeThreshold);
    Channel &channel = gChannels[TEGRA_HOST1X_CHANNEL_DISPLAY];
    IOLockLock(channel.lock);
    int error = !__atomic_load_n(&gRunning, __ATOMIC_ACQUIRE) ? TEGRA_HOST1X_ENODEV
                : gDisplayStream                              ? TEGRA_HOST1X_EAGAIN
                                                              : 0;
    // In the hold that publishes the stream: a failure is stored before the release that would cancel the stream, and
    // that release takes this lock, so it either finds the stream published or its failure is read here. The stored
    // failure trails a producer's own by up to one retire pass, so the producers are read too (their timeline's lock is
    // a leaf, as VIC's and NVDEC's are below).
    if (!error && bridge)
        error = switch_gpu_bridge_failed(bridge);
    if (!error && bridge) {
        const int source = switch_gpu_bridge_source_failure(bridge);
        if (source < 0)
            error = source;
    }
    for (uint32_t i = 0; i < count && !error; ++i) {
        const auto &token = producers[i];
        if (token.engine != SWITCH_GPU_ENGINE_VIC && token.engine != SWITCH_GPU_ENGINE_NVDEC) {
            error = TEGRA_HOST1X_EINVAL;
        } else {
            const int result = displayProducerStatus(token);
            if (result < 0)
                error = result;
        }
    }
    const uint32_t capacity = (count + (bridge ? 1 : 0)) * SWITCH_HOST1X_WAIT_WORDS + 1 + 2 * (stores + 2) + 1;
    uint32_t at = SwitchHost1xArena::kNone;
    if (!error) {
        reclaim(channel);
        at = channel.arena.reserve(capacity);
        if (at == SwitchHost1xArena::kNone)
            error = TEGRA_HOST1X_EAGAIN;
    }
    if (!error) {
        uint32_t *words = arenaWords(channel) + at;
        uint32_t n = 0;
        if (bridge)
            n += tegra_host1x_op_wait_syncpt(words + n, bridgeSyncpt, bridgeThreshold);
        for (uint32_t i = 0; i < count; ++i)
            n += tegra_host1x_op_wait_syncpt(words + n, producers[i].queue, uint32_t(producers[i].value));
        words[n++] = tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_DC, 0, 0);
        for (uint32_t i = 0; i < stores; ++i) {
            words[n++] = tegra_host1x_op_incr(registers[i], 1);
            words[n++] = values[i];
        }
        words[n++] = tegra_host1x_op_incr(0x041, 1);
        words[n++] = update;
        words[n++] = tegra_host1x_op_incr(0x041, 1);
        words[n++] = request;
        words[n++] = tegra_host1x_op_imm(0, stream->syncpt);
        stream->status = 0;
        stream->restore = false;
        stream->count = count;
        for (uint32_t i = 0; i < count; ++i)
            stream->producers[i] = producers[i];
        stream->bridge = bridge;
        stream->bridgeSyncpt = bridgeSyncpt;
        stream->bridgeThreshold = bridgeThreshold;
        stream->bridgeWhy = 0;
        stream->completed = completed;
        stream->context = context;
        switch_host1x_fence_init(&stream->fence, stream->syncpt, tegra_host1x_syncpt_read(stream->syncpt) + 1,
                                 displaySignaled);
        error = switch_host1x_fence_add(&stream->fence, 0);
        if (!error) {
            gDisplayStream = stream;
            error = gatherLocked(TEGRA_HOST1X_CHANNEL_DISPLAY, words, n, true, stream->syncpt, stream->fence.threshold,
                                 false);
            if (error) {
                switch_host1x_fence_remove(&stream->fence);
                gDisplayStream = nullptr;
                stream->bridge = nullptr;
            }
        } else {
            stream->bridge = nullptr;
            endReservation(channel);
        }
    }
    IOLockUnlock(channel.lock);
    return error;
}

int switch_host1x_display_cancel(SwitchHost1xDisplayStream *stream)
{
    if (!stream || !validChannel(TEGRA_HOST1X_CHANNEL_DISPLAY))
        return TEGRA_HOST1X_EINVAL;
    IOLock *lock = gChannels[TEGRA_HOST1X_CHANNEL_DISPLAY].lock;
    IOLockLock(lock);
    if (gDisplayStream == stream)
        displayCancelLocked(true);
    const int status = stream->status;
    IOLockUnlock(lock);
    return status;
}
