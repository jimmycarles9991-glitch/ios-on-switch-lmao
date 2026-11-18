// SPDX-License-Identifier: GPL-2.0-only
#include <sys/errno.h>
#include <sys/systm.h>
#include <switch/bootstrap.h>

#if defined(SWITCH_IPSW_KIND_21H461)
#include <switch/bootstrap_policy_21H461.h>
#elif defined(SWITCH_IPSW_KIND_24B119)
#include <switch/bootstrap_policy_24B119.h>
#else
#error "Bootstrap dyld policy requires the selected IPSW's ABI"
#endif

int switch_bootstrap_dyld_result(struct proc *p, const char *policy, int call, uint64_t argument, int result)
{
    if (result || !switch_bootstrap_process(p) || strcmp(policy, "AMFI") ||
        call != SWITCH_BOOTSTRAP_AMFI_DYLD_CALL) return result;
    struct {
        uint64_t input;
        user_addr_t output;
    } request;
    if (copyin(argument, &request, sizeof(request))) return EFAULT;
    uint64_t flags = 0;
    if (copyin(request.output, &flags, sizeof(flags))) return EFAULT;
    flags |= SWITCH_BOOTSTRAP_AMFI_DYLD_FLAGS;
    return copyout(&flags, request.output, sizeof(flags)) ? EFAULT : 0;
}
