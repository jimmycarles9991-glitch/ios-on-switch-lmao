// SPDX-License-Identifier: MIT
// What the spare pool (TegraGM20BUserClientSpares.cpp) holds of each slot (a kind and a class), and every decision it
// makes over that: the slot the refill makes next, which ready spares go back to make room, whether a dirty spare is
// kept, which ready spare a BoAlloc takes and what a miss teaches. The kernel keeps the lists and calls these under the
// pool's lock.
//
// One budget under the cap holds both kinds. A slot keeps its target, the larger of its warm count and the depth its
// misses taught. Room goes to demand by recency: a slot short of its target may take room from the ready spares of
// slots asked less recently than it, the least recently asked first. A warm count no BoAlloc has asked for yet fills
// only free room, and the compositor's reserved spare is never taken.
#pragma once
#include "SwitchGpuSpares.h"

constexpr uint32_t SWITCH_GPU_SPARE_SLOTS = SWITCH_GPU_SPARE_CLASSES * SWITCH_GPU_SPARE_KINDS;

constexpr uint32_t switch_gpu_spare_slot(uint32_t kind, uint32_t c)
{
	return kind * SWITCH_GPU_SPARE_CLASSES + c;
}

constexpr uint32_t switch_gpu_spare_slot_class(uint32_t slot)
{
	return slot % SWITCH_GPU_SPARE_CLASSES;
}

constexpr uint32_t switch_gpu_spare_slot_kind(uint32_t slot)
{
	return slot / SWITCH_GPU_SPARE_CLASSES;
}

constexpr uint64_t switch_gpu_spare_slot_bytes(uint32_t slot)
{
	return switch_gpu_spare_class_bytes(switch_gpu_spare_slot_class(slot));
}

// A slot's last ask: never asked, seeded from the previous boot's record, then this boot's asks in order
constexpr uint64_t SWITCH_GPU_SPARE_NEVER = 0, SWITCH_GPU_SPARE_SEEDED = 1;

struct switch_gpu_spare_books {
	uint16_t ready[SWITCH_GPU_SPARE_SLOTS];    // zeroed and clean
	uint16_t zeroing[SWITCH_GPU_SPARE_SLOTS];  // on the copy engine
	uint16_t dirty[SWITCH_GPU_SPARE_SLOTS];  // freed or newly made, waiting for the copy engine
	uint16_t depth
		[SWITCH_GPU_SPARE_SLOTS];  // what its misses taught (or the previous boot's record)
	uint16_t floor[SWITCH_GPU_SPARE_SLOTS];  // the warm table's count
	bool reserved[SWITCH_GPU_SPARE_SLOTS];
	uint64_t used[SWITCH_GPU_SPARE_SLOTS];  // its last ask, by the clock
	uint64_t clock;
	// Bytes of every spare above, in any state, plus one the refill is making: the cap bounds all of them, so a copy
	// engine too busy to zero cannot let the refill wire past it
	uint64_t held;
};

constexpr uint32_t SWITCH_GPU_SPARE_NONE = SWITCH_GPU_SPARE_SLOTS;

inline void switch_gpu_spare_books_init(switch_gpu_spare_books &b)
{
	b = {};
	b.clock = SWITCH_GPU_SPARE_SEEDED;
	for (const switch_gpu_spare_warm &w : SWITCH_GPU_SPARE_WARM) {
		const uint32_t s = switch_gpu_spare_slot(SWITCH_GPU_SPARE_CACHED,
							 switch_gpu_spare_class_up(w.bytes));
		b.floor[s] = w.ready;
		b.reserved[s] = w.reserved;
	}
}

// The class a spare of `bytes` serves: the largest class not past it
constexpr uint32_t switch_gpu_spare_class_of_spare(uint64_t bytes)
{
	const uint32_t c = switch_gpu_spare_class_up(bytes);
	return c < SWITCH_GPU_SPARE_CLASSES && switch_gpu_spare_class_bytes(c) == bytes ? c : c - 1;
}

inline uint16_t switch_gpu_spare_books_target(const switch_gpu_spare_books &b, uint32_t s)
{
	return b.depth[s] > b.floor[s] ? b.depth[s] : b.floor[s];
}

inline bool switch_gpu_spare_books_short(const switch_gpu_spare_books &b, uint32_t s)
{
	return switch_gpu_spare_short(b.ready[s], b.zeroing[s], b.dirty[s],
				      switch_gpu_spare_books_target(b, s));
}

// Ready spares of slot `v` that slot `s` may take the room of: none of its own, none of a slot asked as recently or
// later, and never the compositor's reserved last one
inline uint16_t switch_gpu_spare_books_evictable(const switch_gpu_spare_books &b, uint32_t s,
						 uint32_t v)
{
	if (v == s || b.used[v] >= b.used[s])
		return 0;
	return b.reserved[v] && b.ready[v] ? uint16_t(b.ready[v] - 1) : b.ready[v];
}

