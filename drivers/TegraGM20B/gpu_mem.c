// SPDX-License-Identifier: MIT
// The pool, address spaces, memory objects and their import notes.
#include "gpu_internal.h"

/* Pool BOs are mapped into processes, and a process maps whole VM pages (four pool pages): each BO starts on one and is
 * padded to one, so its mapping never reaches another BO. */
static uint64_t pool_bo_bytes(uint64_t bytes)
{
	const uint64_t vm_page = nvxnu_vm_page_size();
	return (bytes + vm_page - 1) & ~(vm_page - 1);
}

void *tgm_pool_alloc(uint64_t bytes, uint64_t align, uint64_t *pa)
{
	unsigned long long h = 0;
	void *va = nvxnu_pool_alloc_clean(pool_bo_bytes(bytes),
					  max_t(u64, align, nvxnu_vm_page_size()), &h);
	*pa = va ? h : 0;
	return va;
}

void *tgm_pool_alloc_ready(uint64_t bytes, uint64_t align, uint64_t reserve, uint64_t *pa)
{
	unsigned long long physical = 0;
	void *va = nvxnu_pool_alloc_ready(
		pool_bo_bytes(bytes), max_t(u64, align, nvxnu_vm_page_size()), reserve, &physical);
	*pa = va ? physical : 0;
	return va;
}

#if TGM_STATS
/* stats: freed pool BOs the CPU zeroed for the copy engine (tgm_pool_free_zeroed) */
u64 tgm_cpu_zeroes, tgm_cpu_zero_bytes;

#endif
void tgm_pool_free_zeroed(void *va, uint64_t bytes, uint64_t pa)
{
	bytes = pool_bo_bytes(bytes); /* the padding too: the owner's mapping covered it */
	if (TGM_STATS) {
		__atomic_fetch_add(&tgm_cpu_zeroes, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&tgm_cpu_zero_bytes, bytes, __ATOMIC_RELAXED);
	}
	memset(va, 0, bytes);
	/* the write-combining buffers drained before the pages are handed out again */
	__asm__ volatile("dsb sy" ::: "memory");
	nvxnu_pool_free_zeroed(pa, bytes);
}

/* Back dirty: the scrubber zeroes it on the GPU (see "pool scrub") unless an allocation takes it first and zeroes it. */
void tgm_pool_free(void *va, uint64_t bytes, uint64_t pa)
{
	dma_free_attrs(&tgm_gpu_pdev.dev, pool_bo_bytes(bytes), va, pa, 0);
	scrub_kick();
}

int tgm_pool_alloc_chunks(uint64_t bytes, uint64_t *pas)
{
	if (!bytes || (bytes & (TGM_COMP_BYTES - 1)))
		return -EINVAL;
	return nvxnu_pool_alloc_chunks(bytes >> TGM_COMP_SHIFT, pas);
}

uint64_t tgm_pool_clean_chunks(void)
{
	return nvxnu_pool_clean_chunks();
}

void tgm_pool_free_chunks(const uint64_t *pas, uint32_t npages)
{
	u64 lo = 0;
	u64 bytes = 0;
	uint32_t i = 0;
	for (i = 0; i < npages; i += TGM_COMP_PAGES) {
		const u64 pa = pas[i];
		if (bytes && pa + TGM_COMP_BYTES == lo) {
			lo = pa;
		} else if (!bytes || pa != lo + bytes) {
			if (bytes)
				dma_free_attrs(&tgm_gpu_pdev.dev, bytes, NULL, lo, 0);
			lo = pa;
			bytes = 0;
		}
		bytes += TGM_COMP_BYTES;
	}
	if (bytes)
		dma_free_attrs(&tgm_gpu_pdev.dev, bytes, NULL, lo, 0);
	scrub_kick();
}

