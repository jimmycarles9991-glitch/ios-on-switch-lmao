// SPDX-License-Identifier: BSD-2-Clause-Patent
// A VIC copy held behind a syncpoint the test releases (SwitchVICGateABI.h), debug kernels only: a video set whose
// producer is still pending at its swap, which no decoder or Core Animation request makes (each completes before the
// picture is presented), for display.media_completion_flip_gated.
#pragma once
#include "TegraVIC.h"

#if TEGRA_VIC_STATS
#include <IOKit/IOUserClient.h>

// client: the TegraVIC connection asking; only it releases the gate it armed.
IOReturn switch_vic_gate_call(const void *client, const IOExternalMethodArguments *a);
// The connection closes: a gate it armed and has not released is released now, so VIC never waits on a dead test.
void switch_vic_gate_close(const void *client);
#endif
