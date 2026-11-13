/* SPDX-License-Identifier: MIT */
#ifndef SWITCH_GPU_BACKING_H
#define SWITCH_GPU_BACKING_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
	SWITCH_GPU_ENGINE_GM20B = 1,
	SWITCH_GPU_ENGINE_CE = 2,
	SWITCH_GPU_ENGINE_VIC = 3,
	SWITCH_GPU_ENGINE_NVDEC = 4,
	SWITCH_GPU_ENGINE_DISPLAY = 5,
	SWITCH_GPU_ENGINE_NVENC = 6,
	/* a writer promise (SwitchGpuWriterPromise.h): queue is its word's byte offset in the fence pool */
	SWITCH_GPU_ENGINE_PROMISE = 7,
	SWITCH_GPU_ENGINE_COUNT = 8,
	SWITCH_GPU_BACKING_READ = 1u << 0,
	SWITCH_GPU_BACKING_WRITE = 1u << 1,
	SWITCH_GPU_BACKING_CPU_AVOID_SYNC = 1u << 2,
	SWITCH_GPU_BACKING_CPU_DISCARD = 1u << 3,
	SWITCH_GPU_BACKING_CPU_NO_CACHE = 1u << 4,
	SWITCH_GPU_BACKING_CPU_CONCURRENT = 1u << 5,
	SWITCH_GPU_BACKING_CPU_PRESERVE_DIRTY = 1u << 6,
	SWITCH_GPU_BACKING_UNLISTED = 1u << 7,
	SWITCH_GPU_BACKING_NO_CPU_EXCLUSION = 1u << 8,
	/* a display read lease taken behind writer promises: no admission waits for it (SwitchGpuWriterPromise.h) */
	SWITCH_GPU_BACKING_PROMISED = 1u << 9,
	SWITCH_GPU_CACHE_CLEAN = 0,
	SWITCH_GPU_CACHE_INVALIDATE = 1,
	SWITCH_GPU_CACHE_CLEAN_INVALIDATE = 2,
	SWITCH_GPU_BACKING_GPU_DIRTY = 1u << 0,
	SWITCH_GPU_BACKING_CPU_DIRTY = 1u << 1,
	SWITCH_GPU_BACKING_PITCH = 0,
	SWITCH_GPU_BACKING_BLOCK_LINEAR = 1,
	SWITCH_GPU_BACKING_MAX_PLANES = 4,
};

/* writer promises a backing holds pending at once (SwitchGpuWriterPromise.h) */
#define SWITCH_GPU_BACKING_PROMISES 4

struct switch_gpu_backing;
struct switch_gpu_cpu_lease;

struct switch_gpu_backing_token {
	uint64_t value;
	uint64_t reset_generation;
	uint32_t engine;
	uint32_t queue;
};

struct switch_gpu_backing_plane {
	uint64_t offset;
	uint64_t bytes;
	uint32_t width, height, pitch, bytes_per_element;
	uint32_t layout, block_height_log2;
};

struct switch_gpu_backing_layout {
	uint32_t format, plane_count;
	uint32_t color_matrix, color_range, transfer, primaries;
	uint32_t chroma_top, chroma_bottom;
	struct switch_gpu_backing_plane planes[SWITCH_GPU_BACKING_MAX_PLANES];
};

struct switch_gpu_backing_info {
	uint64_t allocation_id, allocation_generation, content_generation;
	const uint64_t *pages;
	uint32_t page_count, surface_id, cpu_locks, dirty;
	struct switch_gpu_backing_layout layout;
};

/* The page vector remains valid until the lease is released. Registration takes the owner's supplied reference on
 * success; release_owner must release the actual pinned allocation, not only a surface wrapper. */
struct switch_gpu_backing *switch_gpu_backing_register(const uint64_t *pages, uint32_t count,
						       uint32_t surface_id, void *owner,
						       void (*release_owner)(void *));