/* What BoAlloc's pool allocations can take: free pages the scrubber has zeroed. A freed page waits for it. */
uint64_t tgm_pool_avail(void)
{
	return (u64)nvxnu_pool_ready_pages() << PAGE_SHIFT;
}

struct switch_gpu_mapping_entry {
	u64 va, offset, bytes;
	u8 shift;
	u8 pending; /* no PTEs yet: switch_gpu_vm_install maps it */
};

struct switch_gpu_mapping {
	u32 count;
	u8 sparse; /* switch_gpu_vm_sparse's: no memory behind any entry */
	u8 l2_writable;
	u8 compressed;
	u8 l2_clean;
	struct switch_gpu_mapping_entry entries[];
};

struct tgm_kmap {
	struct list_head head;
	struct nvif_vma vma;
};

/* The present channel's fence page (see "GPU present"): one pool page, made with the present's address space and kept
 * for good. */
struct pfence_state tgm_pfence;

static DEFINE_MUTEX(pfence_lock);

int tgm_pfence_get(struct tgm_vm *vm)
{
	int ret = 0;
	mutex_lock(&pfence_lock);
	if (!tgm_pfence.mem) {
		if (!tgm_pfence.va)
			tgm_pfence.va = tgm_pool_alloc(PAGE_SIZE, 0, &tgm_pfence.pa);
		ret = tgm_pfence.va ? tgm_mem_new(vm, &tgm_pfence.pa, 1, &tgm_pfence.mem) : -ENOMEM;
	}
	mutex_unlock(&pfence_lock);
	return ret;
}

int tgm_vm_new(struct tgm_vm **pvm)
{
	/* bigpage selects the big-page size, 16 (64 KiB) or 17 (128 KiB, nouveau's default); anything else is -EINVAL
	 * (vmmgm200.c gm200_vmm_new_; 0 fails M3). 4 KiB pages stay available for fragmented ranges and tails. */
	struct gm200_vmm_v0 args = {.version = 0, .bigpage = 16};
	struct tgm_vm *vm = NULL;
	int ret = 0;

	if (tgm_gpu.stage < 2)
		return -ENODEV;
	vm = kzalloc(sizeof(*vm), GFP_KERNEL);
	if (!vm)
		return -ENOMEM;
	mutex_init(&vm->map_lock);
	INIT_LIST_HEAD(&vm->kmaps);
	tgm_tlb_space_init(&vm->tlb);
	tgm_gpu_lock();
	/* RAW: the client manages [0, 1<<39) with raw binds; nvkm allocates in [1<<39, 1<<40) */
	ret = nvif_vmm_ctor(&tgm_gpu.mmu, "uvmm", NVIF_CLASS_VMM_GM200, RAW, TGM_KERNEL_VA,
			    TGM_VA_LIMIT - TGM_KERNEL_VA, &args, sizeof(args), &vm->vmm);
	mutex_unlock(&tgm_gpu.lock);
	if (ret) {
		mutex_destroy(&vm->map_lock);
		kfree(vm);
		return ret;
	}
	/* every channel's acquires on another channel's fence (task order, ownership dependencies) read this view */
	ret = tgm_fences_map(vm);
	if (ret) {
		tgm_vm_del(&vm);
		return ret;
	}
	*pvm = vm;
	return 0;
}

