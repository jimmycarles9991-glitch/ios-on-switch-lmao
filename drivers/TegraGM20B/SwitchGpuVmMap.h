// SPDX-License-Identifier: MIT
#pragma once

/* Sysmem is mapped volatile (GPU L2 does not keep its lines): CONTRACT.md §3(b) option one; cacheable with
 * l2_on(). A compressible kind maps with 128 KiB pages and the memory's comptags (TegraGM20BComp.h) or is refused: a map
 * demoted to its uncompressed kind, as nvgpu demotes one, would look compressed to its caller, who would go on clearing
 * and resolving it as compressed. */
int tgm_vm_map_raw(struct tgm_vm *vm, uint64_t va, uint64_t range, struct tgm_mem *mem,
		   uint64_t offset, uint8_t kind, int read_only, int vol, int gpu_ordered,
		   uint8_t *page_shift)
{
	struct gf100_vmm_map_v0 args = {
		.version = 0, .vol = !!vol, .ro = !!read_only, .priv = 0, .kind = kind};
	u8 shift = PAGE_SHIFT;
	int ret = 0;
	int got = 0;
	u32 ordered_scope = 0;
	if (va < TGM_USER_VA_START || range > TGM_KERNEL_VA || va > TGM_KERNEL_VA - range ||
	    offset > mem->bytes || range > mem->bytes - offset ||
	    !nvif_mmu_kind_valid(&tgm_gpu.mmu, kind))
		return -EINVAL;
	if (tgm_gpu.mmu.kind[kind] != kind) {
		if (!tgm_comp_bind_ok(va, offset, range, mem->page))
			return -EINVAL;
		shift = TGM_COMP_SHIFT;
	}
	tgm_gate_enter();
	mutex_lock(&vm->map_lock);
	if (gpu_ordered)
		ordered_scope = nvxnu_gpu_ordered_begin();
	ret = nvif_vmm_raw_get(&vm->vmm, va, range, shift);
	got = !ret;
	if (got) {
		ret = nvif_vmm_raw_map(&vm->vmm, va, range, shift, &args, sizeof(args), &mem->mem,
				       offset);
	}
	if (gpu_ordered)
		nvxnu_gpu_ordered_end(ordered_scope);
	if (gpu_ordered && !ret)
		tgm_tlb_mapped(&vm->tlb);
	if (ret) {
		/* removals are synchronous (TegraGM20BTlb.h): a failed get may have dropped page tables it made */
		if (got) {
			nvif_vmm_raw_put(&vm->vmm, va, range, shift);
		} else if (gpu_ordered) {
			mmu_invalidate_all();
		}
	}
	mutex_unlock(&vm->map_lock);
	gate_leave();
	if (!ret && page_shift)
		*page_shift = shift;
	if (!ret && stats_on())
		tgm_stats_map(shift == PAGE_SHIFT ? range : 0, shift == PAGE_SHIFT ? 0 : range,
			      shift == PAGE_SHIFT
				      ? switch_gpu_mapping_big_bytes(mem->pages, mem->npages, va,
								     offset, range)
				      : 0);
	return ret;
}

int tgm_vm_map(struct tgm_vm *vm, uint64_t va, uint64_t range, struct tgm_mem *mem, uint64_t offset,
	       uint8_t kind, int read_only)
{
	return tgm_vm_map_raw(vm, va, range, mem, offset, kind, read_only, !l2_on(), 0, NULL);
}

int tgm_vm_map_ordered(struct tgm_vm *vm, uint64_t va, uint64_t range, struct tgm_mem *mem,
		       uint64_t offset, uint8_t kind, int read_only, uint8_t *page_shift)
{
	if (!vm->owner)
		return -EINVAL;
	return tgm_vm_map_raw(vm, va, range, mem, offset, kind, read_only, !l2_on(), 1, page_shift);
}

