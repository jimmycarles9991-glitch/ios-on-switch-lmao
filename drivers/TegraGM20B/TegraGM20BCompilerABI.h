/* SPDX-License-Identifier: MIT */
/* The system shader compiler's rendezvous on a TegraGM20B connection (TegraGM20BABI.h's wire form, direct or tunnelled), the one
 * channel every GPU client's sandbox already admits. Kept out of TegraGM20BABI.h, which NVK and the GPU seed build from: the
 * method numbers are far from theirs, and a kernel without them answers kIOReturnUnsupported (no compiler: clients compile in
 * process).
 *
 * A client asks for a render state's record (Compile: its request, raw/runtime/raw_compile_request.h, in its own memory at
 * `address`, copied at the call): synchronously, the calling thread sleeping in the kernel until the request completes, or
 * with IOConnectCallAsyncStructMethod, whose callback gets two words: the cookie, and the result with the generation in its
 * high half. Each request completes exactly once, by a compiler: one that arrives before any compiler registered waits for
 * one (TegraGM20BCompilerExpected), and one a compiler held when its connection closed is queued again for the next (twice:
 * Failed). Unavailable: no compiler will come this boot, or the compiler did not take it. The record itself is never sent:
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
	TegraGM20BCompilePendingPerClient = 16,
	TegraGM20BCompilePending = 256,
	TegraGM20BCompileQueuedBytes = 512u << 20,
};

struct TegraGM20BAbiCompilerInfoReply {
	uint64_t store_bytes, stack;
	uint32_t flags, generation;
};

/* qos: the asking thread's qos_class_t (QOS_CLASS_UNSPECIFIED for a fixed-priority thread) */
struct TegraGM20BAbiCompile {
	uint64_t address, bytes, cookie, stack;
	uint32_t qos, flags;
};

struct TegraGM20BAbiCompileReply {
	int32_t result;
	uint32_t generation, how, reserved;
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
};

struct TegraGM20BAbiCompilerComplete {
	uint64_t id;
	int32_t result;
	uint32_t how;
};

#endif