/* The caller holds tgm_gpu.lock or the SC7 gate. LTC/MMU locks serialize hardware maintenance. */
static void mmu_invalidate_all(void)
{
	struct nvkm_device *device = tgm_gpu.device;

	nvkm_ltc_invalidate(device->ltc);
	mutex_lock(&device->mmu->mutex);
	/* nouveau's nvkm_msec assigns in its polling loop's condition. */
	/* NOLINTNEXTLINE(bugprone-assignment-in-if-condition) */
	if (nvkm_msec(device, 2000, if (nvkm_rd32(device, 0x100c80) & 0x00ff0000) break;) < 0) {
		nvxnu_panic("tegra-gm20b: MMU invalidate: no free slot after 2 s (0x100c80 %08x)",
			    nvkm_rd32(device, 0x100c80));
	}
	nvkm_wr32(device, 0x100cbc, 0x80000000 | 0x00000003); /* PAGE_ALL | ALL_PDB */
	/* nouveau's nvkm_msec assigns in its polling loop's condition. */
	/* NOLINTNEXTLINE(bugprone-assignment-in-if-condition) */
	if (nvkm_msec(device, 2000, if (nvkm_rd32(device, 0x100c80) & 0x00008000) break;) < 0) {
		nvxnu_panic(
			"tegra-gm20b: MMU invalidate not acknowledged after 2 s (0x100c80 %08x)",
			nvkm_rd32(device, 0x100c80));
	}
	mutex_unlock(&device->mmu->mutex);
}

/* The owner has drained mapping callers and its retirement worker before destroying the VM. */
void tgm_vm_del(struct tgm_vm **pvm)
{
	struct tgm_vm *vm = *pvm;
	struct tgm_kmap *k = NULL;
	struct tgm_kmap *t = NULL;
	if (!vm)
		return;
	tgm_gpu_lock();
	nvkm_ltc_flush(tgm_gpu.device->ltc);
	list_for_each_entry_safe(k, t, &vm->kmaps, head)
	{
		nvif_vmm_put(&vm->vmm, &k->vma);
		list_del(&k->head);
		kfree(k);
	}
	nvif_vmm_dtor(&vm->vmm);
	mmu_invalidate_all(); /* a later space may reuse this one's page directory page */
	mutex_unlock(&tgm_gpu.lock);
	mutex_destroy(&vm->map_lock);
	kfree(vm);
	*pvm = NULL;
}

void tgm_vm_claim(struct tgm_vm *vm, const void *owner)
{
	vm->owner = owner;
}

void tgm_vm_claim_connection(struct tgm_vm *vm, const void *connection)
{
	vm->owner = connection;
	vm->connection = connection;
}

static void mem_prepare_big_views(struct tgm_mem *mem);

static int mem_new(const uint64_t *pas, uint32_t npages, u8 page, int compute_comp, int allow_smmu,
		   struct tgm_mem **pmem, uint64_t *lock_wait_ns)
{
	struct nvif_mem_ram_v0 args = {.version = 0, .dma = NULL, .sgl = NULL};
	struct tgm_mem *mem = NULL;
	const uint64_t *effective = pas;
	u64 smmu_iova = 0;
	u64 *smmu_pages = NULL;
	u64 asked = 0;
	int ret = 0;
	if (tgm_gpu.stage < 2)
		return -ENODEV;
	mem = kzalloc(sizeof(*mem), GFP_KERNEL);
	if (!mem)
		return -ENOMEM;
	/* GM20B's L2 sits upstream of Tegra SMMU and tags lines by pre-SMMU address (BIT(34)|iova
	 * vs direct PA). Only canonical exclusive compressible memory may acquire an SMMU IOVA. */
	if (allow_smmu && compute_comp && switch_gpu_smmu_should_map(pas, npages)) {
		smmu_iova = switch_gpu_smmu_map(pas, npages);
		if (smmu_iova) {
			smmu_pages = kmalloc_array(npages, sizeof(*smmu_pages), GFP_KERNEL);
			if (smmu_pages) {
				uint32_t i = 0;
				for (i = 0; i < npages; ++i) {
					const u64 page_iova = smmu_iova + ((u64)i << PAGE_SHIFT);
					smmu_pages[i] = switch_gpu_smmu_pa(page_iova);
				}
				effective = smmu_pages;
			} else {
				switch_gpu_smmu_unmap(smmu_iova, npages);
				smmu_iova = 0;
			}
		}
	}
	if (compute_comp)
		page = (u8)tgm_comp_page_shift(effective, npages);
	args.dma = (dma_addr_t *)effective;
	mem->bytes = (u64)npages << PAGE_SHIFT;
	mem->page = page;
	mem->pages = effective;
	mem->npages = npages;
	mem->smmu_iova = smmu_iova;
	mem->smmu_pages = smmu_pages;
	asked = stats_on() && lock_wait_ns ? nvxnu_nanotime() : 0;
	tgm_gpu_lock();
	if (asked)
		*lock_wait_ns = nvxnu_nanotime() - asked;
	ret = nvif_mem_ctor_type(&tgm_gpu.mmu, "bo", NVIF_CLASS_MEM_GF100, tgm_gpu.mem_type, page,
				 mem->bytes, &args, sizeof(args), &mem->mem);
	if (!ret)
		mem_prepare_big_views(mem);
	mutex_unlock(&tgm_gpu.lock);
	if (ret) {
		if (smmu_iova)
			switch_gpu_smmu_unmap(smmu_iova, npages);
		kfree(smmu_pages);
		kfree(mem);
		return ret;
	}
	*pmem = mem;
	return 0;
}

