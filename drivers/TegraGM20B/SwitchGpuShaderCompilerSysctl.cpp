// SPDX-License-Identifier: MIT
#include "SwitchGpuShaderCompiler.h"
#if SWITCH_GPU_COMPILER_LINE
#include <string.h>
#include <sys/sysctl.h>
#include "../../kits/SwitchSysctl/SwitchSysctl.h"

namespace
{
int compilerSysctl SYSCTL_HANDLER_ARGS
{
	(void)oidp;
	(void)arg1;
	(void)arg2;
	char line[96];
	switch_gpu_compiler_line(line, sizeof(line));
	return SYSCTL_OUT(req, line, strlen(line) + 1);
}

struct sysctl_oid gCompilerOid;
}  // namespace

void switch_gpu_compiler_register_sysctl(void)
{
	switch_sysctl_oid(
		&gCompilerOid, &sysctl__kern_children, "switch_shader_compiler",
		static_cast<int>(CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED),
		compilerSysctl, "A",
		"the shader compiler broker: its compiler's pid and generation");
	sysctl_register_oid(&gCompilerOid);
}
#endif
