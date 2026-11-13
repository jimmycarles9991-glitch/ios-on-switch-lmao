/* SPDX-License-Identifier: MIT */
/* The job interface: persistent resource sets and SubmitJob. The raw stack key does not hash this header, so no unit it
 * hashes (gpu/BUILD.bazel raw_stack, RawStackIncludes) may include it, and TegraGM20BABI.h never does. Records keep
 * TegraGM20BABI.h's rules: little endian, naturally aligned, reserved fields zero on input. */
#ifndef TEGRA_GM20B_JOB_ABI_H
#define TEGRA_GM20B_JOB_ABI_H
#include <stdint.h>
#include "TegraGM20BABI.h"

enum {
	TegraGM20BMethodResourceSetCreate = 31,
	TegraGM20BMethodResourceSetRelease = 32,
	TegraGM20BMethodSubmitJob = 33,
	TegraGM20BMethodWriterPromise = 34,

	TegraGM20BMaxResourceSets = 16,
	TegraGM20BMaxSetMembers = 4096,
	/* The connection's fence page (ChannelCreate's fence_token) holds each set's generation at this word plus its id
	 * minus one, so a client that gets kIOReturnNotReady can tell a stale set from a stale native view. */
	TegraGM20BSetGenerationWord = 768,
	/* Writer promises (method 34): promise i is released when the fence page's word TegraGM20BPromiseWord + i
	 * reaches its value */
	TegraGM20BPromiseWord = 512,
	TegraGM20BMaxPromises = 32,
	TegraGM20BMaxPromiseUses = 8,
	TegraGM20BMaxJobPromises = 4,
};

/* Method 31. Members are the BOs a SubmitJob names by set instead of by use, named as TegraGM20BAbiSurfaceUse records
 * (a handle, or with TegraGM20BSurfaceGpuAddress a VA in a live binding; the whole BO is the member). Write: the GPU may
 * write it; otherwise the set's jobs only read it. Only private memory can be a member: the connection's BOs and its
 * task's own imported pages. An IOSurface, a read-only import, a native view, a BO whose pages another connection or
 * engine reaches, a BO a CPU lease or CpuComplete ever named, and a BO in another set are refused: each comes back with
 * access 0 in the caller's records and the set holds the rest. A member a CPU lease or CpuComplete names later leaves the
 * set (its generation ends) with the set's jobs as its users; so does one a replace no longer lists.
 * set 0 makes a new set; a live set's id replaces its members and returns a new generation.
 * kIOReturnBadArgument for a record the kernel cannot take at all leaves the set as it was and names that record: the
 * records are copied back with its `reserved` holding a TegraGM20BSetRefused reason (the first such record only). */
struct TegraGM20BAbiResourceSetCreate {
	uint64_t uses_address;
	uint32_t count, set;
};

enum {
	TegraGM20BSetRefusedReserved = 1, /* its reserved word was not zero */
	TegraGM20BSetRefusedAccess =
		2, /* neither Read nor Write, or a flag other than GpuAddress */
	TegraGM20BSetRefusedGone =
		3, /* no BO of the connection has the handle (GpuAddress: no binding holds the VA) */
};

/* A method 31 record's refusal before its BO is looked up, 0 when its form is right */
static inline uint32_t switch_gpu_set_record_refusal(const struct TegraGM20BAbiSurfaceUse *use)
{
	const uint32_t access = (uint32_t)TegraGM20BSurfaceRead | (uint32_t)TegraGM20BSurfaceWrite;
	if (use->reserved)
		return TegraGM20BSetRefusedReserved;
	if (!(use->access & access) ||
	    (use->access & ~(access | (uint32_t)TegraGM20BSurfaceGpuAddress)))
		return TegraGM20BSetRefusedAccess;
	return 0;
}

struct TegraGM20BAbiResourceSetCreateReply {
	uint32_t set, generation;
	uint32_t fence_token,
		generation_offset; /* the generation's mirror: byte offset in the fence page */
};

/* Method 32. Later SubmitJobs naming the set are refused; its members' GPU work is unaffected. */
struct TegraGM20BAbiResourceSetRelease {
	uint32_t set, reserved;
};

