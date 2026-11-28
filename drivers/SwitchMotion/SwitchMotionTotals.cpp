// SPDX-License-Identifier: BSD-2-Clause-Patent
// Debug kernels: counts HW Test's sensor.device_motion reads around its run (apps/hwtest/tests/SensorTests.swift). They
// are sysctls because its app sandbox reads only the kern.* names its entitlements list, and drops a registry entry's
// properties it does not know:
//  - kern.switch_motion_reports: the device motion reports SwitchMotion fused and sent since boot;
//  - kern.switch_motion_fastpath_copies: the fast path copies that answered one (SwitchHIDFastPath.cpp);
//  - kern.switch_motion_gyro_us: the raw gyroscope's client interval, 0 while nobody streams it (nobody fuses on the CPU).
#include "SwitchMotion.h"

#if DEVELOPMENT || DEBUG
#include <sys/sysctl.h>
#include "../../kits/SwitchSysctl/SwitchSysctl.h"
#include "../SwitchTouch/SwitchHID.h"

namespace {
enum : int {
    kReports,
    kCopies,
    kGyro,
    kTotals
};
struct sysctl_oid gOids[kTotals];

int readTotal(struct sysctl_oid *, void *, int which, struct sysctl_req *req)
{
    uint64_t reports = 0;
    uint32_t gyroUS = 0;
    switch_motion_totals(&reports, &gyroUS);
    const uint64_t totals[kTotals] = {reports, switch_hid_fastpath_copies(), gyroUS};
    const uint64_t value = totals[static_cast<unsigned>(which) % kTotals];
    return SYSCTL_OUT(req, &value, sizeof(value));
}
}  // namespace

void switch_motion_totals_register()
{
    static const char *const names[kTotals] = {"switch_motion_reports", "switch_motion_fastpath_copies",
                                               "switch_motion_gyro_us"};
    for (int i = 0; i < kTotals; ++i) {
        if (gOids[i].oid_name)
            continue;
        switch_sysctl_oid(&gOids[i], &sysctl__kern_children, names[i],
                          static_cast<int>(CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED), readTotal, "Q",
                          "since boot: device motion reports sent, fast path copies answered; the raw gyroscope's "
                          "client interval (us)");
        gOids[i].oid_arg2 = i;
        sysctl_register_oid(&gOids[i]);
    }
}
#else
void switch_motion_totals_register()
{}
#endif
