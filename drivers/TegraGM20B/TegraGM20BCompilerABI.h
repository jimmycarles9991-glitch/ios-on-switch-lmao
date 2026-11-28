/* SPDX-License-Identifier: MIT */
/* The system shader compiler's rendezvous on a TegraGM20B connection (TegraGM20BABI.h's wire form, direct or tunnelled), the one
 * channel every GPU client's sandbox already admits. Kept out of TegraGM20BABI.h, which NVK and the GPU seed build from: the
 * method numbers are far from theirs, and a kernel without them answers kIOReturnUnsupported (no compiler: clients compile in
 * process).
 *
 * A client asks for a render state's record (Compile: its request, compile_request.h, in its own memory at
 * `address`, copied at the call): synchronously, the calling thread sleeping in the kernel until the request completes, or
 * with IOConnectCallAsyncStructMethod, whose callback gets TegraGM20BCompileAnswerWords words: the cookie, the result with
 * the generation in its high half, then TegraGM20BAbiCompileTimes' words in order. Each request completes exactly once, by
 * a compiler: one that arrives before any compiler registered waits for one (TegraGM20BCompilerExpected), and one a
 * compiler held when its connection closed is queued again for the next (twice: Failed). Unavailable: no compiler will come this boot, or the compiler did not take it. The record itself is never sent:
 * it lands in the store, which a client maps read only with Map (method 12) and TegraGM20BCompilerStoreToken. `generation`
 * names the store a completion is about; a client holding another generation's mapping maps the store again.
 *
 * The compiler (drivers/TegraGM20BMetal/compiler) registers its store with the kernel (Register: the mapping of its store
 * file in its own address space, which the kernel shares read only from then on) and then takes requests one at a time
 * (Take sleeps until one is queued) and completes them (Complete). Only a task holding the
 * com.brunostjohn.switch.shader-compiler entitlement registers, one at a time. It announces itself first (Announce), so
 * its connection closing before it registered counts as a decline; one that cannot serve says so (Decline): every
 * request waiting and every later one is Unavailable until a compiler registers. */
#ifndef TEGRA_GM20B_COMPILER_ABI_H
#define TEGRA_GM20B_COMPILER_ABI_H
#include <stdint.h>

enum {
	TegraGM20BMethodCompilerInfo = 0x5343, /* 'SC' */
	TegraGM20BMethodCompile = 0x5344,
	TegraGM20BMethodCompilerRegister = 0x5345,
	TegraGM20BMethodCompilerTake = 0x5346,
	TegraGM20BMethodCompilerComplete = 0x5347,
	TegraGM20BMethodCompilerDecline = 0x5348,
	TegraGM20BMethodCompilerAnnounce = 0x5349,
	TegraGM20BMethodCompilerServe = 0x534a,
	TegraGM20BMethodCompileWait = 0x534b,
	TegraGM20BMethodCompilerRegisterLibrary = 0x534c,
	TegraGM20BMethodCompilerMapLibrary = 0x534d,
	TegraGM20BMethodCompilerUnmapLibrary = 0x534e,

	TegraGM20BCompilerStoreToken =
		0x53430001, /* Map (method 12), read only: the registered store */

	/* TegraGM20BAbiCompilerInfoReply.flags */
	TegraGM20BCompilerCapable = 1u << 0,    /* every method here is implemented */
	TegraGM20BCompilerRegistered = 1u << 1, /* a compiler takes requests now */
	TegraGM20BCompilerStore = 1u << 2,      /* a store is registered (the token maps) */
	/* a request waits for a compiler to register; without it one that finds none is Unavailable */
	TegraGM20BCompilerExpected = 1u << 3,
	/* none comes this boot though the boot has one: it declined, or its launcher stopped starting it */
	TegraGM20BCompilerDeclined = 1u << 4,
	/* client library handles (RegisterLibrary/MapLibrary/UnmapLibrary) are implemented */
	TegraGM20BCompilerLibraries = 1u << 5,

	/* TegraGM20BAbiCompilerRegister.flags */
	TegraGM20BCompilerRestarts = 1u << 0,

	/* TegraGM20BAbiCompile.flags, TegraGM20BAbiCompilerTakeReply.flags */
	/* a thread waits for it: its class is the waiter's, not background */
	TegraGM20BCompileWaited = 1u << 0,

	/* results */
	TegraGM20BCompileMade = 0,
	TegraGM20BCompileRefused = 1, /* its refusal is in the store */
	TegraGM20BCompileFailed = 2,  /* the compile could not run; a later ask may */
	TegraGM20BCompileUnavailable =
		3, /* no compiler this boot, or it did not take the request (its store full, a request it cannot read) */

	/* TegraGM20BAbiCompilerComplete.how and TegraGM20BAbiCompileReply.how: bits 0-7 how the compiler answered, bits
	 * 8-15 the class (its pool's band, compiler_pool.h) it ran the compile at */
	TegraGM20BCompileCompiled = 0, /* this request's compile */
	/* another request's compile of the same record, in flight when it came */
	TegraGM20BCompileJoined = 1,
	TegraGM20BCompileStored = 2, /* the store held it already */