/* What ends a set's generation, the kernel's counter index for each */
enum {
	TegraGM20BSetCauseFree = 0,   /* a member's BO was freed */
	TegraGM20BSetCauseLayout = 1, /* a member's layout changed */
	TegraGM20BSetCauseShared =
		2, /* a member's pages were reached from outside the set's connection */
	TegraGM20BSetCauseKill = 3,    /* a channel that used the set was killed */
	TegraGM20BSetCauseReplace = 4, /* method 31 replaced the members */
	TegraGM20BSetCauseCpu = 5,     /* a CPU lease or CpuComplete named a member */
	TegraGM20BSetCauses = 6,
};

/* TegraGM20BAbiJob.flags */
enum {
	TegraGM20BJobExternal = 1u
		<< 0, /* the job's writes reach DRAM before its fence (CPU, display, other engines) */
	TegraGM20BJobHostWritten = 1u
		<< 1, /* the CPU wrote memory the job reads through the GPU L2 since the last job */
	TegraGM20BJobHasEvents = 1u
		<< 2, /* a TegraGM20BAbiSubmitEvents and its uses follow the shared uses */
	/* the job's end raises no interrupt unless a waiter asks for one: a wait then gets it at or after the seqno, behind
	 * the channel's newest job. Ignored with HasEvents. */
	TegraGM20BJobQuiet = 1u << 3,
	/* A command buffer of several jobs holds its shared uses' admission from its first job to its last, so a CPU lock of
	 * any of them orders after the whole command buffer: Begin with the first job's uses, Continue (uses covered by
	 * Begin's), End with the last; Cancel (no pushes, no uses) gives it back after a refused job. A
	 * TegraGM20BAbiSubmitAdmission (its cookie: larger than the channel's last) follows the shared uses. */
	TegraGM20BJobAdmissionBegin = 1u << 4,
	TegraGM20BJobAdmissionContinue = 1u << 5,
	TegraGM20BJobAdmissionEnd = 1u << 6,
	TegraGM20BJobAdmissionCancel = 1u << 7,
	TegraGM20BJobAdmission = TegraGM20BJobAdmissionBegin | TegraGM20BJobAdmissionContinue |
		TegraGM20BJobAdmissionEnd | TegraGM20BJobAdmissionCancel,
	/* the job keeps writer promises (method 34): a TegraGM20BAbiJobPromises follows the admission record (or the
	 * shared uses), and the job's end releases them */
	TegraGM20BJobFulfills = 1u << 8,
};

/* Method 34, reply TegraGM20BAbiWriterPromiseReply. A command buffer counted as scheduled before its jobs reach the
 * kernel promises its writes of the IOSurfaces it names (TegraGM20BAbiSurfaceUse handles, Write): until the promise is
 * released, a present of one of them and every job of another connection that uses one wait for it on the GPU, and a
 * CPU lock of one waits for the job that keeps it. Make:
 * `uses_address`/`count`, `channel` the channel whose job will keep it; the reply names the promise. Cancel: `promise`,
 * released at once. A job that names it (TegraGM20BJobFulfills) releases it at its end; a kill of the channel it was
 * kept on and the connection's close release it too. */
enum {
	TegraGM20BPromiseMake = 0,
	TegraGM20BPromiseCancel = 1,
};

struct TegraGM20BAbiWriterPromise {
	uint64_t uses_address;
	uint32_t op, count;
	uint32_t promise, channel;
};

struct TegraGM20BAbiWriterPromiseReply {
	uint32_t promise, reserved;
};

struct TegraGM20BAbiJobPromises {
	uint32_t count, reserved;
	uint32_t promise[TegraGM20BMaxJobPromises];
};

/* Method 33, reply TegraGM20BAbiExecReply. Pushes and shared uses are as SubmitAddress's (TegraGM20BPushFence allowed);
 * push VAs are not resolved to BOs: pushes in GPU-cached memory need HostWritten after CPU writes.
 * With a set, the job runs after the latest job of each other channel of the connection that named the set, as an
 * acquire on that channel's fence word, and its members take no per-use work. kIOReturnNotReady: set_generation is not
 * the set's (its fence-page word says the current one), or a shared use's native view is stale. A member named as a
 * shared use is refused. Shared uses keep SubmitAddress's ownership exactly, CPU-lock exclusion included.
 * Order between two channels is the order their SubmitJobs return in; calls that overlap are not ordered by the set.
 * A job submitted without the set that uses a member is not ordered against the set's jobs. */
