// SPDX-License-Identifier: MIT
// TegraGM20BZcullContext.h against nvgpu's zcull context switching (L4T r32.7.6 gk20a/gr_gk20a.c
// gr_gk20a_ctx_zcull_setup, gr_gk20a_write_zcull_ptr, gr_gk20a_init_golden_ctx_image;
// hw_ctxsw_prog_gm20b.h), over a model of nvkm's memory and VMM: each GR context's zcull image, the
// words that point FECS at it, and that the image is that context's alone until its teardown.
#include "gm20b_zcull_context_fixture.h"
extern "C" {
#include "drivers/TegraGM20B/TegraGM20BZcullContext.h"
}
#include <gtest/gtest.h>
#include <map>
#include <set>

struct nvkm_vmm {
	u64 next;
};

namespace
{
constexpr int kNoMemory = -12, kNoSpace = -28;
constexpr u32 kFecsZcullBytes = 0x1500;
constexpr u32 kZcullWord = 0x1c, kZcullPtrWord = 0x20, kNoCtxsw = 1, kSeparateBuffer = 2;

enum class Fail {
	None,
	Memory,
	Va,
	Map
};
Fail failing;

struct Memory : nvkm_memory {
	u64 bytes = 0;
	u32 align = 0;
	bool zero = false;
	nvkm_memory_target target = NVKM_MEM_TARGET_VRAM;
	nvkm_vmm *vmm = nullptr;
	nvkm_vma *at = nullptr;
	const void *args = nullptr;
	u32 argc = 0;
};

std::set<Memory *> memories;
std::set<nvkm_vma *> vmas;

u64 memorySize(nvkm_memory *memory)
{
	return static_cast<Memory *>(memory)->bytes;
}

int memoryMap(nvkm_memory *m, u64 offset, nvkm_vmm *vmm, nvkm_vma *vma, void *argv, u32 argc)
{
	if (failing == Fail::Map)
		return kNoSpace;
	auto *memory = static_cast<Memory *>(m);
	EXPECT_EQ(offset, 0u);
	memory->vmm = vmm;
	memory->at = vma;
	memory->args = argv;
	memory->argc = argc;
	return 0;
}

const nvkm_memory_func memoryFunc = {memorySize, memoryMap};

struct Context : nvkm_gpuobj {
	std::map<u32, u32> words;
};

void contextWrite(nvkm_gpuobj *context, u32 offset, u32 data)
{
	static_cast<Context *>(context)->words[offset] = data;
}

const nvkm_gpuobj_func contextFunc = {contextWrite};

// A context image as nvkm copies it from the golden one, whose zcull words are no ctxsw and 0
Context goldenCopy()
{
	Context context;
	context.ptrs = &contextFunc;
	context.words = {{kZcullWord, kNoCtxsw}, {kZcullPtrWord, 0}};
	return context;
}

struct Channel {
	nvkm_memory *image = nullptr;
	nvkm_vma *vma = nullptr;
};

bool disjoint(const nvkm_vma *a, const nvkm_vma *b)
{
	return a->addr + a->size <= b->addr || b->addr + b->size <= a->addr;
}

class ZcullContext : public testing::Test
{
protected:
	// above 4 GiB, so the pointer word is the VA >> 8 and not the VA's low word
	nvkm_vmm vm{0x1000400000ULL};
	// nvkm's gf100_vmm_map_v0 { .priv = 1 }
	u8 args[8] = {1};

	void SetUp() override
	{
		failing = Fail::None;
	}

	void TearDown() override
	{
		EXPECT_TRUE(memories.empty());
		EXPECT_TRUE(vmas.empty());
		for (const Memory *memory : memories)
			delete memory;
		for (const nvkm_vma *vma : vmas)
			delete vma;
		memories.clear();
		vmas.clear();
	}

	int create(Channel &channel, u32 bytes = kFecsZcullBytes)
	{
		return tgm_zcull_image_new(nullptr, &vm, bytes, args, sizeof args, &channel.image,
					   &channel.vma);
	}

	void destroy(Channel &channel)
	{
		tgm_zcull_image_del(&vm, &channel.image, &channel.vma);
	}
};
}  // namespace

