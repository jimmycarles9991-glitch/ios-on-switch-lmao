// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <IOKit/IOLib.h>
#include "../TegraGM20B/SwitchGpuBridge.h"

class SwitchHost1xDependencies {
    static constexpr uint32_t kInline = 32;
    switch_gpu_backing_token inlineTokens[kInline] = {};
    uint32_t retained = 0;
    switch_gpu_bridge *bridge = nullptr;

public:
    switch_gpu_backing_token *tokens = inlineTokens;
    uint32_t capacity = kInline;

    SwitchHost1xDependencies() = default;
    SwitchHost1xDependencies(const SwitchHost1xDependencies &) = delete;
    SwitchHost1xDependencies &operator=(const SwitchHost1xDependencies &) = delete;

    ~SwitchHost1xDependencies()
    {
        dropTokens();
        switch_gpu_bridge_release(&bridge);
        if (tokens != inlineTokens)
            IODelete(tokens, switch_gpu_backing_token, capacity);
    }

    int grow(uint32_t needed)
    {
        if (needed > SWITCH_GPU_BRIDGE_MAX_TOKENS)
            return -28;
        if (needed <= capacity)
            return 0;
        auto *replacement = IONew(switch_gpu_backing_token, needed);
        if (!replacement)
            return -12;
        if (tokens != inlineTokens)
            IODelete(tokens, switch_gpu_backing_token, capacity);
        tokens = replacement;
        capacity = needed;
        return 0;
    }

    int covered(const switch_gpu_backing_token &token) const
    {
        return bridge ? switch_gpu_bridge_covers(bridge, &token) : 0;
    }

    int holdGpu(uint32_t count)
    {
        retained = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (tokens[i].engine != SWITCH_GPU_ENGINE_GM20B && tokens[i].engine != SWITCH_GPU_ENGINE_PROMISE)
                continue;
            const auto token = tokens[i];
            const int error = switch_gpu_backing_token_retain(&token);
            if (error) {
                dropTokens();
                return error;
            }
            tokens[retained++] = token;
        }
        return retained ? 0 : -22;
    }

    int holdAll(uint32_t count)
    {
        retained = 0;
        for (; retained < count; ++retained) {
            const int error = switch_gpu_backing_token_retain(&tokens[retained]);
            if (error) {
                dropTokens();
                return error;
            }
        }
        return 0;
    }

    void releaseTokens()
    {
        dropTokens();
    }

    int submitBridge()
    {
        switch_gpu_bridge_release(&bridge);
        int error;
        for (;;) {
            error = switch_gpu_bridge_submit(tokens, retained, &bridge);
            if (error != -16)
                break;
            error = switch_gpu_bridge_wait_room();
            if (error)
                break;
        }
        dropTokens();
        return error;
    }

    bool acquire(uint32_t *syncpoint, uint32_t *threshold) const
    {
        if (!bridge)
            return false;
        switch_gpu_bridge_syncpoint(bridge, syncpoint, threshold);
        return true;
    }

    switch_gpu_bridge *takeBridge()
    {
        auto *result = bridge;
        bridge = nullptr;
        return result;
    }

private:
    void dropTokens()
    {
        while (retained)
            switch_gpu_backing_token_release(&tokens[--retained]);
    }
};