struct switch_gpu_backing *switch_gpu_backing_find_pa(uint64_t first_page);
struct switch_gpu_backing *switch_gpu_backing_find_surface(uint32_t surface_id);
void switch_gpu_backing_retain(struct switch_gpu_backing *backing);
void switch_gpu_backing_release(struct switch_gpu_backing *backing);
/* The display scans the backing in place (a direct scanout slot) from add 1 to add 0: a job that writes it ends
 * External, so its completion means its writes are in DRAM and the display may flip to it with no present job. */
void switch_gpu_backing_scanout(struct switch_gpu_backing *backing, int add);
int switch_gpu_backing_scanned(const struct switch_gpu_backing *backing);
void switch_gpu_backing_describe(struct switch_gpu_backing *backing,
				 struct switch_gpu_backing_info *info);
int switch_gpu_backing_set_layout(struct switch_gpu_backing *backing,
				  const struct switch_gpu_backing_layout *layout);

/* status/wait: 1 completed, 0 pending, negative errno on failed work. retain_token pins the timeline's completion
 * record through channel destruction/reset; release_token balances it. value_bits is 32 or 64. */
struct switch_gpu_backing_engine {
	int (*status)(const struct switch_gpu_backing_token *);
	int (*wait)(const struct switch_gpu_backing_token *);
	int (*retain_token)(const struct switch_gpu_backing_token *);
	void (*release_token)(const struct switch_gpu_backing_token *);
	uint32_t value_bits;
};

int switch_gpu_backing_register_engine(uint32_t engine,
				       const struct switch_gpu_backing_engine *operations);
int switch_gpu_backing_token_retain(const struct switch_gpu_backing_token *token);
void switch_gpu_backing_token_release(const struct switch_gpu_backing_token *token);
int switch_gpu_backing_token_status(const struct switch_gpu_backing_token *token);
int switch_gpu_backing_token_wait(const struct switch_gpu_backing_token *token);
/* A host1x filter waiter's read, in any context (interrupts off included), once the token's syncpoint has reached its
 * fence: 1 when its job will complete with status 1, nothing but that fence left; 0 when not known yet or failed.
 * Engines that register none read 0. */
typedef int (*switch_gpu_backing_filter_status)(const struct switch_gpu_backing_token *);
int switch_gpu_backing_register_filter_status(uint32_t engine,
					      switch_gpu_backing_filter_status status);
int switch_gpu_backing_token_filter_status(const struct switch_gpu_backing_token *token);
struct switch_gpu_event_waits;

struct switch_gpu_backing_semantics {
	int (*snapshot)(const struct switch_gpu_backing_token *, struct switch_gpu_event_waits **);
	int (*physical_status)(const struct switch_gpu_backing_token *, int external);
	int (*pending)(void);
};

int switch_gpu_backing_register_semantics(uint32_t engine,
					  const struct switch_gpu_backing_semantics *operations);
int switch_gpu_backing_has_semantics(void);
int switch_gpu_backing_semantic_snapshot(const struct switch_gpu_backing_token *,
					 struct switch_gpu_event_waits **);
/* Retirement of the semantic authority itself must observe physical completion, avoiding a self-dependency. */
int switch_gpu_backing_token_physical_status(const struct switch_gpu_backing_token *, int external);

struct switch_gpu_backing_visibility {
	int (*status)(const struct switch_gpu_backing_token *);
	int (*wait)(const struct switch_gpu_backing_token *);
};

int switch_gpu_backing_register_visibility(uint32_t engine,
					   const struct switch_gpu_backing_visibility *operations);
/* External completion additionally promises DRAM visibility for CPU, host1x and display consumers. */
int switch_gpu_backing_token_external_status(const struct switch_gpu_backing_token *token);
int switch_gpu_backing_token_external_wait(const struct switch_gpu_backing_token *token);
/* Call after publishing an engine completion or failure, outside its timeline lock. */
void switch_gpu_backing_completed(void);

struct switch_gpu_backing_use {
	struct switch_gpu_backing *backing;
	uint32_t access, reserved;
};

