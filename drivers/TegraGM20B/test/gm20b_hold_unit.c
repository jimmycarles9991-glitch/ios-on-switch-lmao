// SPDX-License-Identifier: MIT
// hold.c and hold_stats.c as the kernel builds them, their #include lines left out for gm20b_hold_stubs.h's stand-ins.
#include "drivers/TegraGM20B/test/gm20b_hold_stubs.h"
#include "drivers/TegraGM20B/SwitchGpuChanTable.h"
#include "drivers/TegraGM20B/SwitchGpuPreemptLanding.h"
#include "drivers/TegraGM20B/TegraGM20BGpu.h"
#include "drivers/TegraGM20B/TegraGM20BHost.h"
#include "drivers/TegraGM20B/TegraGM20BSched.h"
#include "drivers/TegraGM20B/TegraGM20BSchedPolicy.h"
#include "drivers/TegraGM20B/deadline_stats.h"
#include "drivers/TegraGM20B/test/hold_source.h"
#include "drivers/TegraGM20B/test/hold_stats_source.h"