int tgm_vm_unmap_pages(struct tgm_vm *vm, uint64_t va, uint64_t range, uint8_t page_shift)
{
	int ret = 0;
	if (va < TGM_USER_VA_START || range > TGM_KERNEL_VA || va > TGM_KERNEL_VA - range)
		return -EINVAL;
	tgm_gate_enter();
	mutex_lock(&vm->map_lock);
	nvkm_ltc_flush(tgm_gpu.device->ltc);
	ret = nvif_vmm_raw_unmap(&vm->vmm, va, range, page_shift, false);
	nvif_vmm_raw_put(&vm->vmm, va, range, page_shift);
	mutex_unlock(&vm->map_lock);
	gate_leave();
	return ret;
}

int tgm_vm_unmap(struct tgm_vm *vm, uint64_t va, uint64_t range)
{
	return tgm_vm_unmap_pages(vm, va, range, PAGE_SHIFT);
}

static void mem_prepare_big_views(struct tgm_mem *mem)
{
	u32 at = 0;
	if (mem->big_views_ready)
		return;
	mem->big_views_ready = true;
	if (mem->page >= SWITCH_GPU_BIG_SHIFT)
		return;
	while (at <= mem->npages && SWITCH_GPU_BIG_PAGES <= mem->npages - at) {
		u32 first = at;
		struct switch_gpu_memory_view *view;
		struct nvif_mem_ram_v0 args = {.version = 0};
		int ret;
		if (!switch_gpu_mapping_big_page(mem->pages, mem->npages, at)) {
			++at;
			continue;
		}
		do {
			at += SWITCH_GPU_BIG_PAGES;
		} while (switch_gpu_mapping_big_page(mem->pages, mem->npages, at));
		view = kzalloc(sizeof(*view), GFP_KERNEL);
		if (!view)
			continue;
		view->offset = (u64)first << PAGE_SHIFT;
		view->bytes = (u64)(at - first) << PAGE_SHIFT;
		args.dma = (dma_addr_t *)(mem->pages + first);
		ret = nvif_mem_ctor_type(&tgm_gpu.mmu, "big-span", NVIF_CLASS_MEM_GF100,
					 tgm_gpu.mem_type, SWITCH_GPU_BIG_SHIFT, view->bytes, &args,
					 sizeof(args), &view->mem);
		if (ret) {
			kfree(view);
			continue;
		}
		view->next = mem->big_views;
		mem->big_views = view;
	}
}

static struct nvif_mem *mem_mapping_view(struct tgm_mem *mem,
					 struct switch_gpu_mapping_entry *entry, u64 *offset)
{
	struct switch_gpu_memory_view *view;
	*offset = entry->offset;
	if (entry->shift == PAGE_SHIFT || mem->page >= entry->shift)
		return &mem->mem;
	for (view = mem->big_views; view; view = view->next) {
		if (entry->offset >= view->offset && entry->offset - view->offset <= view->bytes &&
		    entry->bytes <= view->bytes - (entry->offset - view->offset)) {
			*offset -= view->offset;
			return &view->mem;
		}
	}
	entry->shift = PAGE_SHIFT;
	return &mem->mem;
}

