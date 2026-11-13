// SPDX-License-Identifier: MIT
#pragma once
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <vector>

// GPU memory as the model sees it: a page table of 4 KiB GPU pages, each the CPU's view of the bytes behind it and
// whether its PTE is read-only. Only the stubbed vm-map entry points (tgm_vm_map_ordered, switch_gpu_vm_map,
// switch_gpu_vm_map_volatile, tgm_vm_kmap) fill it, at the addresses the driver asks for: a read of an unmapped page and a
// write through a read-only PTE are faults, as the GMMU raises them.
// The host's pushbuffer fetch goes through the GPU's L2 for a page mapped cacheable (not volatile): a line keeps what
// its first fetch read, whatever the CPU writes behind it, until invalidateL2() (MEM_OP L2_SYSMEM_INVALIDATE).
struct Memory {
	static constexpr uint64_t kPage = 4096, kLine = 128;

	// A page of a test's buffer (followed through the vector, which may reallocate) or of host memory.
	struct Pte {
		std::vector<uint8_t> *buffer;
		uint8_t *bytes;
		uint64_t offset, size;
		bool readOnly, cached;

		uint8_t *base() const
		{
			return buffer ? buffer->data() + offset : bytes;
		}

		uint64_t valid() const
		{
			return buffer ? (buffer->size() > offset
						 ? std::min(kPage, buffer->size() - offset)
						 : 0)
				      : size;
		}
	};

	std::map<uint64_t, Pte> ptes;                              // by GPU page number
	std::map<const uint8_t *, std::array<uint8_t, kLine>> l2;  // by the line's first byte
	std::map<const uint8_t *, uint64_t> dirty;  // l2's written lines: their valid bytes
	// One translation per direction: a copy alternates its reads and writes between two surfaces
	uint64_t lastPage[2] = {~0ull, ~0ull};
	const Pte *lastPte[2] = {};
	const Pte *last = nullptr;

	void map(uint64_t va, uint8_t *bytes, uint64_t size, bool readOnly, bool cached = false)
	{
		EXPECT_EQ(va % kPage, 0u) << "a GPU mapping starts on a page";
		last = lastPte[0] = lastPte[1] = nullptr;
		for (uint64_t at = 0; at < size; at += kPage)
			ptes[(va + at) / kPage] = {nullptr,  bytes + at,
						   0,        std::min(kPage, size - at),
						   readOnly, cached};
	}

	void map(uint64_t va, std::vector<uint8_t> &buffer, bool readOnly = false,
		 bool cached = false)
	{
		EXPECT_EQ(va % kPage, 0u) << "a GPU mapping starts on a page";
		last = lastPte[0] = lastPte[1] = nullptr;
		for (uint64_t at = 0; at < buffer.size(); at += kPage)
			ptes[(va + at) / kPage] = {&buffer, nullptr, at, 0, readOnly, cached};
	}

	void invalidateL2()
	{
		for (auto line = l2.begin(); line != l2.end();)
			line = dirty.count(line->first) ? std::next(line) : l2.erase(line);
	}

	// An engine's store through its GPU address. A cacheable page's line takes it and stays dirty, out of the CPU's sight,
	// until flushL2() (MEM_OP L2_FLUSH_DIRTY) writes it to the bytes behind it. false: a fault.
	bool store(uint64_t va, const uint8_t *data, uint64_t size)
	{
		for (uint64_t i = 0; i < size; ++i) {
			const uint64_t address = va + i, pageOffset = address % kPage;
			uint8_t *byte = at(address, true);
			if (!byte)
				return false;
			if (!last->cached) {
				*byte = data[i];
				continue;
			}
			const uint64_t lineOffset = pageOffset % kLine;
			const uint8_t *first = byte - lineOffset;
			throughL2(byte, pageOffset);
			l2[first][lineOffset] = data[i];
			dirty[first] = std::min(kLine, last->valid() - (pageOffset - lineOffset));
		}
		return true;
	}

	void flushL2()
	{
		for (const auto &[first, valid] : dirty)
			std::copy_n(l2[first].begin(), valid, const_cast<uint8_t *>(first));
		dirty.clear();
	}

	void unmap(uint64_t va, uint64_t size)
	{
		last = lastPte[0] = lastPte[1] = nullptr;
		for (uint64_t at = 0; at < size; at += kPage)
			ptes.erase((va + at) / kPage);
	}

	bool mapped(uint64_t va) const
	{
		return ptes.count(va / kPage) != 0;
	}

	bool readOnly(uint64_t va) const
	{
		const auto it = ptes.find(va / kPage);
		return it != ptes.end() && it->second.readOnly;
	}

	// nullptr: a fault (no PTE, past the buffer, or a write through a read-only PTE)
	uint8_t *at(uint64_t va, bool write = false)
	{
		const uint64_t page = va / kPage, offset = va % kPage;
		if (page != lastPage[write] || !lastPte[write]) {
			const auto it = ptes.find(page);
			if (it == ptes.end())
				return nullptr;
			lastPage[write] = page;
			lastPte[write] = &it->second;
		}
		last = lastPte[write];
		if (offset >= last->valid() || (write && last->readOnly))
			return nullptr;
		return last->base() + offset;
	}

	// A pushbuffer as the GPU's host fetches it: through its GPU address. false: a fault.
	bool fetch(uint64_t va, uint32_t dwords, std::vector<uint32_t> &words)
	{
		words.resize(dwords);
		for (uint32_t i = 0; i < dwords; ++i) {
			const uint64_t address = va + uint64_t(i) * 4;
			const uint8_t *word = at(address);
			if (!word || address % kPage > kPage - 4)
				return false;
			if (last->cached)
				word = throughL2(word, address % kPage);
			__builtin_memcpy(&words[i], word, 4);
		}
		return true;
	}

private:
	const uint8_t *throughL2(const uint8_t *byte, uint64_t pageOffset)
	{
		const uint64_t lineOffset = pageOffset % kLine;
		const uint8_t *first = byte - lineOffset;
		auto line = l2.find(first);
		if (line == l2.end()) {
			line = l2.emplace(first, std::array<uint8_t, kLine>{}).first;
			std::copy_n(first,
				    std::min(kLine, last->valid() - (pageOffset - lineOffset)),
				    line->second.begin());
		}
		return line->second.data() + lineOffset;
	}
};
