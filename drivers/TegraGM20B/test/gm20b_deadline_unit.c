// SPDX-License-Identifier: MIT
// deadline.c and deadline_stats.c as the kernel builds them, their #include lines left out for gm20b_sched_stubs.h's
// stand-ins.
#include "drivers/TegraGM20B/test/gm20b_sched_stubs.h"
#include "drivers/TegraGM20B/TegraGM20BGpu.h"
#include "drivers/TegraGM20B/TegraGM20BSched.h"
#include "drivers/TegraGM20B/TegraGM20BSchedPolicy.h"
#include "drivers/TegraGM20B/TegraGM20BCostHist.h"
#include "drivers/TegraGM20B/SwitchGpuKeptWindow.h"
#include "drivers/TegraGM20B/deadline_stats.h"
#include "drivers/TegraDisplayFlip/TegraScanoutCopy.h"
#include "drivers/TegraGM20B/test/deadline_source.h"
#include "drivers/TegraGM20B/test/deadline_stats_source.h"