extern "C" {
int nvkm_memory_new(nvkm_device *, nvkm_memory_target target, u64 size, u32 align, bool zero,
		    nvkm_memory **out)
{
	if (failing == Fail::Memory)
		return kNoMemory;
	auto *memory = new Memory();
	memory->func = &memoryFunc;
	memory->bytes = size;
	memory->align = align;
	memory->zero = zero;
	memory->target = target;
	memories.insert(memory);
	*out = memory;
	return 0;
}

void nvkm_memory_unref(nvkm_memory **memory)
{
	if (!*memory)
		return;
	memories.erase(static_cast<Memory *>(*memory));
	delete static_cast<Memory *>(*memory);
	*memory = nullptr;
}

int nvkm_vmm_get(nvkm_vmm *vmm, u8 page, u64 size, nvkm_vma **out)
{
	if (failing == Fail::Va)
		return kNoSpace;
	auto *vma = new nvkm_vma{vmm->next, size, page};
	vmm->next += (size + 0xfff) & ~0xfffULL;
	vmas.insert(vma);
	*out = vma;
	return 0;
}

void nvkm_vmm_put(nvkm_vmm *, nvkm_vma **vma)
{
	if (!*vma)
		return;
	vmas.erase(*vma);
	delete *vma;
	*vma = nullptr;
}
}

TEST_F(ZcullContext, AContextSavesItsZcullInAnImageOfItsOwn)
{
	Channel channel;
	ASSERT_EQ(create(channel), 0);
	const auto *image = static_cast<const Memory *>(channel.image);
	ASSERT_NE(image, nullptr);
	ASSERT_NE(channel.vma, nullptr);
	EXPECT_EQ(image->bytes, kFecsZcullBytes);
	EXPECT_EQ(image->target, NVKM_MEM_TARGET_INST);
	EXPECT_TRUE(image->zero);
	EXPECT_EQ(image->align, static_cast<u32>(TGM_ZCULL_CTXSW_ALIGN))
		<< "the alignment method 'ZC' reports";
	EXPECT_EQ(channel.vma->page, 12);
	EXPECT_EQ(channel.vma->size, kFecsZcullBytes);
	EXPECT_EQ(image->vmm, &vm) << "mapped in the channel's own VM";
	EXPECT_EQ(image->at, channel.vma);
	EXPECT_EQ(image->args, args);
	EXPECT_EQ(image->argc, sizeof args);

	Context context = goldenCopy();
	tgm_zcull_image_bind(&context, channel.vma);
	EXPECT_EQ(context.words[kZcullWord], kSeparateBuffer);
	EXPECT_EQ(context.words[kZcullPtrWord], static_cast<u32>(channel.vma->addr >> 8));
	EXPECT_EQ(context.words.size(), 2u);
	destroy(channel);
}

TEST_F(ZcullContext, WithoutAnImageTheContextKeepsNoCtxsw)
{
	Channel channel;
	ASSERT_EQ(create(channel, 0), 0);
	EXPECT_EQ(channel.image, nullptr);
	EXPECT_EQ(channel.vma, nullptr);
	EXPECT_TRUE(memories.empty());

	Context context = goldenCopy();
	context.words[kZcullWord] = kSeparateBuffer;
	context.words[kZcullPtrWord] = 0xdead;
	tgm_zcull_image_bind(&context, channel.vma);
	EXPECT_EQ(context.words[kZcullWord], kNoCtxsw);
	EXPECT_EQ(context.words[kZcullPtrWord], 0u);
	destroy(channel);
}

TEST_F(ZcullContext, TwoContextsNeverShareAnImage)
{
	Channel first;
	Channel second;
	ASSERT_EQ(create(first), 0);
	ASSERT_EQ(create(second), 0);
	EXPECT_NE(first.image, second.image);
	EXPECT_TRUE(disjoint(first.vma, second.vma));

	Context firstContext = goldenCopy();
	Context secondContext = goldenCopy();
	tgm_zcull_image_bind(&firstContext, first.vma);
	tgm_zcull_image_bind(&secondContext, second.vma);
	EXPECT_NE(firstContext.words[kZcullPtrWord], secondContext.words[kZcullPtrWord]);

	destroy(second);
	EXPECT_EQ(memories.count(static_cast<Memory *>(first.image)), 1u);
	EXPECT_EQ(vmas.count(first.vma), 1u);
	EXPECT_EQ(static_cast<Memory *>(first.image)->at, first.vma);
	EXPECT_EQ(firstContext.words[kZcullPtrWord], static_cast<u32>(first.vma->addr >> 8));
	destroy(first);
}

TEST_F(ZcullContext, TeardownReleasesWhatAFailedCreationLeft)
{
	for (const Fail at : {Fail::Memory, Fail::Va, Fail::Map}) {
		failing = at;
		Channel channel;
		EXPECT_EQ(create(channel), at == Fail::Memory ? kNoMemory : kNoSpace);
		failing = Fail::None;
		destroy(channel);
		EXPECT_EQ(channel.image, nullptr);
		EXPECT_EQ(channel.vma, nullptr);
		EXPECT_TRUE(memories.empty());
		EXPECT_TRUE(vmas.empty());
	}
}
