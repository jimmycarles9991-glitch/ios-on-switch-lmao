// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <IOKit/IOLocks.h>
#include "../TegraGM20B/SwitchGpuSharedEvent.h"
#include "SwitchHost1xResults.h"

template <uint32_t Engine, uint32_t Slots> class SwitchHost1xSemantics {
public:
    struct Node {
        switch_gpu_backing_token token;
        switch_gpu_event_waits *batch;
    };

    using Results = SwitchHost1xResults<Engine>;

private:
    inline static IOLock *lock = nullptr;
    inline static Node nodes[Slots] = {};
    inline static uint32_t syncpoint = 0, pending = 0;
    inline static void (*retired)(switch_gpu_event_waits *batch) = nullptr;

    static int snapshot(const switch_gpu_backing_token *token, switch_gpu_event_waits **out)
    {
        *out = nullptr;
        const int status = Results::status(token);
        if (status)
            return status < 0 ? status : 0;
        IOLockLock(lock);
        for (auto &node : nodes)
            if (node.batch && node.token.value == token->value &&
                node.token.reset_generation == token->reset_generation) {
                *out = node.batch;
                switch_gpu_event_waits_retain(*out);
                break;
            }
        IOLockUnlock(lock);
        return *out ? 1 : 0;
    }

    static int physicalStatus(const switch_gpu_backing_token *token, int)
    {
        return Results::physicalStatus(token);
    }

    static int hasPending()
    {
        return __atomic_load_n(&pending, __ATOMIC_ACQUIRE) != 0;
    }

    static void completed(void *context, int result)
    {
        auto *node = static_cast<Node *>(context);
        IOLockLock(lock);
        auto *batch = node->batch;
        const auto token = node->token;
        Results::semantic(token, result);
        node->batch = nullptr;
        __atomic_sub_fetch(&pending, 1u, __ATOMIC_RELEASE);
        IOLockUnlock(lock);
        switch_gpu_events_completed();
        if (retired)
            retired(batch);
        switch_gpu_event_waits_release(&batch);
    }

public:
    // onRetired: the engine's step for a job whose batch's semantic outcome is known and whose node is free again; it
    // runs in the events' retirement and must not sleep.
    static bool start(uint32_t id, void (*onRetired)(switch_gpu_event_waits *batch))
    {
        if (lock)
            return syncpoint == id;
        if (!Results::start(id))
            return false;
        lock = IOLockAlloc();
        if (!lock)
            return false;
        syncpoint = id;
        retired = onRetired;
        const switch_gpu_backing_semantics adapter = {snapshot, physicalStatus, hasPending};
        if (!switch_gpu_backing_register_semantics(Engine, &adapter))
            return true;
        IOLockFree(lock);
        lock = nullptr;
        return false;
    }

    static bool hasPendingWork()
    {
        return hasPending();
    }

    static int bind(switch_gpu_event_waits *batch, const switch_gpu_backing_token *token, Node **out)
    {
        *out = nullptr;
        if (!batch)
            return 0;
        IOLockLock(lock);
        Node *free = nullptr;
        for (auto &node : nodes)
            if (!node.batch && !free)
                free = &node;
        int error = free ? 0 : -16;
        if (!error)
            error = switch_gpu_event_waits_bind(batch, token);
        if (!error)
            error = switch_gpu_event_waits_completion(batch, completed, free);
        if (!error) {
            free->token = *token;
            free->batch = batch;
            switch_gpu_event_waits_retain(batch);
            __atomic_add_fetch(&pending, 1u, __ATOMIC_RELEASE);
            *out = free;
        }
        IOLockUnlock(lock);
        return error;
    }

    static void commit(Node *)
    {}
};
