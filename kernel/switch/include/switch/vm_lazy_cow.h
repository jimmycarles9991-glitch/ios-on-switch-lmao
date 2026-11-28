// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lazy COW-write grants for nested shared-region submaps (xnu-12377 / iOS 27).
 * Included by osfmk/vm/vm_map.c after its own headers (vm_prot_t, VM_PROT_*).
 */
#ifndef SWITCH_VM_LAZY_COW_H
#define SWITCH_VM_LAZY_COW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SWITCH_VM_LAZY_RW_MAX      64u
#define SWITCH_VM_LAZY_RW_SOFT_MAX 48u

struct switch_vm_lazy_rw_range {
	uint64_t start;
	uint64_t end;
};

/*
 * Sorted, disjoint, coalesced half-open [start, end) intervals where
 * vm_map_protect(..., FALSE, R|W|COPY) on a nested COW shared-region submap
 * was deferred until an actual write fault or eager-materializing VM call.
 */
struct switch_vm_lazy_rw_table {
	uint32_t count;
	struct switch_vm_lazy_rw_range ranges[SWITCH_VM_LAZY_RW_MAX];
};

static inline bool
switch_vm_lazy_cow_request_qualifies(bool set_max, vm_prot_t new_prot,
    bool is_kernel_map, bool map_sealed)
{
	return !set_max &&
	       new_prot == (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) &&
	       !is_kernel_map && !map_sealed;
}

static inline bool
switch_vm_lazy_cow_top_submap_qualifies(bool is_sub_map, bool needs_copy,
    bool superpage, vm_prot_t prot, bool submap_is_nested,
    bool submap_is_sealed)
{
	return is_sub_map && needs_copy && !superpage &&
	       prot == VM_PROT_READ && submap_is_nested && submap_is_sealed;
}

static inline bool
switch_vm_lazy_cow_submap_entry_qualifies(bool is_sub_map, vm_prot_t prot,
    vm_prot_t max_prot, bool permanent, bool jit, bool csm_associated,
    bool used_for_tpro, bool resilient_codesign, bool translated_allow_exec,
    bool wired, bool has_valid_object)
{
	if (is_sub_map || permanent || jit || csm_associated || used_for_tpro ||
	    resilient_codesign || translated_allow_exec || wired ||
	    !has_valid_object) {
		return false;
	}
	if (prot != VM_PROT_READ) {
		return false;
	}
	if ((max_prot & (VM_PROT_READ | VM_PROT_WRITE)) !=
	    (VM_PROT_READ | VM_PROT_WRITE)) {
		return false;
	}
	if (max_prot & VM_PROT_ALLEXEC) {
		return false;
	}
	return true;
}

static inline bool
switch_vm_lazy_cow_private_entry_qualifies(bool is_sub_map, bool superpage,
    bool is_shared, vm_prot_t prot, vm_prot_t max_prot, bool permanent,
    bool jit, bool csm_associated, bool used_for_tpro, bool resilient_codesign,
    bool translated_allow_exec, bool wired, bool has_valid_object)
{
	if (is_sub_map || superpage || is_shared || permanent || jit ||
	    csm_associated || used_for_tpro || resilient_codesign ||
	    translated_allow_exec || wired || !has_valid_object) {
		return false;
	}
	if (prot != VM_PROT_READ && prot != (VM_PROT_READ | VM_PROT_WRITE)) {
		return false;
	}
	if ((max_prot & (VM_PROT_READ | VM_PROT_WRITE)) !=
	    (VM_PROT_READ | VM_PROT_WRITE)) {
		return false;
	}
	if (max_prot & VM_PROT_ALLEXEC) {
		return false;
	}
	return true;
}

static inline bool
switch_vm_lazy_rw_table_add(struct switch_vm_lazy_rw_table *t,
    uint64_t start, uint64_t end)
{
	if (start >= end) {
		return true;
	}

	uint32_t lo = 0;
	while (lo < t->count && t->ranges[lo].end < start) {
		lo++;
	}

	uint32_t hi = lo;
	while (hi < t->count && t->ranges[hi].start <= end) {
		hi++;
	}

	uint64_t m_start = start;
	uint64_t m_end = end;
	if (lo < hi) {
		if (t->ranges[lo].start < m_start) {
			m_start = t->ranges[lo].start;
		}
		if (t->ranges[hi - 1].end > m_end) {
			m_end = t->ranges[hi - 1].end;
		}
	}

	uint32_t removed = hi - lo;
	uint32_t new_count = t->count - removed + 1u;
	if (new_count > SWITCH_VM_LAZY_RW_SOFT_MAX) {
		return false;
	}

	if (removed == 0u) {
		for (uint32_t i = t->count; i > lo; i--) {
			t->ranges[i] = t->ranges[i - 1u];
		}
	} else if (removed > 1u) {
		uint32_t shift = removed - 1u;
		for (uint32_t i = hi; i < t->count; i++) {
			t->ranges[i - shift] = t->ranges[i];
		}
	}

	t->ranges[lo].start = m_start;
	t->ranges[lo].end = m_end;
	t->count = new_count;
	return true;
}

static inline bool
switch_vm_lazy_rw_table_subtract_would_overflow(
    const struct switch_vm_lazy_rw_table *t, uint64_t start, uint64_t end)
{
	if (start >= end || t->count < SWITCH_VM_LAZY_RW_MAX) {
		return false;
	}
	for (uint32_t i = 0; i < t->count; i++) {
		if (t->ranges[i].start < start && end < t->ranges[i].end) {
			return true;
		}
	}
	return false;
}

