// SPDX-License-Identifier: MIT
// The scanout read-back (TegraGM20BReadbackABI.h), debug kernels only. Its two buffers are wired kernel memory that the
// present's address space maps at kReadbackVa (above the present's targets and surfaces, TegraGM20BUserClient.cpp) and
// the arming connection maps into its task with method 12. They are debug one-shots, made at Arm and dropped at Disarm.
// The GPU does not snoop the CPU's caches: they are cleaned and invalidated over the buffers at Arm (no dirty line may be
// written back over the copy later) and again when a query first finds the read-back done (lines the CPU fetched
// meanwhile are stale).
#include "TegraGM20BReadback.h"

#if TGM_STATS
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <sys/errno.h>

namespace
{
constexpr uint64_t kReadbackVa = 0x50000000, kReadbackSpan = 8ULL << 20;
constexpr uint64_t kReadbackBytes = 4ULL << 20;  // a 1280 x 720 frame of 4-byte pixels fits
constexpr uint32_t kBuffers = 2;
constexpr uint32_t kTokens[kBuffers] = {
	0xfe000002u, 0xfe000003u};  // beside kFenceToken; BO tokens count up from 1

struct Buffer {
	IOBufferMemoryDescriptor *memory;
	bool prepared, mapped;
	uint64_t *pas;
	uint32_t npages;
	tgm_mem *mem;
};

struct Readback {
	const void *owner;  // null after the arming connection closed with its present in flight
	Buffer buffers[kBuffers];
	bool invalidated, live;
	// Written by the display's path while in flight, read once the state says done or failed.
	int state, status;
	uint32_t width, height, flags, previousSequence;
	uint64_t sequence;
};

// What window A scans for the frame offered last: its surface on direct scanout, else the scanout buffer its copy
// wrote. Written by the display's path only, whose offers come one at a time (gPresentLock).
struct Shown {
	tgm_present_read read;
	uint64_t sequence;
};

Readback gReadback;
Shown gShown;
IOLock *gReadbackLock;

IOLock *readbackLock()
{
	if (!__atomic_load_n(&gReadbackLock, __ATOMIC_ACQUIRE)) {
		IOLock *fresh = IOLockAlloc();
		// NOLINTNEXTLINE(misc-const-correctness): the compare-and-swap writes the current value back into it
		IOLock *expected = nullptr;
		if (fresh && !__atomic_compare_exchange_n(&gReadbackLock, &expected, fresh, false,
							  __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			IOLockFree(fresh);
	}
	return __atomic_load_n(&gReadbackLock, __ATOMIC_ACQUIRE);
}

uint64_t bufferVa(uint32_t i)
{
	return kReadbackVa + i * kReadbackSpan;
}

void dropBuffer(Buffer &b, uint32_t i)
{
	if (b.mapped)
		tgm_present_unmap(bufferVa(i), kReadbackBytes);
	tgm_mem_del(&b.mem);
	if (b.pas)
		IODelete(b.pas, uint64_t, b.npages);
	if (b.prepared)
		b.memory->complete();
	if (b.memory)
		b.memory->release();
	b = {};
}

bool makeBuffer(Buffer &b, uint32_t i)
{
	b.memory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut,
							       kReadbackBytes, PAGE_SIZE);
	b.prepared = b.memory != nullptr && b.memory->prepare() == kIOReturnSuccess;
	b.npages = static_cast<uint32_t>(kReadbackBytes >> 12);
	b.pas = b.prepared ? IONew(uint64_t, b.npages) : nullptr;
	if (!b.pas)
		return false;
	for (uint32_t p = 0; p < b.npages; p++) {
		IOByteCount len = 0;
		const addr64_t pa = b.memory->getPhysicalSegment(static_cast<uint64_t>(p) << 12,
								 &len, kIOMemoryMapperNone);
		// the GPU addresses 34-bit physical memory (TegraGM20BUserClient.cpp collectPages)
		if (!pa || (pa & 0xfff) || pa + 4096 > (1ULL << 34))
			return false;
		b.pas[p] = pa;
	}
	tgm_vm *vm = nullptr;
	if (tgm_present_open(&vm) || tgm_mem_new(vm, b.pas, b.npages, &b.mem))
		return false;
	b.mapped = tgm_present_map(bufferVa(i), b.mem, kReadbackBytes, 0) == 0;
	return b.mapped;
}

void dropBuffers()
{
	for (uint32_t i = 0; i < kBuffers; i++)
		dropBuffer(gReadback.buffers[i], i);
}

// Gives up an armed read-back so the display's path can no longer take it: false while its present is in flight.
bool idle()
{
	int armed = TegraGM20BReadbackArmed;
	__atomic_compare_exchange_n(&gReadback.state, &armed, TegraGM20BReadbackIdle, false,
				    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
	return armed != TegraGM20BReadbackInFlight;
}

void reapOrphan()
{
	if (!gReadback.owner && gReadback.buffers[0].memory && idle()) {
		dropBuffers();
		__atomic_store_n(&gReadback.state, TegraGM20BReadbackIdle, __ATOMIC_RELEASE);
	}
}

IOReturn arm(const void *owner, bool live)
{
	Readback &r = gReadback;
	if (r.owner && r.owner != owner)
		return kIOReturnBusy;
	if (__atomic_load_n(&r.state, __ATOMIC_ACQUIRE) == TegraGM20BReadbackInFlight)
		return kIOReturnBusy;
	for (uint32_t i = 0; i < kBuffers; i++) {
		if (!r.buffers[i].memory && !makeBuffer(r.buffers[i], i)) {
			dropBuffers();
			return kIOReturnNoMemory;
		}
	}
	for (const Buffer &b : r.buffers)
		tgm_cpu_clean_pages(b.pas, b.npages);
	r.owner = owner;
	r.invalidated = false;
	r.live = live;
	r.status = 0;
	__atomic_store_n(&r.state, TegraGM20BReadbackArmed, __ATOMIC_RELEASE);
	return kIOReturnSuccess;
}

IOReturn disarm(const void *owner)
{
	if (gReadback.owner != owner)
		return gReadback.owner ? kIOReturnNotPermitted : kIOReturnSuccess;
	if (!idle())
		return kIOReturnBusy;
	dropBuffers();
	gReadback.owner = nullptr;
	__atomic_store_n(&gReadback.state, TegraGM20BReadbackIdle, __ATOMIC_RELEASE);
	return kIOReturnSuccess;
}

void query(const void *owner, TegraGM20BAbiReadbackReply *out)
{
	Readback &r = gReadback;
	const int state = __atomic_load_n(&r.state, __ATOMIC_ACQUIRE);
	*out = {};
	out->state = state;
	if (state != TegraGM20BReadbackDone && state != TegraGM20BReadbackFailed)
		return;
	out->status = r.status;
	out->width = r.width;
	out->height = r.height;
	out->flags = r.flags;
	out->previous_sequence = r.previousSequence;
	out->sequence = r.sequence;
	if (state != TegraGM20BReadbackDone || r.owner != owner)
		return;
	if (!r.invalidated) {
		for (const Buffer &b : r.buffers)
			tgm_cpu_clean_pages(b.pas, b.npages);
		r.invalidated = true;
	}
	out->framebuffer_token = kTokens[0];
	out->scanout_token = kTokens[1];
}
}  // namespace

IOReturn tgm_readback_call(const void *owner, const TegraGM20BAbiReadback *in,
			   TegraGM20BAbiReadbackReply *out)
{
	if (in->reserved > 1 || (in->reserved && in->op != TegraGM20BReadbackArm))
		return kIOReturnBadArgument;
	IOLock *lock = readbackLock();
	if (!lock)
		return kIOReturnNoMemory;
	IOLockLock(lock);
	reapOrphan();
	IOReturn ret = kIOReturnSuccess;
	switch (in->op) {
	case TegraGM20BReadbackArm:
		ret = arm(owner, in->reserved == 1);
		break;
	case TegraGM20BReadbackDisarm:
		ret = disarm(owner);
		break;
	case TegraGM20BReadbackQuery:
		break;
	default:
		ret = kIOReturnBadArgument;
		break;
	}
	if (!ret)
		query(owner, out);
	IOLockUnlock(lock);
	return ret;
}

IOMemoryDescriptor *tgm_readback_memory(const void *owner, uint32_t token)
{
	if (token != kTokens[0] && token != kTokens[1])
		return nullptr;
	IOLock *lock = readbackLock();
	if (!lock)
		return nullptr;
	IOLockLock(lock);
	IOBufferMemoryDescriptor *memory = nullptr;
	if (gReadback.owner == owner &&
	    __atomic_load_n(&gReadback.state, __ATOMIC_ACQUIRE) == TegraGM20BReadbackDone) {
		memory = gReadback.buffers[token == kTokens[0] ? 0 : 1].memory;
		if (memory)
			memory->retain();
	}
	IOLockUnlock(lock);
	return memory;
}

void tgm_readback_closed(const void *owner)
{
	IOLock *lock = readbackLock();
	if (!lock)
		return;
	IOLockLock(lock);
	if (gReadback.owner == owner) {
		gReadback.owner = nullptr;
		reapOrphan();  // else its present is in flight: the next call reaps it
	}
	IOLockUnlock(lock);
}

uint32_t tgm_readback_take(const TegraScanoutCopy *copy, uint64_t source, int32_t sourceBlockLog2,
			   uint64_t target, tgm_present_read (&reads)[TGM_PRESENT_READS])
{
	Readback &r = gReadback;
	const bool direct = (copy->flags & TEGRA_SCANOUT_COPY_FENCE_ONLY) != 0u;
	const bool sourceBlock = sourceBlockLog2 >= 0;
	const uint32_t pitch = copy->width * 4;
	const Shown before = gShown;
	gShown.read =
		direct ? tgm_present_read{source,
					  0,
					  sourceBlock ? pitch
						      : static_cast<uint32_t>(copy->sourceStride),
					  pitch,
					  copy->width,
					  copy->height,
					  sourceBlockLog2,
					  0}
		       : tgm_present_read{target,
					  0,
					  copy->targetPitch,
					  pitch,
					  copy->width,
					  copy->height,
					  static_cast<int32_t>(copy->blockHeightLog2),
					  0};
	gShown.sequence = copy->sequence;
	int armed = TegraGM20BReadbackArmed;
	if (__atomic_load_n(&r.state, __ATOMIC_RELAXED) != armed ||
	    !__atomic_compare_exchange_n(&r.state, &armed, TegraGM20BReadbackInFlight, false,
					 __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return 0;
	if (static_cast<uint64_t>(pitch) * copy->height > kReadbackBytes) {
		r.status = EFBIG;
		__atomic_store_n(&r.state, TegraGM20BReadbackFailed, __ATOMIC_RELEASE);
		return 0;
	}
	r.width = copy->width;
	r.height = copy->height;
	r.sequence = copy->sequence;
	r.flags = (direct ? TegraGM20BReadbackDirect : 0u) |
		  (sourceBlock ? TegraGM20BReadbackSourceBlock : 0u);
	r.previousSequence = 0;
	r.status = 0;
	const tgm_present_read framebuffer = {
		source,
		bufferVa(0),
		sourceBlock ? pitch : static_cast<uint32_t>(copy->sourceStride),
		pitch,
		copy->width,
		copy->height,
		sourceBlockLog2,
		0,
	};
	reads[0] = framebuffer;
	// on direct scanout window A scans the swapped surface itself
	reads[1] = direct ? framebuffer
			  : tgm_present_read{target,
					     0,
					     copy->targetPitch,
					     pitch,
					     copy->width,
					     copy->height,
					     static_cast<int32_t>(copy->blockHeightLog2),
					     0};
	if (r.live && before.read.src_va && before.read.width == copy->width &&
	    before.read.height == copy->height) {
		reads[1] = before.read;
		r.flags |= TegraGM20BReadbackPrevious;
		r.previousSequence = static_cast<uint32_t>(before.sequence);
	}
	reads[1].dst_va = bufferVa(1);
	return kBuffers;
}

void tgm_readback_refused()
{
	int inFlight = TegraGM20BReadbackInFlight;
	__atomic_compare_exchange_n(&gReadback.state, &inFlight, TegraGM20BReadbackArmed, false,
				    __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

// From the GPU interrupt pass: atomics only.
void tgm_readback_done(void *display, int status)
{
	gReadback.status = status;
	__atomic_store_n(&gReadback.state,
			 status ? TegraGM20BReadbackFailed : TegraGM20BReadbackDone,
			 __ATOMIC_RELEASE);
	native_legacy_gpu_complete(display, status);
}
#endif
