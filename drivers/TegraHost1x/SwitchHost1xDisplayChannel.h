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
    IOLockLock(gChannelLock);
    int status = stream->status ? stream->status : failure;
    for (uint32_t i = 0; i < stream->count && !status; ++i) {
        const int result = displayProducerStatus(stream->producers[i]);
        if (result < 0)
            status = result;
    }
    auto *completed = stream->completed;
    void *context = stream->context;
    stream->queued = false;
    if (gDisplayStream == stream)
        gDisplayStream = nullptr;
    IOLockUnlock(gChannelLock);
    completed(context, status);
}

int displayCancelLocked(uint32_t allocated, bool force = false)
{
    auto *stream = gDisplayStream;
    if (!stream || stream->status ||
        static_cast<int32_t>(tegra_host1x_syncpt_read(stream->syncpt) - stream->fence.threshold) >= 0)
        return 0;
    if (!force) {
        bool affected = false;
        for (uint32_t i = 0; i < stream->count && !affected; ++i)
            affected = switch_gpu_backing_token_physical_status(&stream->producers[i], 0) < 0;
        if (!affected)
            return 0;
    }
    teardownLocked(TEGRA_HOST1X_CHANNEL_DISPLAY);
    stream->restore = force;
    __atomic_store_n(&stream->status, SwitchHost1xDisplayStream::kCancelled, __ATOMIC_RELEASE);
    restartLocked(TEGRA_HOST1X_CHANNEL_DISPLAY);

    struct Registers {
        uint32_t read(uint32_t offset)
        {
            return syncRead(offset);
        }

        void write(uint32_t offset, uint32_t value)
        {
            syncSet(offset, value);
        }

        void barrier()
        {
            __asm__ volatile("dsb sy" ::: "memory");
        }
    } registers;

    return switch_host1x_release_failed(registers, stream->syncpt, stream->fence.threshold, 1, allocated);
}

}  // namespace

int switch_host1x_channels_cancel_display(uint32_t allocated)
{
    return displayCancelLocked(allocated);
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
                                 uint32_t update, uint32_t request, void (*completed)(void *, int), void *context)
{
    if (!stream || !stream->syncpt || !producers || !count || count > SwitchHost1xDisplayStream::kProducers ||
        !registers || !values || !stores || stores > 4 || !completed || !gChannelLock)
        return TEGRA_HOST1X_EINVAL;
    IOLockLock(gChannelLock);
    int error = !gRunning ? TEGRA_HOST1X_ENODEV : gDisplayStream ? TEGRA_HOST1X_EAGAIN : 0;
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
    Channel &channel = gChannels[TEGRA_HOST1X_CHANNEL_DISPLAY];
    const uint32_t capacity = count * SWITCH_HOST1X_WAIT_WORDS + 1 + 2 * (stores + 2) + 1;
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
        stream->completed = completed;
        stream->context = context;
        ++stream->generation;
        switch_host1x_fence_init(&stream->fence, stream->syncpt, tegra_host1x_syncpt_read(stream->syncpt) + 1,
                                 displaySignaled);
        error = switch_host1x_fence_add(&stream->fence, 0);
        if (!error) {
            stream->queued = true;
            gDisplayStream = stream;
            error = gatherLocked(TEGRA_HOST1X_CHANNEL_DISPLAY, words, n, true, stream->syncpt, stream->fence.threshold,
                                 false);
            if (error) {
                switch_host1x_fence_remove(&stream->fence);
                stream->queued = false;
                gDisplayStream = nullptr;
            }
        } else {
            endReservation(channel);
        }
    }
    IOLockUnlock(gChannelLock);
    return error;
}

int switch_host1x_display_cancel(SwitchHost1xDisplayStream *stream)
{
    if (!stream || !gChannelLock)
        return TEGRA_HOST1X_EINVAL;
    IOLockLock(gChannelLock);
    if (gDisplayStream == stream)
        displayCancelLocked(stream->syncpt + 1, true);
    const int status = stream->status;
    IOLockUnlock(gChannelLock);
    return status;
}