struct TegraGM20BAbiJob {
	uint32_t channel, push_count;
	uint32_t set, set_generation;
	uint32_t shared_count, flags;
};

enum {
	TegraGM20BMethodEventWord = 40,
	TegraGM20BSubmitEventReleases = 1u
		<< 12, /* TegraGM20BAbiSubmit.surface_flags (SubmitAddress) */
	TegraGM20BJobEventReleases = 1u << 12, /* TegraGM20BAbiJob.flags */
	TegraGM20BEventReleaseLimit = 1u << 31,
	/* The connection's fence page holds the word of each imported event whose handle's slot is below
	 * TegraGM20BEventWords at this word plus the slot: the words after the client bind epoch (256) and before the
	 * writer promises. Method 40 answers kIOReturnNoResources for the other slots, whose signals end their jobs. */
	TegraGM20BEventWordFirst = 257,
	TegraGM20BEventWords = TegraGM20BPromiseWord - TegraGM20BEventWordFirst,
};

/* Method 40. The word one channel of the connection releases an imported event's values into, mid-job: a signal of value v
 * goes to the GPU as payload = v - base (mod 2^64), floor < payload < TegraGM20BEventReleaseLimit, released at `address`
 * (NV906F SEMAPHORED, 4 bytes) after the wait for idle and the L2 flush a CPU reader needs, then NON_STALL_INTERRUPT.
 * The kernel completes such a signal once the word holds its payload, as the job's end completes the others, and other
 * jobs waiting for a value it produces acquire on the word. Each released payload is the channel's new floor. A call
 * while none of the word's signals is outstanding binds the word to `channel` and moves base to the event's value less
 * the word's (the word never goes back); otherwise it answers the current base and floor, or kIOReturnBusy while another
 * channel's are outstanding. kIOReturnNoResources: the word passed half its range. */
struct TegraGM20BAbiEventWord {
	uint64_t handle;
	uint32_t channel, reserved;
};

struct TegraGM20BAbiEventWordReply {
	uint64_t address, base;
	uint32_t floor, reserved;
};

/* With SubmitEventReleases or JobEventReleases, one per signal after the signals. Released: the job's pushes release
 * payload into the event's word for it (method 40); the submission is refused if payload is not that signal's. */
enum {
	TegraGM20BSignalReleased = 1u << 0
};

struct TegraGM20BAbiSignalRelease {
	uint32_t payload, flags;
};

#ifdef __cplusplus
#define TEGRA_GM20B_JOB_ASSERT(x, n) static_assert(sizeof(x) == (n), #x " layout")
#else
#define TEGRA_GM20B_JOB_ASSERT(x, n) _Static_assert(sizeof(struct x) == (n), #x " layout")
#endif
TEGRA_GM20B_JOB_ASSERT(TegraGM20BAbiResourceSetCreate, 16);
TEGRA_GM20B_JOB_ASSERT(TegraGM20BAbiResourceSetCreateReply, 16);
TEGRA_GM20B_JOB_ASSERT(TegraGM20BAbiResourceSetRelease, 8);
TEGRA_GM20B_JOB_ASSERT(TegraGM20BAbiJob, 24);
TEGRA_GM20B_JOB_ASSERT(TegraGM20BAbiWriterPromise, 24);
TEGRA_GM20B_JOB_ASSERT(TegraGM20BAbiWriterPromiseReply, 8);
TEGRA_GM20B_JOB_ASSERT(TegraGM20BAbiJobPromises, 24);
TEGRA_GM20B_JOB_ASSERT(TegraGM20BAbiEventWord, 16);
TEGRA_GM20B_JOB_ASSERT(TegraGM20BAbiEventWordReply, 24);
TEGRA_GM20B_JOB_ASSERT(TegraGM20BAbiSignalRelease, 8);
#undef TEGRA_GM20B_JOB_ASSERT
#endif