static inline bool
switch_vm_lazy_rw_table_subtract(struct switch_vm_lazy_rw_table *t,
    uint64_t start, uint64_t end)
{
	if (start >= end || t->count == 0u) {
		return true;
	}

	for (uint32_t i = 0; i < t->count; i++) {
		if (t->ranges[i].start < start && end < t->ranges[i].end) {
			if (t->count >= SWITCH_VM_LAZY_RW_MAX) {
				return false;
			}
			for (uint32_t j = t->count; j > i + 1u; j--) {
				t->ranges[j] = t->ranges[j - 1u];
			}
			t->ranges[i + 1u].start = end;
			t->ranges[i + 1u].end = t->ranges[i].end;
			t->ranges[i].end = start;
			t->count += 1u;
			return true;
		}
	}

	uint32_t out = 0u;
	for (uint32_t i = 0; i < t->count; i++) {
		struct switch_vm_lazy_rw_range r = t->ranges[i];
		if (r.end <= start || r.start >= end) {
			t->ranges[out++] = r;
		} else if (r.start >= start && r.end <= end) {
			continue;
		} else if (r.start < start) {
			r.end = start;
			t->ranges[out++] = r;
		} else {
			r.start = end;
			t->ranges[out++] = r;
		}
	}
	t->count = out;
	return true;
}

static inline bool
switch_vm_lazy_rw_table_contains_page(const struct switch_vm_lazy_rw_table *t,
    uint64_t page_start, uint64_t page_end)
{
	if (page_start >= page_end) {
		return false;
	}
	for (uint32_t i = 0; i < t->count; i++) {
		if (t->ranges[i].end <= page_start) {
			continue;
		}
		if (t->ranges[i].start > page_start) {
			return false;
		}
		return page_end <= t->ranges[i].end;
	}
	return false;
}

/*
 * Selects the minimal slice to materialize on a write fault to [page_start, page_end).
 * Normally this is the single page itself; if splitting an interval would exceed the
 * soft cap, materialize the shorter side so subtraction does not increase interval count.
 */
static inline bool
switch_vm_lazy_rw_table_fault_slice(const struct switch_vm_lazy_rw_table *t,
    uint64_t page_start, uint64_t page_end,
    uint64_t *slice_start, uint64_t *slice_end)
{
	if (page_start >= page_end) {
		return false;
	}
	for (uint32_t i = 0; i < t->count; i++) {
		struct switch_vm_lazy_rw_range r = t->ranges[i];
		if (r.end <= page_start) {
			continue;
		}
		if (r.start > page_start || page_end > r.end) {
			return false;
		}
		if (r.start < page_start && page_end < r.end &&
		    t->count >= SWITCH_VM_LAZY_RW_SOFT_MAX) {
			if ((page_start - r.start) <= (r.end - page_end)) {
				*slice_start = r.start;
				*slice_end = page_end;
			} else {
				*slice_start = page_start;
				*slice_end = r.end;
			}
		} else {
			*slice_start = page_start;
			*slice_end = page_end;
		}
		return true;
	}
	return false;
}

static inline bool
switch_vm_lazy_rw_table_first_overlap(const struct switch_vm_lazy_rw_table *t,
    uint64_t start, uint64_t end, uint64_t *ov_start, uint64_t *ov_end)
{
	if (start >= end) {
		return false;
	}
	for (uint32_t i = 0; i < t->count; i++) {
		struct switch_vm_lazy_rw_range r = t->ranges[i];
		if (r.end <= start) {
			continue;
		}
		if (r.start >= end) {
			return false;
		}
		*ov_start = (r.start > start) ? r.start : start;
		*ov_end = (r.end < end) ? r.end : end;
		return true;
	}
	return false;
}

static inline bool
switch_vm_lazy_rw_table_first_range(const struct switch_vm_lazy_rw_table *t,
    uint64_t *r_start, uint64_t *r_end)
{
	if (t->count == 0u) {
		return false;
	}
	*r_start = t->ranges[0].start;
	*r_end = t->ranges[0].end;
	return true;
}

/*
 * Computes the virtual slice [slice_start, slice_end) of [entry_start, entry_end)
 * containing query_addr under the active deferred RW table, returning true if that
 * slice has a deferred write grant.
 */
static inline bool
switch_vm_lazy_rw_table_region_slice(const struct switch_vm_lazy_rw_table *t,
    uint64_t entry_start, uint64_t entry_end, uint64_t query_addr,
    uint64_t *slice_start, uint64_t *slice_end)
{
	uint64_t q = (query_addr > entry_start) ? query_addr : entry_start;
	uint64_t s = entry_start;

	for (uint32_t i = 0; i < t->count; i++) {
		struct switch_vm_lazy_rw_range r = t->ranges[i];
		if (r.end <= q) {
			if (r.end > s) {
				s = r.end;
			}
			continue;
		}
		if (r.start <= q) {
			*slice_start = (r.start > entry_start) ? r.start : entry_start;
			*slice_end = (r.end < entry_end) ? r.end : entry_end;
			return true;
		}
		*slice_start = s;
		*slice_end = (r.start < entry_end) ? r.start : entry_end;
		return false;
	}

	*slice_start = s;
	*slice_end = entry_end;
	return false;
}

#ifdef MACH_KERNEL_PRIVATE
extern uint32_t switch_vm_lazy_rw_global_active;
extern void switch_vm_lazy_rw_fault(vm_map_t map, vm_map_offset_t vaddr);
extern bool switch_vm_lazy_rw_page_active(vm_map_t map, vm_map_offset_t vaddr);
#endif

#endif