int switch_gpu_vm_map_volatile(struct tgm_vm *vm, u64 va, u64 range, struct tgm_mem *mem,
			       u64 offset, u8 kind, int read_only, int vol,
			       struct switch_gpu_mapping **mapping)
{
	struct gf100_vmm_map_v0 args = {
		.version = 0, .vol = !!vol || !l2_on(), .ro = !!read_only, .kind = kind};
	struct switch_gpu_mapping_run run;
	struct switch_gpu_mapping *made;
	u64 at = 0;
	u32 count = 0, got = 0, i, ordered_scope;
	int ret = 0, failed_get = 0;
	bool compressed;
	if (!mapping || !vm || !mem || !vm->owner || !range ||
	    ((va | range | offset) & (PAGE_SIZE - 1)) || va < TGM_USER_VA_START ||
	    range > TGM_KERNEL_VA || va > TGM_KERNEL_VA - range || offset > mem->bytes ||
	    range > mem->bytes - offset || !nvif_mmu_kind_valid(&tgm_gpu.mmu, kind))
		return -EINVAL;
	*mapping = NULL;
	compressed = tgm_gpu.mmu.kind[kind] != kind;
	if ((vol && kind) || (compressed && !tgm_comp_bind_ok(va, offset, range, mem->page)))
		return -EINVAL;
	while (at < range) {
		if (compressed)
			run = (struct switch_gpu_mapping_run){range, SWITCH_GPU_BIG_SHIFT};
		else if (!switch_gpu_mapping_next(mem->pages, mem->npages, va + at, offset + at,
						  range - at, &run))
			return -EINVAL;
		++count;
		at += run.bytes;
	}
	made = kzalloc(sizeof(*made) + count * sizeof(made->entries[0]), GFP_KERNEL);
	if (!made)
		return -ENOMEM;
	made->count = count;
	at = 0;
	for (i = 0; i < count; ++i) {
		if (compressed)
			run = (struct switch_gpu_mapping_run){range, SWITCH_GPU_BIG_SHIFT};
		else
			switch_gpu_mapping_next(mem->pages, mem->npages, va + at, offset + at,
						range - at, &run);
		made->entries[i] = (struct switch_gpu_mapping_entry){va + at, offset + at,
								     run.bytes, run.shift, 0};
		at += run.bytes;
	}
	tgm_gate_enter();
	mutex_lock(&vm->map_lock);
	ordered_scope = nvxnu_gpu_ordered_begin();
	for (i = 0; i < count; ++i) {
		struct switch_gpu_mapping_entry *entry = &made->entries[i];
		u64 memory_offset;
		struct nvif_mem *memory = mem_mapping_view(mem, entry, &memory_offset);
		ret = nvif_vmm_raw_get(&vm->vmm, entry->va, entry->bytes, entry->shift);
		if (ret) {
			failed_get = 1;
			break;
		}
		++got;
		ret = nvif_vmm_raw_map(&vm->vmm, entry->va, entry->bytes, entry->shift, &args,
				       sizeof(args), memory, memory_offset);
		if (ret)
			break;
	}
	nvxnu_gpu_ordered_end(ordered_scope);
	if (ret) {
		for (i = 0; i < got; ++i) {
			const struct switch_gpu_mapping_entry *entry = &made->entries[i];
			nvif_vmm_raw_unmap(&vm->vmm, entry->va, entry->bytes, entry->shift, false);
			nvif_vmm_raw_put(&vm->vmm, entry->va, entry->bytes, entry->shift);
		}
		if (failed_get)
			mmu_invalidate_all();
	} else {
		tgm_tlb_mapped(&vm->tlb);
	}
	mutex_unlock(&vm->map_lock);
	gate_leave();
	if (ret)
		kfree(made);
	else
		*mapping = made;
	for (i = 0; !ret && stats_on() && i < count; ++i) {
		const struct switch_gpu_mapping_entry *entry = &made->entries[i];
		const bool big = entry->shift != PAGE_SHIFT;

		tgm_stats_map(big ? 0 : entry->bytes, big ? entry->bytes : 0, 0);
	}
	return ret;
}

int switch_gpu_vm_map(struct tgm_vm *vm, u64 va, u64 range, struct tgm_mem *mem, u64 offset,
		      u8 kind, int read_only, struct switch_gpu_mapping **mapping)
{
	return switch_gpu_vm_map_volatile(vm, va, range, mem, offset, kind, read_only, 0, mapping);
}

