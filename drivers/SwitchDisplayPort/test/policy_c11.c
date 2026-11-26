// SPDX-License-Identifier: GPL-2.0-only
// The policy headers as C11 with -Wconversion -Wsign-conversion: they are included by C++ in the kernel, and kept C so
// that both languages see one definition.
#include "../SwitchDPAux.h"
#include "../SwitchDPCEC.h"
#include "../SwitchDPAppleTiming.h"
#include "../SwitchDPLink.h"
#include "../SwitchDPModes.h"
#include "../SwitchDPTraining.h"
#include "../SwitchPDAltModeSteps.h"

int switch_dp_policy_c11(void);

int switch_dp_policy_c11(void)
{
    struct switch_pd_alt alt;
    switch_pd_alt_init(&alt, true);
    const struct switch_pd_alt_in in = {0, 0, 0, false, true, false, false};
    return (int)switch_pd_alt_event(&alt, &in).act;
}
