// SPDX-License-Identifier: MIT
// debug.switch_gpu_ramfc (debug kernels): writing runs tgm_ramfc_probe (each live client channel's RAMFC timeslice
// against the value last written to it, once it has completed a job since; then the kernel probe channel's changed and
// changed back, each read after a job); reading gives the last run's line, for HW Test gpu.ramfc_timeslice.
#include <IOKit/IOLib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include "../../kits/SwitchSysctl/SwitchSysctl.h"
#include "TegraGM20BGpu.h"

#if TGM_STATS
namespace
{
struct sysctl_oid gOid;

char gLine[288] = "channels=0 why=not_run";
bool gProbing;

int ramfcSysctl SYSCTL_HANDLER_ARGS
{
	(void)oidp;
	(void)arg1;
	(void)arg2;
	if (req->newptr) {
		int run = 0;
		const int error = SYSCTL_IN(req, &run, sizeof(run));
		if (error)
			return error;
		if (__atomic_exchange_n(&gProbing, true, __ATOMIC_ACQUIRE))
			return EBUSY;
		struct tgm_ramfc_look look = {};
		const int ret = tgm_ramfc_probe(&look);
		__atomic_store_n(&gProbing, false, __ATOMIC_RELEASE);
		if (ret)
			return EBUSY;
		snprintf(gLine, sizeof(gLine),
			 "channels=%u checked=%u high=%u mismatched=%u first_chid=%u "
			 "first_ramfc=%08x "
			 "first_expected=%08x flip_chid=%u flip_from=%08x flip_to=%08x "
			 "flip_read=%08x "
			 "back_read=%08x flip_ret=%d",
			 look.channels, look.checked, look.high, look.mismatched, look.chid,
			 look.ramfc, look.expected, look.flip_chid, look.flip_from, look.flip_to,
			 look.flip_read, look.back_read, look.flip_ret);
	}
	return SYSCTL_OUT(req, gLine, strlen(gLine) + 1);
}
}  // namespace

extern "C" void tgm_ramfc_probe_register(void)
{
	switch_sysctl_oid(
		&gOid, &sysctl__debug_children, "switch_gpu_ramfc",
		static_cast<int>(CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED),
		ramfcSysctl, "A",
		"write: probe the RAMFC timeslices; read: client channels against their last "
		"write, and a change and back");
	sysctl_register_oid(&gOid);
}
#endif