struct switch_gpu_backing_write_range {
	struct switch_gpu_backing *backing;
	uint64_t offset, bytes; /* bytes zero denotes an unqualified whole-backing write. */
};

int switch_gpu_backing_cpu_track(struct switch_gpu_backing *backing);

enum {
	SWITCH_GPU_BACKING_STALE = -116,
	SWITCH_GPU_BACKING_DEPENDENCY_FAILED = -125
};

/* The caller retains the generation word through commit/abort. Its owner changes it only under the logical
 * backing's full CPU WRITE lease, including invalidation when replacing a descriptor's attachment. */
struct switch_gpu_backing_guard {
	struct switch_gpu_backing *backing;
	const volatile uint64_t *generation;
	uint64_t expected;
};

struct switch_gpu_backing_transaction {
	struct switch_gpu_backing_use *uses;
	uint32_t count, locked, prepared, host_written;
	struct switch_gpu_backing_token completion;
	/* switch_gpu_backing_promise_dependencies ran: the writer promises outside [promise_first, promise_end) its reads
	 * order behind */
	uint32_t promised, promise_first, promise_end;
	/* debug kernels: what begin left in `uses`, which commit and abort check before they trust it */
	uint64_t uses_check;
};

/* begin sorts/merges the caller's use array and locks its backings in allocation order. Keep those locks only through
 * dependency encoding and publication; every success must end with commit or abort. No token can be lost at commit.
 * The transaction keeps the caller's array, not a copy: commit and abort find the backings to record and unlock in it,
 * so it must stay as begin left it until then (an array in a frame that returns before the commit is a dangling one;
 * debug kernels refuse it with one line). A begin that fails leaves the transaction empty, and commit and abort of an
 * empty transaction do nothing.
 * Failed work fails no later use: its records retire as completed ones do, and what a failed writer wrote is undefined
 * (logged once). Nothing here, in dependencies, or in a CPU acquire, wait or snapshot returns its errno.
 * An UNLISTED use is one the engine cannot vouch for (a legacy submission's private BOs and unlisted imports): CPU
 * access and the kernel's other engines still order after the job, as after a writer when WRITE is set, but no GM20B
 * job waits for it, it waits for nothing and CPU owners never refuse it.
 * NO_CPU_EXCLUSION: CPU owners holding the backing now do not refuse the use (a GM20B user submission: no submission
 * ABI refuses for a CPU lock). An owner ordered itself after earlier work when it acquired; this job orders the next
 * acquire. While a CPU writer holds the backing every begin cleans what it wrote so far, and its release the rest. */
int switch_gpu_backing_begin(struct switch_gpu_backing_use *uses, uint32_t count,
			     const struct switch_gpu_backing_token *completion,
			     struct switch_gpu_backing_transaction *transaction);
/* Conversion leases cover whole allocations without directly exposing their bytes to the CPU. Keep every supplied
 * lease held through commit/abort; the view broker must await conversion before exposing the converted CPU view. */
int switch_gpu_backing_begin_conversion(struct switch_gpu_backing_use *uses, uint32_t count,
					struct switch_gpu_cpu_lease *const *leases,
					uint32_t lease_count,
					const struct switch_gpu_backing_token *completion,
					struct switch_gpu_backing_transaction *transaction);
int switch_gpu_backing_begin_checked(struct switch_gpu_backing_use *uses, uint32_t count,
				     const struct switch_gpu_backing_guard *guards,
				     uint32_t guard_count,
				     const struct switch_gpu_backing_token *completion,
				     struct switch_gpu_backing_transaction *transaction);
int switch_gpu_backing_begin_conversion_checked(struct switch_gpu_backing_use *uses, uint32_t count,
						struct switch_gpu_cpu_lease *const *leases,
						uint32_t lease_count,
						const struct switch_gpu_backing_guard *guards,
						uint32_t guard_count,
						const struct switch_gpu_backing_token *completion,
						struct switch_gpu_backing_transaction *transaction);