int tgm_mem_new(struct tgm_vm *vm, const uint64_t *pas, uint32_t npages, struct tgm_mem **pmem)
{
	(void)vm;
	return mem_new(pas, npages, PAGE_SHIFT, 0, 0, pmem, NULL);
}

int tgm_mem_new_waited(struct tgm_vm *vm, const uint64_t *pas, uint32_t npages,
		       struct tgm_mem **pmem, uint64_t *lock_wait_ns)
{
	(void)vm;
	return mem_new(pas, npages, PAGE_SHIFT, 0, 0, pmem, lock_wait_ns);
}

/* The common backing retains this memory object and its tag reference through final physical retirement.
 * Imports use ordinary memory objects and cannot create an independently tagged view of these pages. */
int tgm_mem_new_exclusive(struct tgm_vm *vm, const uint64_t *pas, uint32_t npages,
			  struct tgm_mem **pmem)
{
	const int comp = tgm_gpu.stage >= 2 && tgm_gpu.device->ltc->num_tags;
	(void)vm;
	return mem_new(pas, npages, PAGE_SHIFT, comp, 0, pmem, NULL);
}

int tgm_mem_new_exclusive_smmu(struct tgm_vm *vm, const uint64_t *pas, uint32_t npages,
			       struct tgm_mem **pmem)
{
	const int comp = tgm_gpu.stage >= 2 && tgm_gpu.device->ltc->num_tags;
	(void)vm;
	return mem_new(pas, npages, PAGE_SHIFT, comp, comp, pmem, NULL);
}

int tgm_mem_can_compress(const struct tgm_mem *mem)
{
	return mem && mem->page >= TGM_COMP_SHIFT;
}

int tgm_mem_is_smmu(const struct tgm_mem *mem)
{
	return mem && mem->smmu_iova != 0;
}

void tgm_mem_del(struct tgm_mem **pmem)
{
	struct tgm_mem *mem = *pmem;
	if (!mem)
		return;
	tgm_gpu_lock();
	mem_big_views_drop(mem);
	nvif_mem_dtor(&mem->mem);
	mutex_unlock(&tgm_gpu.lock);
	if (mem->smmu_iova)
		switch_gpu_smmu_unmap(mem->smmu_iova, mem->npages);
	kfree(mem->smmu_pages);
	kfree(mem);
	*pmem = NULL;
}

#include "SwitchGpuVmMap.h"
#include "SwitchGpuVmSparse.h"

/* Every user channel, so a CPU reader outside the GPU client can wait for all GPU work (tegra_gm20b_sync_for_cpu). */
/* An empty list head points at itself. */
/* NOLINTNEXTLINE(misc-static-initialization-cycle) */
LIST_HEAD(tgm_live_chans);
DEFINE_MUTEX(tgm_live_lock);
u64 tgm_live_ids;