void switch_gpu_vm_unmap(struct tgm_vm *vm, struct switch_gpu_mapping **mapping)
{
	struct switch_gpu_mapping *made = *mapping;
	u32 i;
	if (!made)
		return;
	tgm_gate_enter();
	mutex_lock(&vm->map_lock);
	nvkm_ltc_flush(tgm_gpu.device->ltc);
	for (i = 0; i < made->count; ++i) {
		const struct switch_gpu_mapping_entry *entry = &made->entries[i];
		if (entry->pending)
			continue;
		nvif_vmm_raw_unmap(&vm->vmm, entry->va, entry->bytes, entry->shift, false);
		nvif_vmm_raw_put(&vm->vmm, entry->va, entry->bytes, entry->shift);
	}
	mutex_unlock(&vm->map_lock);
	gate_leave();
	kfree(made);
	*mapping = NULL;
}

static struct switch_gpu_mapping *mapping_alloc(u32 count)
{
	struct switch_gpu_mapping *made = NULL;
	if (count) {
		made = kzalloc(sizeof(*made) + count * sizeof(made->entries[0]), GFP_KERNEL);
		if (!made)
			return NULL;
		made->count = 0;
	}
	return made;
}

static void mapping_add(struct switch_gpu_mapping *part, u64 va, u64 offset, u64 bytes, u8 shift)
{
	part->entries[part->count++] =
		(struct switch_gpu_mapping_entry){va, offset, bytes, shift, 0};
}

static void mapping_add_pending(struct switch_gpu_mapping *part, u64 va, u64 offset, u64 bytes,
				u8 shift, u8 pending)
{
	mapping_add(part, va, offset, bytes, shift);
	part->entries[part->count - 1].pending = pending;
}

int switch_gpu_vm_split(struct tgm_vm *vm, struct switch_gpu_mapping **mapping, struct tgm_mem *mem,
			u8 kind, int read_only, int vol, u64 va, u64 range,
			struct switch_gpu_mapping **below, struct switch_gpu_mapping **taken,
			struct switch_gpu_mapping **above)
{
	struct switch_gpu_mapping *old = mapping ? *mapping : NULL;
	struct switch_gpu_mapping *parts[3] = {NULL, NULL, NULL};
	u32 counts[3] = {0, 0, 0}, i, p;
	const u64 end = va + range;
	(void)read_only;
	(void)vol;
	if (!old || !vm || !mem || !range || ((va | range) & (PAGE_SIZE - 1)) || end < va)
		return -EINVAL;
	for (i = 0; i < old->count; ++i) {
		const struct switch_gpu_mapping_entry *e = &old->entries[i];
		const u64 e_end = e->va + e->bytes;
		const bool cut = e->va < end && va < e_end && (e->va < va || e_end > end);
		if (cut && e->shift != PAGE_SHIFT && tgm_gpu.mmu.kind[kind] != kind)
			return -EINVAL;
		counts[0] += e->va < va;
		counts[2] += e_end > end;
		counts[1] +=
			e->va < end && va < e_end && !(cut && e->pending && e->shift != PAGE_SHIFT);
	}
	for (p = 0; p < 3; ++p) {
		parts[p] = mapping_alloc(counts[p]);
		if (counts[p] && !parts[p]) {
			while (p--)
				kfree(parts[p]);
			return -ENOMEM;
		}
	}
	for (i = 0; i < old->count; ++i) {
		const struct switch_gpu_mapping_entry *e = &old->entries[i];
		const u64 e_end = e->va + e->bytes;
		const bool cut = e->va < end && va < e_end && (e->va < va || e_end > end);
		/* a 128 KiB page an edge cuts keeps its PTE, whole, with the range's part until that retires; what stays
		 * outside the range waits, as 4 KiB entries, for switch_gpu_vm_install once the big PTE is gone */
		const bool big = cut && e->shift != PAGE_SHIFT;
		const u8 kept = big ? PAGE_SHIFT : e->shift, pending = big ? 1 : e->pending;
		const u64 low = e->va > va ? e->va : va, high = e_end < end ? e_end : end;
		if (e->va < va)
			mapping_add_pending(parts[0], e->va, e->offset,
					    (e_end < va ? e_end : va) - e->va, kept, pending);
		if (big && !e->pending)
			mapping_add_pending(parts[1], e->va, e->offset, e->bytes, e->shift, 0);
		else if (e->va < end && va < e_end && !big)
			mapping_add_pending(parts[1], low, e->offset + (low - e->va), high - low,
					    e->shift, e->pending);
		if (e_end > end) {
			const u64 from = e->va > end ? e->va : end;
			mapping_add_pending(parts[2], from, e->offset + (from - e->va),
					    e_end - from, kept, pending);
		}
	}
	for (p = 0; p < 3; ++p) {
		if (parts[p] && !parts[p]->count) {
			kfree(parts[p]);
			parts[p] = NULL;
		}
	}
	kfree(old);
	*mapping = NULL;
	*below = parts[0];
	*taken = parts[1];
	*above = parts[2];
	return 0;
}

