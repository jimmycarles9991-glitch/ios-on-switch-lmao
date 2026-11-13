// SPDX-License-Identifier: MIT
// A write-combined fence page as T210 has it, for host tests: only a plain store completes there, and an exclusive (a
// compare-and-swap, any atomic read-modify-write) is answered with an SError. The model's page is read-only, so any
// write but its own store() faults; the code under test's single store goes through store().
#pragma once
#include <cstddef>
#include <cstdint>
#include <sys/mman.h>
#include <unistd.h>

namespace fence_model
{
struct Page {
	volatile uint32_t *page = nullptr;
	size_t bytes = size_t(getpagesize());
	uint32_t owner = 1;   // tgm_fence_owner
	bool reused = false;  // handed to a connection that made no promise on it yet

	Page()
	{
		void *made =
			mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		page = static_cast<volatile uint32_t *>(made);
		mprotect(made, bytes, PROT_READ);
	}

	~Page()
	{
		munmap(const_cast<uint32_t *>(page), bytes);
	}

	void set(uint32_t offset, uint32_t value)
	{
		store(&page[offset / 4], value);
	}

	// its connection closed, the page goes to another, cleared (tgm_fence_page_get)
	void reuse()
	{
		for (size_t i = 0; i < bytes / 4; ++i)
			store(&page[i], 0);
		++owner;
		reused = true;
	}

	void store(volatile uint32_t *word, uint32_t value)
	{
		mprotect(const_cast<uint32_t *>(page), bytes, PROT_READ | PROT_WRITE);
		__atomic_store_n(word, value, __ATOMIC_RELEASE);
		mprotect(const_cast<uint32_t *>(page), bytes, PROT_READ);
	}
};
}  // namespace fence_model
