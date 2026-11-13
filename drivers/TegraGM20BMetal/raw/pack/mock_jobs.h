// SPDX-License-Identifier: MIT
// The job interface (TegraGM20BJobABI.h, methods 31-33) over the mock GPU, in the host harnesses' mock only
// (SWITCH_MOCK_JOB_INTERFACE): the raw stack key hashes the pack's mock and not that header. A resource set's members decide
// residency and ownership, which the mock does not model, so a set is its id and generation, and a job runs as Exec of
// its pushes.
#pragma once
#include "TegraGM20BJobABI.h"

static struct {
    uint32_t live, generation;
} mock_sets[TegraGM20BMaxResourceSets];

static kern_return_t mock_set_create(const void *input, size_t bytes, void *out)
{
    const struct TegraGM20BAbiResourceSetCreate *in = input;
    if (bytes != sizeof *in || !in->count || in->count > TegraGM20BMaxSetMembers || !in->uses_address ||
        in->set > TegraGM20BMaxResourceSets)
        return kIOReturnBadArgument;
    uint32_t index = in->set ? in->set - 1 : 0;
    if (in->set && !mock_sets[index].live)
        return kIOReturnBadArgument;
    while (!in->set && index < TegraGM20BMaxResourceSets && mock_sets[index].live)
        index++;
    if (index == TegraGM20BMaxResourceSets)
        return kIOReturnNoResources;
    mock_sets[index].live = 1;
    const uint32_t word = TegraGM20BSetGenerationWord + index;
    fence_page()[word] = ++mock_sets[index].generation;
    const struct TegraGM20BAbiResourceSetCreateReply reply = {index + 1, mock_sets[index].generation, kFenceToken,
                                                              word * 4};
    memcpy(out, &reply, sizeof reply);
    return KERN_SUCCESS;
}

static kern_return_t mock_set_release(const void *input, size_t bytes)
{
    const struct TegraGM20BAbiResourceSetRelease *in = input;
    if (bytes != sizeof *in || in->reserved || !in->set || in->set > TegraGM20BMaxResourceSets ||
        !mock_sets[in->set - 1].live)
        return kIOReturnBadArgument;
    mock_sets[in->set - 1].live = 0;
    return KERN_SUCCESS;
}

static kern_return_t mock_job(mach_port_t connection, const void *input, size_t bytes, void *out, size_t *outsz)
{
    const struct TegraGM20BAbiSubmitAddress *address = input;
    if (bytes != sizeof *address || !address->address || address->reserved ||
        address->bytes < sizeof(struct TegraGM20BAbiJob))
        return kIOReturnBadArgument;
    const struct TegraGM20BAbiJob *job = (const void *)(uintptr_t)address->address;
    if (job->channel >= 16 || job->push_count > TegraGM20BMaxJobPushes || job->shared_count > TegraGM20BMaxJobUses ||
        job->set > TegraGM20BMaxResourceSets || !job->set != !job->set_generation ||
        address->bytes < sizeof *job + job->push_count * sizeof(struct TegraGM20BAbiPush) +
                             job->shared_count * sizeof(struct TegraGM20BAbiSurfaceUse))
        return kIOReturnBadArgument;
    if (job->set && !mock_sets[job->set - 1].live)
        return kIOReturnBadArgument;
    if (job->set && mock_sets[job->set - 1].generation != job->set_generation)
        return kIOReturnNotReady;
    const size_t execBytes =
        offsetof(struct TegraGM20BAbiExec, pushes) + job->push_count * sizeof(struct TegraGM20BAbiPush);
    struct TegraGM20BAbiExec *exec = calloc(1, execBytes);
    if (!exec)
        return kIOReturnNoMemory;
    exec->channel = job->channel;
    exec->count = job->push_count;
    memcpy(exec->pushes, job + 1, job->push_count * sizeof(struct TegraGM20BAbiPush));
    const kern_return_t result = mock_call(connection, TegraGM20BMethodExec, exec, execBytes, out, outsz);
    free(exec);
    return result;
}