int switch_gpu_backing_dependencies(const struct switch_gpu_backing_transaction *transaction,
				    struct switch_gpu_backing_token *out, uint32_t capacity,
				    uint32_t *needed);
/* The transaction's uses' pending writer promises (SwitchGpuWriterPromise.h) but those in [own_first, own_end) (a
 * connection's own) as tokens after the *needed already in `out`, read under the locks the transaction holds on their
 * backings: *needed grows by them, -28 past capacity. The reader acquires them on the GPU, so its reads read what the
 * promised writes leave, and commit records them so: the promised job does not wait for them, which wait for it. */
/* A writer promise was given up on (SwitchGpuWriterPromise.h): CPU threads asleep behind one go on. Thread context, no
 * backing's lock held. */
void switch_gpu_backing_promise_sleepers_wake(void);
int switch_gpu_backing_promise_dependencies(struct switch_gpu_backing_transaction *transaction,
					    struct switch_gpu_backing_token *out, uint32_t capacity,
					    uint32_t *needed, uint32_t own_first, uint32_t own_end);
void switch_gpu_backing_commit(struct switch_gpu_backing_transaction *transaction);
void switch_gpu_backing_abort(struct switch_gpu_backing_transaction *transaction);
int switch_gpu_backing_write_ranges(struct switch_gpu_backing_transaction *transaction,
				    struct switch_gpu_backing_write_range *ranges, uint32_t count);

/* Readers wait for writers; writers wait for all users. DISCARD is valid only for a complete cache-line overwrite.
 * release cleans written lines before advancing content_generation and making the allocation available to engines.
 * NO_CACHE is ownership-only: callers that write cached bytes must explicitly publish those ranges.
 * PRESERVE_DIRTY requires READ|AVOID_SYNC and producers that cleaned CPU aliases before GPU writes. It preserves CPU
 * stores made after completed GPU work; clients without that producer contract must use ordinary invalidation. */
int switch_gpu_backing_cpu_acquire(struct switch_gpu_backing *backing, uint64_t offset,
				   uint64_t bytes, uint32_t access);
/* Sleeps until every engine write the backing records is in DRAM (an Internal GM20B job completes with its writes in the
 * L2), taking no CPU ownership: for a kernel CPU access the caller already orders against new GPU work. */
int switch_gpu_backing_wait_visible(struct switch_gpu_backing *backing);
int switch_gpu_backing_cpu_release(struct switch_gpu_backing *backing, uint64_t offset,
				   uint64_t bytes, uint32_t access);
int switch_gpu_backing_cpu_acquire_owned(struct switch_gpu_backing *backing, uint64_t offset,
					 uint64_t bytes, uint32_t access,
					 struct switch_gpu_cpu_lease **lease);
int switch_gpu_backing_cpu_release_owned(struct switch_gpu_cpu_lease **lease, int wrote);
/* Reserves full-allocation ownership for an engine conversion without waiting for already-published GPU users.
 * Only conversion transactions may discount this reservation; it never makes bytes directly CPU-accessible. */
int switch_gpu_backing_conversion_reserve(struct switch_gpu_backing *backing, uint32_t access,
					  int avoid, struct switch_gpu_cpu_lease **lease);
/* Pins the current published producer and excludes replacement without waiting for it. Engine consumers still use
 * the backing transaction; this lease cannot authorize a conversion write or direct CPU access to pending bytes. */
int switch_gpu_backing_export_read(struct switch_gpu_backing *backing,
				   struct switch_gpu_backing_token *producer,
				   struct switch_gpu_cpu_lease **lease);
/* A retained immutable physical view; a zero producer means current CPU-authored bytes, cleaned for engines. A surface
 * with writer promises pending (SwitchGpuWriterPromise.h) is snapshotted at once, behind a lease no admission of the
 * promised job waits for: the reader orders behind the promises on the GPU or does not show the frame from the CPU. */
