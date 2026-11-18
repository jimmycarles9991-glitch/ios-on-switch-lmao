// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <array>
#include <errno.h>
#include <sys/types.h>

struct SwitchRespringService {
    const char *executable;
    const char *job;
    pid_t pid = 0;
};

using SwitchRespringServices = std::array<SwitchRespringService, 2>;

struct SwitchRespringOperations {
    int (*watch)(int, SwitchRespringServices &);
    bool (*frontboard)();
    int (*wait)(int, SwitchRespringServices &, unsigned);
    int (*kickstart)(const char *);
};

static inline int switch_respring(int queue, SwitchRespringServices &services, const SwitchRespringOperations &ops)
{
    if (!services[1].pid)
        return ENOENT;
    const int watch = ops.watch(queue, services);
    if (watch || (!services[0].pid && !services[1].pid))
        return watch;
    if (ops.frontboard()) {
        const int result = ops.wait(queue, services, 3);
        if (result != ETIMEDOUT)
            return result;
    }
    for (const auto &service : services) {
        const int observed = ops.wait(queue, services, 0);
        if (observed != ETIMEDOUT)
            return observed;
        if (service.pid) {
            const int error = ops.kickstart(service.job);
            if (error)
                return error;
        }
    }
    return ops.wait(queue, services, 5);
}