int switch_gpu_vm_prepare(struct tgm_vm *vm, u64 va, u64 range, struct tgm_mem *mem, u64 offset,
			  u8 kind, struct switch_gpu_mapping **mapping)
{
	struct switch_gpu_mapping_run run;
	struct switch_gpu_mapping *made;
	u64 at = 0;
	u32 count = 0;
	if (!mapping || !vm || !mem || !vm->owner || !range ||
	    ((va | range | offset) & (PAGE_SIZE - 1)) || va < TGM_USER_VA_START ||
	    range > TGM_KERNEL_VA || va > TGM_KERNEL_VA - range || offset > mem->bytes ||
	    range > mem->bytes - offset || !nvif_mmu_kind_valid(&tgm_gpu.mmu, kind))
		return -EINVAL;
	*mapping = NULL;
	if (tgm_gpu.mmu.kind[kind] != kind && !tgm_comp_bind_ok(va, offset, range, mem->page))
		return -EINVAL;
	for (at = 0; at < range; at += run.bytes, ++count)
		if (tgm_gpu.mmu.kind[kind] != kind)
			run = (struct switch_gpu_mapping_run){range, SWITCH_GPU_BIG_SHIFT};
		else if (!switch_gpu_mapping_next(mem->pages, mem->npages, va + at, offset + at,
						  range - at, &run))
			return -EINVAL;
	made = mapping_alloc(count);
	if (!made)
		return -ENOMEM;
	for (at = 0; at < range; at += run.bytes) {
		if (tgm_gpu.mmu.kind[kind] != kind)
			run = (struct switch_gpu_mapping_run){range, SWITCH_GPU_BIG_SHIFT};
		else
			switch_gpu_mapping_next(mem->pages, mem->npages, va + at, offset + at,
						range - at, &run);
		mapping_add_pending(made, va + at, offset + at, run.bytes, run.shift, 1);
	}
	*mapping = made;
	return 0;
}

int switch_gpu_mapping_pending(const struct switch_gpu_mapping *mapping);

int switch_gpu_vm_install(struct tgm_vm *vm, struct switch_gpu_mapping *mapping,
			  struct tgm_mem *mem, u8 kind, int read_only, int vol)
{
	struct gf100_vmm_map_v0 args = {
		.version = 0, .vol = !!vol || !l2_on(), .ro = !!read_only, .kind = kind};
	u32 i, ordered_scope, failed = 0;
	int ret = 0;
	if (!switch_gpu_mapping_pending(mapping))
		return 0;
	mem_prepare_big_views(mem);
	tgm_gate_enter();
	mutex_lock(&vm->map_lock);
	ordered_scope = nvxnu_gpu_ordered_begin();
	for (i = 0; i < mapping->count; ++i) {
		struct switch_gpu_mapping_entry *e = &mapping->entries[i];
		u64 memory_offset;
		struct nvif_mem *memory;
		if (!e->pending)
			continue;
		memory = mem_mapping_view(mem, e, &memory_offset);
		ret = nvif_vmm_raw_get(&vm->vmm, e->va, e->bytes, e->shift);
		if (!ret) {
			ret = nvif_vmm_raw_map(&vm->vmm, e->va, e->bytes, e->shift, &args,
					       sizeof(args), memory, memory_offset);
			if (ret)
				e->pending =
					2; /* got, not mapped: put below, outside the ordered scope */
		}
		if (!ret)
			e->pending = 0;
		else
			++failed;
	}
	nvxnu_gpu_ordered_end(ordered_scope);
	tgm_tlb_mapped(&vm->tlb);
	/* an entry that could not be mapped stays unmapped and leaves the mapping */
	for (i = 0; failed && i < mapping->count;) {
		struct switch_gpu_mapping_entry *e = &mapping->entries[i];
		if (!e->pending) {
			++i;
			continue;
		}
		if (e->pending == 2)
			nvif_vmm_raw_put(&vm->vmm, e->va, e->bytes, e->shift);
		memmove(e, e + 1, (mapping->count - i - 1) * sizeof(*e));
		--mapping->count;
	}
	mutex_unlock(&vm->map_lock);
	gate_leave();
	return failed ? -ENOMEM : 0;
}

