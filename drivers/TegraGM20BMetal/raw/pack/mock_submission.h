// SPDX-License-Identifier: MIT
#pragma once

struct mock_admission {
    uint64_t cookie;
    uint32_t count;
    struct TegraGM20BAbiSurfaceUse uses[TegraGM20BMaxJobUses];
};
static struct mock_admission mock_admission[16];
static unsigned long mock_admission_counts[3];

unsigned long mock_iokit_admissions(unsigned kind)
{
    return kind < 3 ? mock_admission_counts[kind] : 0;
}

static kern_return_t mock_call(mach_port_t, uint32_t, const void *, size_t, void *, size_t *);

static kern_return_t mock_submit(mach_port_t connection, const void *input, size_t bytes, void *out, size_t *outsz)
{
    if (!getenv("MOCK_NATIVE_JOBS") || bytes != sizeof(struct TegraGM20BAbiSubmitAddress))
        return kIOReturnBadArgument;
    const struct TegraGM20BAbiSubmitAddress *address = input;
    if (!address->address || address->reserved || address->bytes < sizeof(struct TegraGM20BAbiSubmit))
        return kIOReturnBadArgument;
    const struct TegraGM20BAbiSubmit *job = (const void *)(uintptr_t)address->address;
    const unsigned mask = TegraGM20BSubmitAdmissionBegin | TegraGM20BSubmitAdmissionContinue |
                          TegraGM20BSubmitAdmissionEnd | TegraGM20BSubmitAdmissionCancel;
    const unsigned flags = job->surface_flags & mask;
    const size_t prefix = sizeof(*job) + job->push_count * sizeof(struct TegraGM20BAbiPush);
    const size_t offset = prefix + job->surface_count * sizeof(struct TegraGM20BAbiSurfaceUse);
    if (job->channel >= 16 || job->push_count > TegraGM20BMaxJobPushes ||
        job->surface_count > TegraGM20BMaxJobUses ||
        address->bytes != offset + (flags ? sizeof(struct TegraGM20BAbiSubmitAdmission) : 0))
        return kIOReturnBadArgument;
    const struct TegraGM20BAbiSurfaceUse *uses = (const void *)((const char *)job + prefix);
    const struct TegraGM20BAbiSubmitAdmission *lease = (const void *)((const char *)job + offset);
    struct mock_admission *held = &mock_admission[job->channel];
    if (flags) {
        if (!lease->cookie || lease->reserved)
            return kIOReturnBadArgument;
        if (flags == TegraGM20BSubmitAdmissionCancel) {
            if (job->push_count || job->surface_count || held->cookie != lease->cookie)
                return kIOReturnBadArgument;
            *held = (struct mock_admission){0};
            ++mock_admission_counts[2];
            *(struct TegraGM20BAbiExecReply *)out = (struct TegraGM20BAbiExecReply){submitted[job->channel], 0};
            return KERN_SUCCESS;
        }
        const unsigned phase = flags & ~TegraGM20BSubmitAdmissionEnd;
        if (phase == TegraGM20BSubmitAdmissionBegin) {
            if (held->cookie)
                return kIOReturnCannotLock;
        } else if (phase == TegraGM20BSubmitAdmissionContinue) {
            if (held->cookie != lease->cookie || held->count != job->surface_count ||
                memcmp(held->uses, uses, job->surface_count * sizeof(*uses)))
                return kIOReturnBadArgument;
        } else {
            return kIOReturnBadArgument;
        }
    } else if (held->cookie) {
        return kIOReturnCannotLock;
    }
    const size_t execBytes = offsetof(struct TegraGM20BAbiExec, pushes) +
                             job->push_count * sizeof(struct TegraGM20BAbiPush);
    struct TegraGM20BAbiExec *exec = calloc(1, execBytes);
    if (!exec)
        return kIOReturnNoMemory;
    exec->channel = job->channel;
    exec->count = job->push_count;
    memcpy(exec->pushes, job + 1, job->push_count * sizeof(struct TegraGM20BAbiPush));
    const kern_return_t result = mock_call(connection, TegraGM20BMethodExec, exec, execBytes, out, outsz);
    free(exec);
    if (result == KERN_SUCCESS && flags) {
        if (flags & TegraGM20BSubmitAdmissionBegin) {
            held->cookie = lease->cookie;
            held->count = job->surface_count;
            memcpy(held->uses, uses, job->surface_count * sizeof(*uses));
            ++mock_admission_counts[0];
        }
        if (flags & TegraGM20BSubmitAdmissionEnd) {
            *held = (struct mock_admission){0};
            ++mock_admission_counts[1];
        }
    }
    return result;
}
