// SPDX-License-Identifier: MIT
#pragma once

int switch_gpu_vm_sparse(struct tgm_vm *vm, u64 va, u64 range, int now,
			 struct switch_gpu_mapping **mapping)
{
	struct switch_gpu_mapping_run run;
	struct switch_gpu_mapping *made;
	u64 at = 0;
	u32 count = 0;
	int ret = 0;
	if (!mapping || !vm || !vm->owner || !range || ((va | range) & (PAGE_SIZE - 1)) ||
	    va < TGM_USER_VA_START || range > TGM_KERNEL_VA || va > TGM_KERNEL_VA - range)
		return -EINVAL;
	*mapping = NULL;
	for (at = 0; at < range; at += run.bytes, ++count)
		switch_gpu_mapping_sparse_next(va + at, range - at, &run);
	made = mapping_alloc(count);
	if (!made)
		return -ENOMEM;
	made->sparse = 1;
	for (at = 0; at < range; at += run.bytes) {
		switch_gpu_mapping_sparse_next(va + at, range - at, &run);
		mapping_add_pending(made, va + at, 0, run.bytes, run.shift, 1);
	}
	if (now)
		ret = switch_gpu_vm_install(vm, made, NULL, 0, 0, 0);
	if (ret)
		switch_gpu_vm_unmap(vm, &made);
	*mapping = made;
	return ret;
}