// Bytes slot `s` may take the room of
inline uint64_t switch_gpu_spare_books_older(const switch_gpu_spare_books &b, uint32_t s)
{
	uint64_t older = 0;
	for (uint32_t v = 0; v < SWITCH_GPU_SPARE_SLOTS; v++)
		older += uint64_t(switch_gpu_spare_books_evictable(b, s, v)) *
			 switch_gpu_spare_slot_bytes(v);
	return older;
}

// Whether `need` more bytes for slot `s` fit under `cap`, once what `s` may take the room of has gone back
inline bool switch_gpu_spare_books_fit(const switch_gpu_spare_books &b, uint32_t s, uint64_t need,
				       uint64_t cap)
{
	return b.held + need <= cap || b.held - switch_gpu_spare_books_older(b, s) + need <= cap;
}

// Of the ready spares slot `s` may take the room of that were asked at `age`, past their own slot's target (with
// `past`) or any: the smallest class that covers `short_bytes`, else the largest
inline uint32_t switch_gpu_spare_books_evict_of(const switch_gpu_spare_books &b, uint32_t s,
						uint64_t age, bool past, uint64_t short_bytes)
{
	uint32_t covers = SWITCH_GPU_SPARE_NONE, largest = SWITCH_GPU_SPARE_NONE;
	for (uint32_t v = 0; v < SWITCH_GPU_SPARE_SLOTS; v++) {
		if (!switch_gpu_spare_books_evictable(b, s, v) || b.used[v] != age ||
		    (past && b.ready[v] <= switch_gpu_spare_books_target(b, v)))
			continue;
		const uint64_t bytes = switch_gpu_spare_slot_bytes(v);
		if (bytes >= short_bytes && (covers == SWITCH_GPU_SPARE_NONE ||
					     bytes < switch_gpu_spare_slot_bytes(covers)))
			covers = v;
		if (largest == SWITCH_GPU_SPARE_NONE ||
		    bytes > switch_gpu_spare_slot_bytes(largest))
			largest = v;
	}
	return covers != SWITCH_GPU_SPARE_NONE ? covers : largest;
}

// The ready spare to give back next so that slot `s` gains `short_bytes` of room, one at a time: from the least
// recently asked slots it may take from, a spare past its own slot's target first (a freed BO nobody keeps), then the
// smallest class that covers what is short, so a 512 KiB shortfall costs no 24 MiB spare; SWITCH_GPU_SPARE_NONE when
// there is none
inline uint32_t switch_gpu_spare_books_evict(const switch_gpu_spare_books &b, uint32_t s,
					     uint64_t short_bytes)
{
	uint32_t oldest = SWITCH_GPU_SPARE_NONE;
	for (uint32_t v = 0; v < SWITCH_GPU_SPARE_SLOTS; v++)
		if (switch_gpu_spare_books_evictable(b, s, v) &&
		    (oldest == SWITCH_GPU_SPARE_NONE || b.used[v] < b.used[oldest]))
			oldest = v;
	if (oldest == SWITCH_GPU_SPARE_NONE)
		return oldest;
	const uint32_t past =
		switch_gpu_spare_books_evict_of(b, s, b.used[oldest], true, short_bytes);
	return past != SWITCH_GPU_SPARE_NONE
		       ? past
		       : switch_gpu_spare_books_evict_of(b, s, b.used[oldest], false, short_bytes);
}

// The slot the refill makes a spare of next: of the slots short of their target that fit under the cap (free room,
// or room they may take), the one asked most recently, the smallest class among equals; SWITCH_GPU_SPARE_NONE when
// none does. A slot takes room only from slots asked before it, so no two take each other's room and the refill stops.
inline uint32_t switch_gpu_spare_books_refill(const switch_gpu_spare_books &b, uint64_t cap)
{
	uint32_t best = SWITCH_GPU_SPARE_NONE;
	for (uint32_t c = 0; c < SWITCH_GPU_SPARE_CLASSES; c++) {
		for (uint32_t k = 0; k < SWITCH_GPU_SPARE_KINDS; k++) {
			const uint32_t s = switch_gpu_spare_slot(k, c);
			if ((best == SWITCH_GPU_SPARE_NONE || b.used[s] > b.used[best]) &&
			    switch_gpu_spare_books_short(b, s) &&
			    switch_gpu_spare_books_fit(b, s, switch_gpu_spare_slot_bytes(s), cap))
				best = s;
		}
	}
	return best;
}

