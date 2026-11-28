// SPDX-License-Identifier: GPL-2.0-only
/*
 * What the flight recorder's three parts share (--//kernel:recorder). recorder.c takes triggers into the ask queue,
 * capture.c's thread call copies each ask's stackshot and ring into memory, and store.c's thread call compresses and
 * writes them. Ownership:
 *   - the ask queue, the refusals and the capture call's deadline: recorder.c's trigger lock (a spin lock, interrupts
 *     off, since triggers come from the scheduler tick and the display's work loop);
 *   - a capture: capture.c's from switch_recorder_store_begin to store.c's push, then the store's pending list (its
 *     mutex) until the store's thread call takes it off the list, then that call's alone until it is freed;
 *   - the store's slot table, deflate stream and output buffer: the store's thread call alone (THREAD_CALL_OPTIONS_ONCE:
 *     it never runs twice at once); the capture thread's deflate stream: that thread alone.
 */
#ifndef SWITCH_RECORDER_STATE_H
#define SWITCH_RECORDER_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "recorder.h"

struct switch_recorder_capture {
    struct switch_recorder_capture *next; /* the store's pending list */
    struct switch_recorder_head head;
    void *shot;
    size_t shot_alloc;
    void *ring; /* a RAW_VERSION1 header page, then head.ring_bytes - its size of events */
    size_t ring_alloc;
};

/* recorder.c */
bool switch_recorder_next(uint64_t now, struct switch_recorder_ask *out);
uint32_t switch_recorder_refusals(struct switch_recorder_ask *out, uint8_t *why, uint32_t max);
bool switch_recorder_ring_ours(void);
void switch_recorder_dumping_set(bool on);
void switch_recorder_count_taken(void);
uint64_t switch_recorder_expected(void);
void switch_recorder_expect_done(uint64_t tid);

/* capture.c */
bool switch_recorder_capture_init(void);
void switch_recorder_capture_call(void *p0, void *p1);

/* store.c */
bool switch_recorder_store_init(void);
uint32_t switch_recorder_store_begin(void);
bool switch_recorder_store_room(size_t bytes, bool mark);
void switch_recorder_store_unroom(size_t bytes);
void switch_recorder_store_push(struct switch_recorder_capture *c);
void switch_recorder_capture_free(struct switch_recorder_capture *c);
void switch_recorder_store_counts(struct switch_recorder_counts *n, struct switch_recorder_last *last);
bool switch_recorder_store_quiet(void);
void switch_recorder_store_abandon(struct switch_recorder_capture *c);

#endif