int switch_gpu_mapping_installed(const struct switch_gpu_mapping *mapping)
{
	u32 i;
	for (i = 0; mapping && i < mapping->count; ++i)
		if (!mapping->entries[i].pending)
			return 1;
	return 0;
}

int switch_gpu_mapping_pending(const struct switch_gpu_mapping *mapping)
{
	u32 i;
	for (i = 0; mapping && i < mapping->count; ++i)
		if (mapping->entries[i].pending)
			return 1;
	return 0;
}

int switch_gpu_mapping_span(const struct switch_gpu_mapping *mapping, u64 *va, u64 *bytes)
{
	if (!mapping || !mapping->count)
		return -EINVAL;
	*va = mapping->entries[0].va;
	*bytes = mapping->entries[mapping->count - 1].va +
		 mapping->entries[mapping->count - 1].bytes - *va;
	return 0;
}

void switch_gpu_mapping_describe(const struct switch_gpu_mapping *mapping, char *out, size_t size)
{
	size_t at = 0;
	u32 i;
	if (!size)
		return;
	out[0] = 0;
	for (i = 0; mapping && i < mapping->count && at + 1 < size; ++i) {
		const struct switch_gpu_mapping_entry *e = &mapping->entries[i];
		const int n =
			snprintf(out + at, size - at, "%s%#llx+%#llx/%u", i ? " " : "",
				 (unsigned long long)e->va, (unsigned long long)e->bytes, e->shift);
		if (n < 0)
			break;
		at += (size_t)n < size - at ? (size_t)n : size - at - 1;
	}
}

int tgm_vm_kmap(struct tgm_vm *vm, struct tgm_mem *mem, uint64_t bytes, uint64_t *va)
{
	return tgm_vm_kmap_raw(vm, mem, bytes, 0, va);
}

int tgm_vm_kmap_raw(struct tgm_vm *vm, struct tgm_mem *mem, uint64_t bytes, int read_only,
		    uint64_t *va)
{
	struct gf100_vmm_map_v0 args = {
		.version = 0, .vol = 1, .ro = !!read_only, .priv = 0, .kind = 0};
	struct tgm_kmap *k = kzalloc(sizeof(*k), GFP_KERNEL);
	int ret = 0;
	if (!k)
		return -ENOMEM;
	tgm_gate_enter();
	mutex_lock(&vm->map_lock);
	ret = nvif_vmm_get(&vm->vmm, ADDR, false, PAGE_SHIFT, 0, bytes, &k->vma);
	if (!ret) {
		ret = nvif_vmm_map(&vm->vmm, k->vma.addr, bytes, &args, sizeof(args), &mem->mem, 0);
		if (ret)
			nvif_vmm_put(&vm->vmm, &k->vma);
	}
	if (!ret)
		list_add(&k->head, &vm->kmaps);
	mutex_unlock(&vm->map_lock);
	gate_leave();
	if (ret) {
		kfree(k);
		return ret;
	}
	*va = k->vma.addr;
	return 0;
}