// Whether the refill has a spare to make: after a take, the class taken from may be short while the asked one has no
// depth (backboardd's 17,280 KiB BOs, a 20 MiB class, are served by the 24 MiB class's spares)
inline bool switch_gpu_spare_books_wants_refill(const switch_gpu_spare_books &b, uint64_t cap)
{
	return switch_gpu_spare_books_refill(b, cap) != SWITCH_GPU_SPARE_NONE;
}

// Whether the copy engine zeroes a dirty spare of slot `s` (already off the dirty count, its bytes still held) rather
// than it going back to the VM: under the cap as it is, or, for a slot short of its target, once the room it may take
// has gone back. Freed BOs of slots nobody keeps a target of are kept only while they fit.
inline bool switch_gpu_spare_books_keep(const switch_gpu_spare_books &b, uint32_t s, uint64_t cap)
{
	return b.held <= cap ||
	       (switch_gpu_spare_books_short(b, s) && switch_gpu_spare_books_fit(b, s, 0, cap));
}

// The end of `slot`'s kind: a take or a miss looks no further, so the kinds never serve each other
constexpr uint32_t switch_gpu_spare_kind_end(uint32_t slot)
{
	return (switch_gpu_spare_slot_kind(slot) + 1) * SWITCH_GPU_SPARE_CLASSES;
}

// The slot a BoAlloc of slot `asked` takes a ready spare from, or SWITCH_GPU_SPARE_NONE. A client that is not the
// compositor leaves a reserved class's last ready spare.
inline uint32_t switch_gpu_spare_books_take(const switch_gpu_spare_books &b, uint32_t asked,
					    bool compositor)
{
	for (uint32_t d = asked; d < switch_gpu_spare_kind_end(asked) &&
				 switch_gpu_spare_serves(switch_gpu_spare_slot_class(asked),
							 switch_gpu_spare_slot_class(d));
	     d++) {
		if (!b.ready[d] || (!compositor && b.reserved[d] && b.ready[d] <= 1))
			continue;
		return d;
	}
	return SWITCH_GPU_SPARE_NONE;
}

// A BoAlloc asked slot `asked`, served from `served` (SWITCH_GPU_SPARE_NONE: a miss): both are asked now. A slot that
// serves a smaller class's BoAllocs is in use though nobody asks its own class.
inline void switch_gpu_spare_books_asked(switch_gpu_spare_books &b, uint32_t asked, uint32_t served)
{
	b.used[asked] = ++b.clock;
	if (served != SWITCH_GPU_SPARE_NONE)
		b.used[served] = b.clock;
}

// Why a BoAlloc of slot `asked` found no spare to take
inline switch_gpu_spare_miss switch_gpu_spare_books_why(const switch_gpu_spare_books &b,
							uint32_t asked, bool compositor,
							bool pressure)
{
	if (!pressure && !compositor) {
		for (uint32_t d = asked; d < switch_gpu_spare_kind_end(asked) &&
					 switch_gpu_spare_serves(switch_gpu_spare_slot_class(asked),
								 switch_gpu_spare_slot_class(d));
		     d++) {
			if (b.ready[d])
				return SWITCH_GPU_SPARE_RESERVED;
		}
	}
	return switch_gpu_spare_miss_of(switch_gpu_spare_books_target(b, asked),
					uint16_t(b.zeroing[asked] + b.dirty[asked]), pressure);
}

// A miss of slot `asked` teaches it a deeper target, within its cap and never below its warm count
inline void switch_gpu_spare_books_learn(switch_gpu_spare_books &b, uint32_t asked,
					 switch_gpu_spare_miss why, uint64_t learn_max_bytes,
					 uint64_t cap)
{
	const uint64_t bytes = switch_gpu_spare_slot_bytes(asked);
	const uint16_t learnt = switch_gpu_spare_depth_after_miss(
		switch_gpu_spare_books_target(b, asked), bytes <= learn_max_bytes, why);
	const uint16_t most = switch_gpu_spare_depth_cap(bytes, cap, b.floor[asked]);
	b.depth[asked] = learnt < most ? learnt : most;
}

// The previous boot's record of slot `s`: its depth from boot, asked before any slot this boot asks. A slot this boot
// has asked already keeps what it learned.
inline void switch_gpu_spare_books_seed(switch_gpu_spare_books &b, uint32_t s, uint16_t depth,
					uint64_t cap)
{
	if (b.used[s] != SWITCH_GPU_SPARE_NEVER)
		return;
	const uint16_t most =
		switch_gpu_spare_depth_cap(switch_gpu_spare_slot_bytes(s), cap, b.floor[s]);
	b.depth[s] = depth < most ? depth : most;
	b.used[s] = SWITCH_GPU_SPARE_SEEDED;
}