	/* a request carries a data library's bytes whole: as large as an app's metallib may be */
	TegraGM20BCompileRequestLimit = 256u << 20,
	TegraGM20BCompileAnswerWords = 6,
	TegraGM20BCompilePendingPerClient = 16,
	TegraGM20BCompilePending = 256,
	TegraGM20BCompileQueuedBytes = 512u << 20,
	TegraGM20BCompilerMaxWorkers = 8,
	TegraGM20BCompilerLibrariesPerClient = 64,
	TegraGM20BCompilerLibraryBytesPerClient = 256u << 20,
	TegraGM20BCompilerLibraryBytesTotal = 512u << 20,
};

/* The process a request is made for, as the kernel reads it in that process's own context: the compiler files a record
 * made from any library off the sealed system volume under it (drivers/TegraGM20BMetal/store_key.h), so a library one
 * process hands the compiler reaches no other process's records. Kind File: its main executable's volume UUID (16 bytes)
 * and file ID (8), which the filesystem assigned at install and no code signature states (an imported app's is made from
 * its IPA's own declarations, a bootstrap image's platform bit from its own). Kind Process, where no executable is known:
 * its pid, pid version and a number drawn once per boot. */
enum {
	TegraGM20BCompilerScopeFile = 1,
	TegraGM20BCompilerScopeProcess = 2,
	TegraGM20BCompilerScopeBytes = 256,
};

struct TegraGM20BAbiCompilerScope {
	uint32_t kind, bytes;
	uint8_t identity[TegraGM20BCompilerScopeBytes];
};

/* scope: the calling process's own */
struct TegraGM20BAbiCompilerInfoReply {
	uint64_t store_bytes, stack;
	uint32_t flags, generation;
	struct TegraGM20BAbiCompilerScope scope;
};

/* qos: the asking thread's qos_class_t (QOS_CLASS_UNSPECIFIED for a fixed-priority thread) */
struct TegraGM20BAbiCompile {
	uint64_t address, bytes, cookie, stack;
	uint32_t qos, flags;
};

/* Where an answered request's wait went, in CLOCK_UPTIME_RAW nanoseconds, as a debug compiler measures it (0 from a
 * release one, and for a record the store held): its compile's start and end, the compiler's Complete call, and the
 * answering thread's scheduler priority then. The kernel passes it on unread. */
struct TegraGM20BAbiCompileTimes {
	uint64_t compile_start, compile_end, answered;
	uint32_t answer_priority, reserved;
};

struct TegraGM20BAbiCompileReply {
	int32_t result;
	uint32_t generation, how, reserved;
	struct TegraGM20BAbiCompileTimes times;
};

/* flags: TegraGM20BCompilerRestarts, its launcher starts it again when it exits (launchd's KeepAlive, the BlueTool
 * stand-in's watch but for its last start); without it, or at the third loss in a row of a compiler that never
 * registered, a compiler's connection closing ends every request Unavailable */
struct TegraGM20BAbiCompilerAnnounce {
	uint32_t flags, reserved;
};

struct TegraGM20BAbiCompilerRegister {
	uint64_t store_address, store_bytes, stack;
	uint32_t flags, reserved;
};

struct TegraGM20BAbiCompilerRegisterReply {
	uint32_t generation, reserved;
};

/* Take: the oldest queued request's bytes into [buffer_address, + buffer_bytes). When they do not fit, the reply's `id` is 0
 * and `bytes` what they need (the request stays queued): a failed call would return no reply. */
struct TegraGM20BAbiCompilerTake {
	uint64_t buffer_address, buffer_bytes;
};

struct TegraGM20BAbiCompilerTakeReply {
	uint64_t id, bytes,
		client; /* client: the asking connection, the same for each of its requests while it is open */
	uint32_t qos, flags;
	int32_t pid;
	uint32_t reserved;
	struct TegraGM20BAbiCompilerScope scope; /* the asking process's, read when it asked */
};

struct TegraGM20BAbiCompilerComplete {
	uint64_t id;
	int32_t result;
	uint32_t how;
	struct TegraGM20BAbiCompileTimes times;
};

/* Serve (compiler only): register the calling worker in `worker` (id == 0), or bind taken request
 * `id` to the worker registered in `worker` (1..TegraGM20BCompilerMaxWorkers; 0 binds to caller). */
struct TegraGM20BAbiCompilerServe {
	uint64_t id;
	uint32_t worker, flags;
};

/* CompileWait (client): sleep on an in-flight asynchronous Compile belonging to the calling
 * connection with `cookie`, donating this thread's turnstile priority to its serving thread. */
struct TegraGM20BAbiCompileWait {
	uint64_t cookie;
	uint32_t flags, reserved;
};

/* RegisterLibrary (client): take a copy-on-write snapshot of [address, + bytes) in the calling
 * task and return a connection-scoped handle id. Released when the client connection closes. */
struct TegraGM20BAbiCompilerRegisterLibrary {
	uint64_t address, bytes;
	uint32_t flags, reserved;
};

struct TegraGM20BAbiCompilerRegisterLibraryReply {
	uint64_t id;
};

/* MapLibrary / UnmapLibrary (compiler only): map or unmap client `client`'s registered library
 * `id` read-only into the compiler task. */
struct TegraGM20BAbiCompilerMapLibrary {
	uint64_t id, client;
	uint32_t flags, reserved;
};

struct TegraGM20BAbiCompilerMapLibraryReply {
	uint64_t address, bytes;
};

struct TegraGM20BAbiCompilerUnmapLibrary {
	uint64_t id, client;
	uint32_t flags, reserved;
};

#endif