int switch_gpu_backing_snapshot_read(struct switch_gpu_backing *backing,
				     struct switch_gpu_backing_token *producer,
				     struct switch_gpu_cpu_lease **lease);
/* The same with the pending promises as tokens the reader acquires on the GPU: their count (at most capacity), or an
 * error (-28: more are pending). */
int switch_gpu_backing_snapshot_read_promises(struct switch_gpu_backing *backing,
					      struct switch_gpu_backing_token *producer,
					      struct switch_gpu_cpu_lease **lease,
					      struct switch_gpu_backing_token *promises,
					      uint32_t capacity);
/* Writer promises (SwitchGpuWriterPromise.h): the engine registered once; a surface's ref to one added (-28: it has
 * SWITCH_GPU_BACKING_PROMISES pending) or dropped by the word's fence-pool offset; the tokens of the ones pending but
 * the asking connection's own (fence-pool offsets [own_first, own_end)), the count or -28; and how many are pending. */
struct switch_gpu_promise_ref;
int switch_gpu_promise_engine(void);
int switch_gpu_backing_promise_add(struct switch_gpu_backing *backing,
				   const struct switch_gpu_promise_ref *ref);
void switch_gpu_backing_promise_drop(struct switch_gpu_backing *backing, uint32_t offset);
int switch_gpu_backing_promise_tokens(struct switch_gpu_backing *backing, uint32_t own_first,
				      uint32_t own_end, struct switch_gpu_backing_token *tokens,
				      uint32_t capacity);
int switch_gpu_backing_promised(struct switch_gpu_backing *backing);
/* Thread-context admission: sleeps only for CPU writers; a GPU producer and a writer promise remain hardware
 * dependencies of the engine reader (switch_gpu_backing_promise_dependencies). */
int switch_gpu_backing_snapshot_read_wait(struct switch_gpu_backing *backing,
					  struct switch_gpu_backing_token *producer,
					  struct switch_gpu_cpu_lease **lease);
/* The same for a CPU reader, which cannot acquire a writer promise: it also sleeps until the promised job is submitted
 * (or the promise is given back), then names that job as the producer. */
int switch_gpu_backing_snapshot_read_cpu(struct switch_gpu_backing *backing,
					 struct switch_gpu_backing_token *producer,
					 struct switch_gpu_cpu_lease **lease);
/* Consumes a full owned NO_CACHE lease into an ordinary CPU reservation without dropping ownership or its backing pin.
 * It waits for conversion and establishes CPU cache visibility; READ leases cannot become writable. */
int switch_gpu_backing_cpu_transfer_owned(struct switch_gpu_backing *backing,
					  struct switch_gpu_cpu_lease **lease, uint32_t access);
/* Adopt descriptor CPU locks that began before this allocation was imported; never invalidate their live bytes. */
int switch_gpu_backing_cpu_adopt(struct switch_gpu_backing *backing, uint32_t readers,
				 uint32_t writers);
int switch_gpu_backing_cpu_publish(struct switch_gpu_backing *backing, uint64_t offset,
				   uint64_t bytes);
int switch_gpu_backing_cpu_publish_generation(struct switch_gpu_backing *backing,
					      struct switch_gpu_cpu_lease *lease,
					      volatile uint64_t *generation, uint64_t expected,
					      uint64_t next);
void switch_gpu_backing_cpu_cancel(struct switch_gpu_backing *backing, uint32_t access);
int switch_gpu_backing_wait(struct switch_gpu_backing *backing, uint32_t access);
/* Nothing waits: the engines (bit 1 << SWITCH_GPU_ENGINE_*) with recorded work still pending that a use of `access`
 * would follow, and the CPU owners (readers and writers, an export's read lease among them) holding the backing now. */
int switch_gpu_backing_pending(struct switch_gpu_backing *backing, uint32_t access,
			       uint32_t *engines, uint32_t *cpu_owners);

#ifdef __cplusplus
}
#endif
#endif
