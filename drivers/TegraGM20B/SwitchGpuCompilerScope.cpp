// SPDX-License-Identifier: MIT
// See SwitchGpuCompilerScope.h.
#include "SwitchGpuCompilerScope.h"
#include <IOKit/IOLib.h>
#include <sys/mount.h>
#include <sys/random.h>
#include <sys/vnode.h>
#include <string.h>

extern "C" {
proc_t current_proc(void);
int proc_pid(proc_t);
int proc_pidversion(proc_t);
vnode_t proc_getexecutablevnode(proc_t);
}

namespace
{

// The main executable's volume UUID and file ID: what the filesystem assigned the file at its install. An imported app's
// code signature is made from what its IPA declares (kernel/switch/include/switch/app_code_blob.h: no code slots), and a
// bootstrap image's platform bit from its own (bootstrap_signature.c), so neither can name a process.
bool fileScope(proc_t p, TegraGM20BAbiCompilerScope &out)
{
	vnode_t vp = proc_getexecutablevnode(p);
	if (!vp)
		return false;
	vfs_context_t ctx = vfs_context_current();
	struct vnode_attr va;
	VATTR_INIT(&va);
	VATTR_WANTED(&va, va_fileid);
	struct vfs_attr fa;
	VFSATTR_INIT(&fa);
	VFSATTR_WANTED(&fa, f_uuid);
	const bool known = !vnode_getattr(vp, &va, ctx) && VATTR_IS_SUPPORTED(&va, va_fileid) &&
			   !vfs_getattr(vnode_mount(vp), &fa, ctx) &&
			   VFSATTR_IS_SUPPORTED(&fa, f_uuid);
	vnode_put(vp);
	if (!known)
		return false;
	out.kind = TegraGM20BCompilerScopeFile;
	out.bytes = sizeof fa.f_uuid + sizeof va.va_fileid;
	memcpy(out.identity, fa.f_uuid, sizeof fa.f_uuid);
	memcpy(out.identity + sizeof fa.f_uuid, &va.va_fileid, sizeof va.va_fileid);
	return true;
}

// Drawn once per boot: a pid and its version repeat in a later boot, whose process must not read this one's records
uint64_t bootNonce()
{
	static uint64_t nonce = 0;
	if (!__atomic_load_n(&nonce, __ATOMIC_ACQUIRE)) {
		uint64_t drawn = 0;
		while (!drawn)
			read_random(&drawn, sizeof drawn);
		uint64_t none = 0;
		__atomic_compare_exchange_n(&nonce, &none, drawn, false, __ATOMIC_ACQ_REL,
					    __ATOMIC_ACQUIRE);
	}
	return __atomic_load_n(&nonce, __ATOMIC_ACQUIRE);
}

}  // namespace

void switch_gpu_compiler_scope(TegraGM20BAbiCompilerScope &out)
{
	out = {};
	proc_t p = current_proc();
	if (fileScope(p, out))
		return;
	static bool said = false;
	if (!__atomic_exchange_n(&said, true, __ATOMIC_RELAXED))
		IOLog("SwitchShaderCompiler: pid %d's executable gives no volume UUID and file "
		      "ID, so its app-library compiles are kept for this process alone (logged "
		      "once)\n",
		      proc_pid(p));
	const int32_t which[2] = {proc_pid(p), proc_pidversion(p)};
	const uint64_t nonce = bootNonce();
	out.kind = TegraGM20BCompilerScopeProcess;
	out.bytes = sizeof which + sizeof nonce;
	memcpy(out.identity, which, sizeof which);
	memcpy(out.identity + sizeof which, &nonce, sizeof nonce);
}
