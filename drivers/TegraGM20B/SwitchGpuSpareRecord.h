// SPDX-License-Identifier: MIT
// One boot's spare demand, kept for the next boot to seed its depths from: for each slot that boot asked (or that served
// an ask), its target then (its warm count or what its misses taught), one byte a slot in slot order, so no slot is ever
// left out, behind a header naming this build's class table. A warm class that served the boot is recorded too, so the
// next boot's seeded classes do not take its room as if nobody had asked. The pool keeps it in NVRAM
// (TegraGM20BUserClientSpareRecord.cpp), so it reaches the card when SwitchNVRAM writes the bank at sleep or shutdown.
//
// Its budget: SWITCH_GPU_SPARE_RECORD_BYTES of the native ADT's 2 KiB common partition, which boot-args shares. A full
// partition refuses the set (the CHRP handler's kIOReturnNoSpace), the next boot's first asks miss, and no other
// variable loses room.
#pragma once
#include "SwitchGpuSpareBooks.h"

constexpr uint8_t SWITCH_GPU_SPARE_RECORD_VERSION = 2;
// version, kinds, classes, 0, then the class table's sizes in KiB summed (32 bits, little-endian)
constexpr uint32_t SWITCH_GPU_SPARE_RECORD_HEADER = 8;
constexpr uint32_t SWITCH_GPU_SPARE_RECORD_BYTES =
	SWITCH_GPU_SPARE_RECORD_HEADER + SWITCH_GPU_SPARE_SLOTS;
static_assert(SWITCH_GPU_SPARE_RECORD_BYTES <= 128,
	      "the record's budget in the 2 KiB NVRAM partition");

constexpr uint32_t switch_gpu_spare_record_table_sum()
{
	uint32_t sum = 0;
	for (uint32_t c = 0; c < SWITCH_GPU_SPARE_CLASSES; c++)
		sum += uint32_t(switch_gpu_spare_class_bytes(c) >> 10);
	return sum;
}

inline void switch_gpu_spare_record_header(uint8_t *out)
{
	const uint32_t sum = switch_gpu_spare_record_table_sum();
	out[0] = SWITCH_GPU_SPARE_RECORD_VERSION;
	out[1] = uint8_t(SWITCH_GPU_SPARE_KINDS);
	out[2] = uint8_t(SWITCH_GPU_SPARE_CLASSES);
	out[3] = 0;
	for (uint32_t i = 0; i < 4; i++)
		out[4 + i] = uint8_t(sum >> (8 * i));
}

// This boot's record into `out` (SWITCH_GPU_SPARE_RECORD_BYTES): its length. A slot this boot did not ask reads 0, so a
// seed it no longer needs ends after one boot.
inline uint32_t switch_gpu_spare_record_encode(const switch_gpu_spare_books &b, uint8_t *out)
{
	switch_gpu_spare_record_header(out);
	for (uint32_t s = 0; s < SWITCH_GPU_SPARE_SLOTS; s++) {
		const uint16_t target = switch_gpu_spare_books_target(b, s);
		out[SWITCH_GPU_SPARE_RECORD_HEADER + s] =
			b.used[s] > SWITCH_GPU_SPARE_SEEDED ? uint8_t(target < 255 ? target : 255)
							    : 0;
	}
	return SWITCH_GPU_SPARE_RECORD_BYTES;
}

// Seeds the books from a record: the number of slots seeded, 0 for a record whose length or header is not this build's
inline uint32_t switch_gpu_spare_record_seed(switch_gpu_spare_books &b, const uint8_t *record,
					     uint32_t length, uint64_t cap)
{
	uint8_t header[SWITCH_GPU_SPARE_RECORD_HEADER];
	switch_gpu_spare_record_header(header);
	if (length != SWITCH_GPU_SPARE_RECORD_BYTES)
		return 0;
	for (uint32_t i = 0; i < SWITCH_GPU_SPARE_RECORD_HEADER; i++)
		if (record[i] != header[i])
			return 0;
	uint32_t seeded = 0;
	for (uint32_t s = 0; s < SWITCH_GPU_SPARE_SLOTS; s++) {
		const uint8_t depth = record[SWITCH_GPU_SPARE_RECORD_HEADER + s];
		if (!depth || depth > SWITCH_GPU_SPARE_DEPTH_MAX)
			continue;
		switch_gpu_spare_books_seed(b, s, depth, cap);
		seeded++;
	}
	return seeded;
}
