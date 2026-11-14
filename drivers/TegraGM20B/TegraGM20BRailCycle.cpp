// SPDX-License-Identifier: MIT
// debug.switch_gpu_rail_cycle (debug kernels): writing N (1-16) rail-gates the GPU and brings it up N times on the
// sysctl's thread (tgm_rail_cycle_once), each rail-up followed by the GPU's first job and a look at its GPCPLL
// (tgm_rail_probe); reading gives the last run's line, which HW Test gpu.rail_cycle judges.
#include <IOKit/IOLib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include "../../kits/SwitchSysctl/SwitchSysctl.h"
#include "TegraGM20BGpu.h"

#if TGM_STATS
namespace
{
constexpr int kRailCyclesMax = 16;
char gLine[288] = "cycles=0 why=not_run";
bool gCycling;
struct sysctl_oid gOid;

bool gpcpllAsSet(const struct tgm_rail_look &look)
{
	return (look.cfg & 0x20001) == 0x20001 && look.vco_ok && look.pl == look.expect_pl;
}

void cycle(int cycles)
{
	int downs = 0, fences = 0, plls = 0;
	uint32_t fenceMax = 0;
	const uint32_t offRate = tgm_rail_wakes_off_rate();
	struct tgm_rail_look look = {};
	const char *why = "-";
	for (int i = 0; i < cycles; ++i) {
		if (tgm_rail_cycle_once()) {
			why = "rail_down_refused";
			break;
		}
		++downs;
		if (tgm_rail_probe(&look) == -EBUSY) {
			why = "before_m3";
			break;
		}
		fences += look.fence == 0;
		plls += gpcpllAsSet(look);
		fenceMax = look.fence_us > fenceMax ? look.fence_us : fenceMax;
	}
	snprintf(gLine, sizeof(gLine),
		 "cycles=%d downs=%d fences=%d plls=%d wakes_off_rate=%u wake_coeff=%08x "
		 "fence_us_max=%u "
		 "coeff=%08x cfg=%08x pl=%u expect_pl=%u khz=%d why=%s",
		 cycles, downs, fences, plls, tgm_rail_wakes_off_rate() - offRate, look.wake_coeff,
		 fenceMax, look.coeff, look.cfg, look.pl, look.expect_pl, look.khz, why);
}

int railCycleSysctl SYSCTL_HANDLER_ARGS
{
	(void)oidp;
	(void)arg1;
	(void)arg2;
	if (req->newptr) {
		int cycles = 0;
		const int error = SYSCTL_IN(req, &cycles, sizeof(cycles));
		if (error)
			return error;
		if (cycles < 1 || cycles > kRailCyclesMax)
			return EINVAL;
		if (__atomic_exchange_n(&gCycling, true, __ATOMIC_ACQUIRE))
			return EBUSY;
		cycle(cycles);
		__atomic_store_n(&gCycling, false, __ATOMIC_RELEASE);
	}
	return SYSCTL_OUT(req, gLine, strlen(gLine) + 1);
}
}  // namespace

extern "C" void tgm_rail_cycle_register(void)
{
	switch_sysctl_oid(
		&gOid, &sysctl__debug_children, "switch_gpu_rail_cycle",
		static_cast<int>(CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED),
		railCycleSysctl, "A",
		"write N: rail-cycle the GPU N times; read: its first job and GPCPLL");
	sysctl_register_oid(&gOid);
}
#endif
