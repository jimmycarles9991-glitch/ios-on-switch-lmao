// SPDX-License-Identifier: MIT
// Implicit GPU order between processes over host imports (TegraGM20BABI.h method 17) on the host: TegraGM20BExecSurfaces.cpp
// and the coherency registry's writer records (TegraGM20BCoherency.cpp) over a model of gpu_*.c's channels, whose acquire
// queue runs TegraGM20BFences.h's decision as tgm_chan_follow does under tgm_live_lock. Writers here are listed writes unless a
// test says otherwise: an Exec without a list is a writer for the CPU lock's wait only.
#include <cstdio>
#include <map>
#include <set>
#include <vector>
#include <gtest/gtest.h>
#include <IOKit/IOLib.h>
#include "drivers/TegraGM20B/TegraGM20BExecSurfaces.h"
#include "drivers/TegraGM20B/TegraGM20BFences.h"
#include "drivers/TegraGM20B/TegraGM20BGpu.h"

std::vector<std::vector<uint64_t>> &cleanCalls()
{
	static std::vector<std::vector<uint64_t>> calls;
	return calls;
}

extern "C" void proc_selfname(char *buf, int size)
{
	snprintf(buf, size_t(size), "fences_test");
}

extern "C" int proc_selfpid(void)
{
	return 1;
}

extern "C" void tgm_cpu_clean_pages(const uint64_t *pas, uint32_t npages)
{
	cleanCalls().emplace_back(pas, pas + npages);
}

// gpu_*.c's live channels by uid (one not listed is gone): completed and submitted seqnos, killed, and the first pages its
// address space imports. queued is order_add's queue: one acquire per waiter and source, the later seqno, at most
// TGM_ORDER_MAX sources per waiter.
struct FakeChannel {
	uint32_t completed = 0, submitted = 0;
	bool killed = false;
	std::set<uint64_t> imports;
};

struct FakeGpu {
	std::map<uint32_t, FakeChannel> channels;
	std::map<uint32_t, std::map<uint32_t, uint32_t>> queued;  // waiter -> source -> seqno
	int follows = 0;
	int followError = 0;
};

static FakeGpu gGpu;

static tgm_chan *chan(uint32_t uid)
{
	return reinterpret_cast<tgm_chan *>(uintptr_t(uid));
}

extern "C" uint32_t tgm_chan_uid(struct tgm_chan *c)
{
	return uint32_t(reinterpret_cast<uintptr_t>(c));
}

static int orderAdd(uint32_t waiter, uint32_t source, uint32_t seqno)
{
	auto &q = gGpu.queued[waiter];
	auto it = q.find(source);
	if (it != q.end()) {
		if (int32_t(seqno - it->second) > 0)
			it->second = seqno;
		return 0;
	}
	if (q.size() == TGM_ORDER_MAX) {
		for (auto i = q.begin(); i != q.end();) {
			if (int32_t(gGpu.channels[i->first].completed - i->second) >= 0)
				i = q.erase(i);
			else
				++i;
		}
		if (q.size() == TGM_ORDER_MAX)
			return -28;
	}
	q[source] = seqno;
	return 0;
}

static tgm_fence_source state(uint32_t waiter, uint32_t uid, const FakeChannel &c)
{
	return {uid == waiter, c.killed, c.completed};
}

extern "C" int tgm_chan_follow(struct tgm_chan *c, const uint64_t *writers, uint32_t n)
{
	gGpu.follows++;
	if (gGpu.followError)
		return gGpu.followError;
	const uint32_t waiter = tgm_chan_uid(c);
	for (uint32_t i = 0; i < n; i++) {
		const auto it = gGpu.channels.find(tgm_fence_uid(writers[i]));
		if (it == gGpu.channels.end())
			continue;
		const tgm_fence_source s = state(waiter, it->first, it->second);
		if (!tgm_fence_wanted(writers[i], &s))
			continue;
		if (const int ret = orderAdd(waiter, it->first, tgm_fence_seqno(writers[i])))
			return ret;
	}
	return 0;
}

extern "C" int tgm_chan_follow_importers(struct tgm_chan *c, uint64_t pa, uint32_t *pending)
{
	const uint32_t waiter = tgm_chan_uid(c);
	*pending = 0;
	for (const auto &[uid, ch] : gGpu.channels) {
		if (!ch.imports.count(pa))
			continue;
		const tgm_fence_source s = state(waiter, uid, ch);
		if (!tgm_fence_wanted(uint64_t(uid) << 32 | ch.submitted, &s))
			continue;
		(*pending)++;
		if (const int ret = orderAdd(waiter, uid, ch.submitted))
			return ret;
	}
	return 0;
}

extern "C" int tgm_writer_done(uint32_t uid, uint32_t seqno)
{
	const auto it = gGpu.channels.find(uid);
	return it == gGpu.channels.end() || it->second.killed ||
	       int32_t(it->second.completed - seqno) >= 0;
}

extern "C" void tgm_writer_wait(uint32_t uid, uint32_t seqno)
{
	gGpu.channels[uid].completed = seqno;
}

extern "C" int tgm_writers_all_done(void)
{
	return 1;
}

extern "C" void tgm_writers_wait_all(void)
{
}

namespace
{
constexpr uint32_t
	kApp = 1,
	kServer = 2;  // the app's channel (the writer) and the render server's (the reader)

struct Connection : tgm_fences::ExecSurfaces {
	~Connection()
	{
		clear();
	}

	void import(uint64_t handle, tgm_coh *coh, bool writable = true)
	{
		ASSERT_TRUE(reserve());
		add(handle, coh, writable);
	}

	// One Exec of channel uid in slot 0 with `pushes` pushes, submitted as `seqno`.
	int exec(uint32_t uid, uint32_t seqno, bool importsWritten = false, uint32_t pushes = 1)
	{
		const int listed = before(0, chan(uid), pushes);
		if (listed < 0)
			return listed;
		if (pushes)
			after(listed, importsWritten, uid, seqno);
		gGpu.channels[uid].submitted = seqno;
		return listed;
	}

	// An Exec that lists `handle` as written, a listed writer of it
	void write(uint32_t uid, uint32_t seqno, uint64_t handle);

	void list(std::vector<TegraGM20BAbiSurfaceUse> uses, uint32_t flags = 0)
	{
		TegraGM20BAbiExecSurfaces in = {};
		in.channel = 1;
		in.count = uint32_t(uses.size());
		in.flags = flags;
		for (size_t i = 0; i < uses.size(); i++)
			in.uses[i] = uses[i];
		ASSERT_TRUE(valid(&in));
		ASSERT_TRUE(set(0, &in));
	}
};

TegraGM20BAbiSurfaceUse reads(uint64_t handle)
{
	return {handle, TegraGM20BSurfaceRead, 0};
}

TegraGM20BAbiSurfaceUse writes(uint64_t handle)
{
	return {handle, TegraGM20BSurfaceWrite, 0};
}

void Connection::write(uint32_t uid, uint32_t seqno, uint64_t handle)
{
	list({writes(handle)});
	EXPECT_TRUE(exec(uid, seqno));
}

uint64_t writer(uint32_t uid, uint32_t seqno)
{
	return uint64_t(uid) << 32 | seqno;
}

// The listed writers of c
std::set<uint64_t> writersOf(const tgm_coh *c)
{
	uint64_t out[TGM_COH_WRITERS], pa = 0;
	uint32_t lost = 0;
	const uint32_t n = tgm_coherency_writers(c, out, &lost, &pa);
	EXPECT_EQ(lost, 0u);
	return std::set<uint64_t>(out, out + n);
}

// Whether a CPU lock of surface `id` would wait for a GPU write still running (kIOSurfaceLockAvoidSync: refused)
bool lockWaits(uint32_t id)
{
	return tegra_gm20b_iosurface_lock_will(id, 2) != 0;
}

// A surface: its entry, `pages` 4 KiB pages from pa, with id `id`. gpuWrites: an import that did not say the GPU only
// samples it.
struct Surface {
	std::vector<uint64_t> pas;
	tgm_coh *coh = nullptr;
	uint32_t id;

	Surface(uint64_t pa, uint32_t surfaceId, bool gpuWrites = true) : id(surfaceId)
	{
		for (uint32_t i = 0; i < 2; i++)
			pas.push_back(pa + i * 0x1000);
		int covered = 0;
		coh = tgm_coherency_add(pa, surfaceId, pas.data(), uint32_t(pas.size()), 1,
					gpuWrites, &covered);
	}

	~Surface()
	{
		tgm_coherency_put(coh);
	}
};

class Fences : public ::testing::Test
{
protected:
	void SetUp() override
	{
		ASSERT_EQ(tgm_coherency_init(), 0);
		gGpu = FakeGpu{};
		gGpu.channels[kApp] = {};
		gGpu.channels[kServer] = {};
		ioLogLines() = 0;
	}

	const std::map<uint32_t, uint32_t> &queued(uint32_t waiter)
	{
		return gGpu.queued[waiter];
	}
};

// A kernel copy (the copy engine's plane copies, a tiling move) follows every running writer on the GPU: its own channel's
// earlier work runs before it anyway, another's fence is an acquire, and one it cannot acquire on is refused (said), never
// waited for on the CPU.
TEST(WriterOrder, AKernelCopyOrdersOnTheGpuOrRefuses)
{
	EXPECT_EQ(tgm_writer_order(7, 7, 0), TGM_WRITER_IN_CHANNEL);
	EXPECT_EQ(tgm_writer_order(7, 7, 1), TGM_WRITER_IN_CHANNEL);
	EXPECT_EQ(tgm_writer_order(7, 9, 1), TGM_WRITER_GPU_ACQUIRE);
	EXPECT_EQ(tgm_writer_order(7, 9, 0), TGM_WRITER_REFUSED);
}

TEST_F(Fences, AReadOfASurfaceAnotherChannelIsWritingWaitsOnceForThatWrite)
{
	Surface s(0x90000000, 101);
	Connection app, server;
	app.import(5, s.coh);
	server.import(9, s.coh);
	app.write(kApp, 7, 5);
	server.list({reads(9), reads(9)});
	EXPECT_TRUE(server.exec(kServer, 3));
	EXPECT_EQ(queued(kServer), (std::map<uint32_t, uint32_t>{{kApp, 7}}));
	EXPECT_EQ(gGpu.follows, 1);
}

TEST_F(Fences, AWriteWaitsForAnotherChannelsWriteToo)
{
	Surface s(0x90100000, 102);
	Connection app, server;
	app.import(5, s.coh);
	server.import(9, s.coh);
	app.write(kApp, 7, 5);
	server.list({writes(9)});
	server.exec(kServer, 3);
	EXPECT_EQ(queued(kServer), (std::map<uint32_t, uint32_t>{{kApp, 7}}));
}

TEST_F(Fences, AWriterOnTheSameChannelIsNotWaitedFor)
{
	Surface s(0x90200000, 103);
	Connection server;
	server.import(9, s.coh);
	server.list({writes(9)});
	server.exec(kServer, 3);
	server.list({reads(9)});
	server.exec(kServer, 4);
	EXPECT_TRUE(queued(kServer).empty());
}

TEST_F(Fences, AFinishedOrGoneWriterIsNotWaitedFor)
{
	Surface s(0x90300000, 104), t(0x90400000, 105);
	Connection app, gone, server;
	app.import(5, s.coh);
	gone.import(6, t.coh);
	server.import(9, s.coh);
	server.import(10, t.coh);
	app.write(kApp, 7, 5);
	gone.write(40, 2, 6);
	gGpu.channels[kApp].completed = 7;
	gGpu.channels.erase(40);
	server.list({reads(9), reads(10)});
	server.exec(kServer, 3);
	EXPECT_TRUE(queued(kServer).empty());
}

TEST_F(Fences, AKilledChannelsWriteIsNotWaitedFor)
{
	Surface s(0x90500000, 106);
	Connection app, server;
	app.import(5, s.coh);
	server.import(9, s.coh);
	app.write(kApp, 7, 5);
	gGpu.channels[kApp].killed = true;
	server.list({reads(9)});
	server.exec(kServer, 3);
	EXPECT_TRUE(queued(kServer).empty());
}

TEST_F(Fences, OnlyTheImportsListedForWritingCountAsWritten)
{
	Surface a(0x90600000, 107), b(0x90700000, 108), sampled(0x90800000, 109, false),
		readOnly(0x90900000, 110), unlisted(0x90a00000, 111);
	Connection server;
	server.import(1, a.coh);
	server.import(2, b.coh);
	server.import(3, sampled.coh);
	server.import(4, readOnly.coh, false);
	server.import(5, unlisted.coh);
	server.list({writes(1), reads(2), writes(3), writes(4)});
	EXPECT_TRUE(server.exec(kServer, 11));
	EXPECT_EQ(writersOf(a.coh), (std::set<uint64_t>{writer(kServer, 11)}));
	EXPECT_TRUE(writersOf(b.coh).empty());
	// a write listed is one even of an import that said the GPU only samples it
	EXPECT_EQ(writersOf(sampled.coh), (std::set<uint64_t>{writer(kServer, 11)}));
	EXPECT_TRUE(writersOf(readOnly.coh).empty());
	EXPECT_TRUE(writersOf(unlisted.coh).empty());
	EXPECT_TRUE(lockWaits(a.id));
	EXPECT_FALSE(lockWaits(b.id));
	EXPECT_TRUE(lockWaits(sampled.id));
	EXPECT_FALSE(lockWaits(unlisted.id));
}

// An app's next frame without a list would be the writer of the front buffer the render server composites, too: the
// server's read waits on the GPU for listed writes only, while a CPU lock still waits for both.
TEST_F(Fences, AWriterWithoutAListIsWaitedForByCpuLocksAndNeverOnTheGpu)
{
	Surface front(0x91800000, 125), back(0x91900000, 126);
	Connection app, server;
	app.import(5, front.coh);
	app.import(6, back.coh);
	server.import(9, front.coh);
	server.import(10, back.coh);
	EXPECT_FALSE(app.exec(kApp, 7));
	EXPECT_TRUE(lockWaits(front.id));
	server.list({reads(9)});
	EXPECT_TRUE(server.exec(kServer, 3));
	EXPECT_TRUE(queued(kServer).empty());
	app.write(kApp, 8, 6);
	server.list({reads(9)});
	server.exec(kServer, 4);
	EXPECT_TRUE(queued(kServer).empty());
	server.list({reads(10)});
	server.exec(kServer, 5);
	EXPECT_EQ(queued(kServer), (std::map<uint32_t, uint32_t>{{kApp, 8}}));
}

TEST_F(Fences, WithoutAListEveryWritableImportCountsAsWrittenAndNothingIsWaitedFor)
{
	Surface a(0x90b00000, 112), sampled(0x90c00000, 113, false), readOnly(0x90d00000, 114);
	Connection app, server;
	app.import(5, a.coh);
	app.write(kApp, 7, 5);
	server.import(1, a.coh);
	server.import(2, sampled.coh);
	server.import(3, readOnly.coh, false);
	EXPECT_FALSE(server.exec(kServer, 4));
	EXPECT_TRUE(queued(kServer).empty());
	EXPECT_EQ(gGpu.follows, 0);
	EXPECT_EQ(writersOf(a.coh), (std::set<uint64_t>{writer(kApp, 7)}));
	gGpu.channels[kApp].completed = 7;
	EXPECT_TRUE(lockWaits(a.id));
	EXPECT_FALSE(lockWaits(sampled.id));
	EXPECT_FALSE(lockWaits(readOnly.id));
	// once the connection's pushes said every import is written (TegraGM20BPushImportsWritten)
	server.exec(kServer, 5, true);
	EXPECT_TRUE(lockWaits(sampled.id));
	EXPECT_FALSE(lockWaits(readOnly.id));
}

TEST_F(Fences, TheAllFlagRunsTheExecAsOneWithoutAList)
{
	Surface a(0x90e00000, 115), b(0x90f00000, 116);
	Connection app, server;
	app.import(5, a.coh);
	app.write(kApp, 7, 5);
	server.import(1, a.coh);
	server.import(2, b.coh);
	server.list({reads(1)}, TegraGM20BExecSurfacesAll);
	EXPECT_FALSE(server.exec(kServer, 4));
	EXPECT_TRUE(queued(kServer).empty());
	EXPECT_EQ(writersOf(a.coh), (std::set<uint64_t>{writer(kApp, 7)}));
	EXPECT_TRUE(writersOf(b.coh).empty());
	EXPECT_TRUE(lockWaits(b.id));
}

TEST_F(Fences, TheChannelsNextExecTakesTheListEvenWithNothingToSubmit)
{
	Surface a(0x91000000, 117), b(0x91100000, 118);
	Connection server;
	server.import(1, a.coh);
	server.import(2, b.coh);
	server.list({writes(1)});
	EXPECT_FALSE(server.exec(kServer, 3, false, 0));
	EXPECT_FALSE(server.exec(kServer, 4));
	EXPECT_TRUE(writersOf(a.coh).empty());
	EXPECT_TRUE(lockWaits(b.id));
	server.list({writes(1)});
	server.forget(0);
	EXPECT_FALSE(server.exec(kServer, 5));
}

TEST_F(Fences, AnotherChannelsListIsLeftForIt)
{
	Surface a(0x91200000, 119), b(0x91300000, 120);
	Connection server;
	server.import(1, a.coh);
	server.import(2, b.coh);
	server.list({writes(1)});
	EXPECT_FALSE(server.before(1, chan(3), 1));
	EXPECT_TRUE(server.exec(kServer, 4));
	EXPECT_EQ(writersOf(a.coh), (std::set<uint64_t>{writer(kServer, 4)}));
	EXPECT_FALSE(lockWaits(b.id));
}

TEST_F(Fences, AFreedImportIsPassedOver)
{
	Surface a(0x91400000, 121);
	Connection app, server;
	app.import(5, a.coh);
	app.write(kApp, 7, 5);
	server.import(1, a.coh);
	server.remove(1);
	server.list({writes(1)});
	EXPECT_TRUE(server.exec(kServer, 4));
	EXPECT_TRUE(queued(kServer).empty());
	EXPECT_EQ(writersOf(a.coh), (std::set<uint64_t>{writer(kApp, 7)}));
}

TEST_F(Fences, TheIndexFindsEveryLiveImportPastItsFirstSize)
{
	std::vector<Surface *> surfaces;
	Connection server;
	for (uint32_t i = 0; i < 150; i++) {
		surfaces.push_back(new Surface(0xa0000000 + uint64_t(i) * 0x100000, 1000 + i));
		server.import(10 + 2 * i, surfaces.back()->coh);
	}
	for (uint32_t i = 0; i < 150; i += 3)
		server.remove(10 + 2 * i);
	std::vector<TegraGM20BAbiSurfaceUse> uses;
	for (uint32_t i = 0; i < 60; i++)
		uses.push_back(writes(10 + 2 * (i * 2 + 1)));
	server.list(uses);
	server.exec(kServer, 9);
	for (uint32_t i = 0; i < 150; i++) {
		const bool listed = i % 2 && i / 2 < 60, live = i % 3;
		EXPECT_EQ(writersOf(surfaces[i]->coh).size(), size_t(listed && live)) << i;
		delete surfaces[i];
	}
}

TEST_F(Fences, TooManyProducerChannelsRejectTheSubmission)
{
	std::vector<Surface *> surfaces;
	Connection server;
	for (uint32_t i = 0; i < 20; i++) {
		surfaces.push_back(new Surface(0xb0000000 + uint64_t(i) * 0x100000, 2000 + i));
		Connection app;
		app.import(1, surfaces.back()->coh);
		app.write(100 + i, 5, 1);
		server.import(10 + i, surfaces.back()->coh);
	}
	std::vector<TegraGM20BAbiSurfaceUse> uses;
	for (uint32_t i = 0; i < 20; i++)
		uses.push_back(reads(10 + i));
	server.list(uses);
	EXPECT_EQ(server.exec(kServer, 3), -28);
	EXPECT_EQ(gGpu.channels[kServer].submitted, 0u);
	EXPECT_EQ(queued(kServer).size(), size_t(TGM_ORDER_MAX));
	EXPECT_EQ(ioLogLines(), 0);
	for (Surface *s : surfaces)
		delete s;
}

TEST_F(Fences, AFinishedWritersSlotGoesToTheNextChannel)
{
	Surface a(0x91500000, 122);
	for (uint32_t uid = 50; uid < 50 + TGM_COH_WRITERS; uid++) {
		Connection app;
		app.import(1, a.coh);
		app.write(uid, 2, 1);
	}
	gGpu.channels[50].completed = 2;
	Connection late;
	late.import(1, a.coh);
	late.write(60, 2, 1);
	const std::set<uint64_t> now = writersOf(a.coh);
	EXPECT_EQ(now.size(), size_t(TGM_COH_WRITERS));
	EXPECT_TRUE(now.count(writer(60, 2)));
	EXPECT_FALSE(now.count(writer(50, 2)));
}

TEST_F(Fences, LostWritersAreWaitedForThroughEveryImporterOfTheSurfaceUntilAllCompleted)
{
	Surface a(0x91600000, 123), other(0x91700000, 124);
	for (uint32_t uid = 70; uid < 71 + TGM_COH_WRITERS; uid++) {
		Connection app;
		app.import(1, a.coh);
		app.write(uid, 2, 1);
		gGpu.channels[uid].imports.insert(0x91600000);
	}
	gGpu.channels[kApp].imports.insert(0x91700000);
	gGpu.channels[kApp].submitted = 9;
	uint64_t out[TGM_COH_WRITERS], pa = 0;
	uint32_t lost = 0;
	tgm_coherency_writers(a.coh, out, &lost, &pa);
	EXPECT_NE(lost, 0u);
	EXPECT_EQ(pa, 0x91600000u);
	Connection server;
	server.import(9, a.coh);
	server.list({reads(9)});
	server.exec(kServer, 3);
	EXPECT_EQ(queued(kServer).size(), size_t(TGM_COH_WRITERS + 1));
	EXPECT_FALSE(queued(kServer).count(kApp));
	// once every importer completed, the next follow finds none pending and the loss is over
	for (uint32_t uid = 70; uid < 71 + TGM_COH_WRITERS; uid++)
		gGpu.channels[uid].completed = 2;
	server.list({reads(9)});
	server.exec(kServer, 4);
	tgm_coherency_writers(a.coh, out, &lost, &pa);
	EXPECT_EQ(lost, 0u);
}

TEST_F(Fences, ProducerOverflowDoesNotPublishAWritableConsumer)
{
	std::vector<Surface *> sources;
	Surface output(0xf0000000, 9000);
	Connection consumer;
	std::vector<TegraGM20BAbiSurfaceUse> uses;
	for (uint32_t i = 0; i < 20; i++) {
		sources.push_back(new Surface(0xc0000000 + uint64_t(i) * 0x100000, 8000 + i));
		Connection producer;
		producer.import(1, sources.back()->coh);
		producer.write(100 + i, 5, 1);
		consumer.import(10 + i, sources.back()->coh);
		uses.push_back(reads(10 + i));
	}
	consumer.import(40, output.coh);
	uses.push_back(writes(40));
	consumer.list(uses);
	EXPECT_EQ(consumer.exec(kServer, 3), -28);
	EXPECT_TRUE(writersOf(output.coh).empty());
	for (uint32_t i = 0; i < 16; i++)
		gGpu.channels[100 + i].completed = 5;
	consumer.list(uses);
	EXPECT_EQ(consumer.exec(kServer, 3), 1);
	EXPECT_EQ(writersOf(output.coh), (std::set<uint64_t>{writer(kServer, 3)}));
	for (Surface *surface : sources)
		delete surface;
}

TEST_F(Fences, MissingFenceViewRejectsWithoutPublishingConsumerWrites)
{
	Surface input(0xe0000000, 9100), output(0xe1000000, 9101);
	Connection producer, consumer;
	producer.import(1, input.coh);
	producer.write(kApp, 5, 1);
	consumer.import(1, input.coh);
	consumer.import(2, output.coh);
	consumer.list({reads(1), writes(2)});
	gGpu.followError = -19;
	EXPECT_EQ(consumer.exec(kServer, 1), -19);
	EXPECT_EQ(gGpu.channels[kServer].submitted, 0u);
	EXPECT_TRUE(writersOf(output.coh).empty());
}

TEST_F(Fences, OverflowedSurfaceWritersCannotBypassTheAcquireLimit)
{
	Surface input(0xe2000000, 9200);
	for (uint32_t uid = 100; uid < 120; uid++) {
		Connection producer;
		producer.import(1, input.coh);
		producer.write(uid, 1, 1);
		gGpu.channels[uid].imports.insert(input.pas[0]);
	}
	Connection consumer;
	consumer.import(1, input.coh);
	consumer.list({reads(1)});
	EXPECT_EQ(consumer.exec(kServer, 1), -28);
	EXPECT_EQ(gGpu.channels[kServer].submitted, 0u);
}

TEST_F(Fences, AtomicSubmitAcceptsTheLargestInlineRecordAndEmptyWork)
{
	alignas(8) unsigned char storage[TegraGM20BMaxSubmitBytes] = {};
	auto *in = reinterpret_cast<TegraGM20BAbiSubmit *>(storage);
	in->channel = 1;
	EXPECT_TRUE(tgm_fences::ExecSurfaces::validSubmit(in, sizeof(*in)));
	in->surface_count = TegraGM20BMaxExecSurfaces;
	in->push_count = (TegraGM20BMaxSubmitBytes - sizeof(*in) -
			  in->surface_count * sizeof(TegraGM20BAbiSurfaceUse)) /
			 sizeof(TegraGM20BAbiPush);
	auto *uses = reinterpret_cast<TegraGM20BAbiSurfaceUse *>(
		storage + sizeof(*in) + in->push_count * sizeof(TegraGM20BAbiPush));
	for (uint32_t i = 0; i < in->surface_count; i++)
		uses[i] = reads(i + 1);
	const uint32_t bytes = sizeof(*in) + in->push_count * sizeof(TegraGM20BAbiPush) +
			       in->surface_count * sizeof(TegraGM20BAbiSurfaceUse);
	EXPECT_EQ(in->push_count, 190u);
	EXPECT_TRUE(tgm_fences::ExecSurfaces::validSubmit(in, bytes));
	in->push_count++;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(in, bytes + sizeof(TegraGM20BAbiPush)));
}

TEST_F(Fences, AtomicSubmitRejectsMalformedPayloadBeforeReadingUses)
{
	alignas(8) unsigned char storage[TegraGM20BMaxSubmitBytes] = {};
	auto *in = reinterpret_cast<TegraGM20BAbiSubmit *>(storage);
	in->channel = 1;
	in->push_count = 2;
	in->surface_count = 1;
	auto *uses = reinterpret_cast<TegraGM20BAbiSurfaceUse *>(storage + sizeof(*in) +
								 2 * sizeof(TegraGM20BAbiPush));
	uses[0] = reads(1);
	const uint32_t bytes = sizeof(*in) + 2 * sizeof(TegraGM20BAbiPush) + sizeof(uses[0]);
	EXPECT_TRUE(tgm_fences::ExecSurfaces::validSubmit(in, bytes));
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(in, bytes - 1));
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(in, bytes + 1));
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(in, sizeof(*in) - 1));
	uses[0].access = 0;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(in, bytes));
	uses[0] = reads(1);
	uses[0].reserved = 1;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(in, bytes));
	uses[0] = reads(1);
	in->surface_flags = 2;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(in, bytes));
	in->surface_flags = 0;
	in->push_count = UINT32_MAX;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(in, bytes));
	in->push_count = 2;
	in->surface_count = UINT32_MAX;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(in, bytes));
	in->push_count = TegraGM20BMaxPushes;
	in->surface_count = 1;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(in, TegraGM20BMaxSubmitBytes));
}

TEST_F(Fences, AddressedJobCarriesEveryResourceAndAllowsOnlyTypedGpuAddresses)
{
	const uint32_t bytes = sizeof(TegraGM20BAbiSubmit) +
			       TegraGM20BMaxJobPushes * sizeof(TegraGM20BAbiPush) +
			       TegraGM20BMaxJobUses * sizeof(TegraGM20BAbiSurfaceUse);
	std::vector<uint64_t> storage((bytes + 7) / 8);
	auto *job = reinterpret_cast<TegraGM20BAbiSubmit *>(storage.data());
	*job = {1, TegraGM20BMaxJobPushes, TegraGM20BMaxJobUses, 0};
	auto *pushes = reinterpret_cast<TegraGM20BAbiPush *>(job + 1);
	auto *uses = reinterpret_cast<TegraGM20BAbiSurfaceUse *>(pushes + job->push_count);
	for (uint32_t i = 0; i < job->surface_count; ++i)
		uses[i] = {0x100000u + uint64_t(i) * 4096,
			   TegraGM20BSurfaceRead | TegraGM20BSurfaceGpuAddress, 0};
	EXPECT_TRUE(tgm_fences::ExecSurfaces::validSubmit(job, bytes, true));
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(job, bytes));
	uses[0].access = TegraGM20BSurfaceGpuAddress;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(job, bytes, true));
	uses[0].access = TegraGM20BSurfaceWrite | (1u << 8);
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(job, bytes, true));
	uses[0].access = TegraGM20BSurfaceWrite;
	++job->push_count;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(job, bytes, true));
}

TEST_F(Fences, MalformedListsAreRefused)
{
	TegraGM20BAbiExecSurfaces in = {};
	in.count = 1;
	in.uses[0] = reads(1);
	EXPECT_TRUE(tgm_fences::ExecSurfaces::valid(&in));
	in.uses[0].access = 0;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::valid(&in));
	in.uses[0].access = 4;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::valid(&in));
	in.uses[0] = reads(1);
	in.uses[0].reserved = 1;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::valid(&in));
	in.uses[0] = reads(1);
	in.flags = 2;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::valid(&in));
	in.flags = 0;
	in.count = TegraGM20BMaxExecSurfaces + 1;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::valid(&in));
}

TEST(FenceDecisions, MergeKeepsEachChannelsLaterSeqno)
{
	uint64_t acc[2];
	uint32_t n = 0;
	EXPECT_EQ(tgm_fence_merge(acc, &n, 2, 0, kServer), 0);
	EXPECT_EQ(tgm_fence_merge(acc, &n, 2, writer(kServer, 5), kServer), 0);
	EXPECT_EQ(n, 0u);
	EXPECT_EQ(tgm_fence_merge(acc, &n, 2, writer(kApp, 0xfffffffe), kServer), 0);
	EXPECT_EQ(tgm_fence_merge(acc, &n, 2, writer(kApp, 3), kServer),
		  0);  // past the wrap: later
	EXPECT_EQ(tgm_fence_merge(acc, &n, 2, writer(kApp, 1), kServer), 0);
	EXPECT_EQ(tgm_fence_merge(acc, &n, 2, writer(3, 1), kServer), 0);
	EXPECT_EQ(tgm_fence_merge(acc, &n, 2, writer(4, 1), kServer), -1);
	ASSERT_EQ(n, 2u);
	EXPECT_EQ(acc[0], writer(kApp, 3));
	EXPECT_EQ(acc[1], writer(3, 1));
}

TEST(FenceDecisions, WaitOnlyForAnUnfinishedWriteOfALiveOtherChannel)
{
	const tgm_fence_source running = {0, 0, 4}, self = {1, 0, 4}, killed = {0, 1, 4};
	EXPECT_TRUE(tgm_fence_wanted(writer(kApp, 5), &running));
	EXPECT_FALSE(tgm_fence_wanted(writer(kApp, 4), &running));
	EXPECT_FALSE(tgm_fence_wanted(writer(kApp, 5), &self));
	EXPECT_FALSE(tgm_fence_wanted(writer(kApp, 5), &killed));
	EXPECT_FALSE(tgm_fence_wanted(0, &running));
}

// A present's wait passed on a killed writer's release (gpu_fenced.h present_fenced): the frame is lost only when the submission it
// waited for was among those the kill left unfinished, across the seqno wrap too.
TEST(FenceDecisions, AWaitIsLostOnlyOnASubmissionTheKillLeftUnfinished)
{
	const TegraGM20BAbiFence live = {9, 0, 0, 0};
	const TegraGM20BAbiFence killed = {12, 7, 12, 2};
	const TegraGM20BAbiFence wrapped = {3, 0xfffffffe, 3, 1};
	EXPECT_FALSE(tgm_fence_lost(&live, 9));
	EXPECT_FALSE(tgm_fence_lost(&killed, 6));
	EXPECT_TRUE(tgm_fence_lost(&killed, 7));
	EXPECT_TRUE(tgm_fence_lost(&killed, 12));
	EXPECT_FALSE(tgm_fence_lost(&killed, 13));
	EXPECT_FALSE(tgm_fence_lost(&wrapped, 0xfffffffd));
	EXPECT_TRUE(tgm_fence_lost(&wrapped, 0xffffffff));
	EXPECT_TRUE(tgm_fence_lost(&wrapped, 2));
}
}  // namespace

TEST_F(Fences, ExplicitVisibilityRequiresAddressedSubmissionAndCoherentFlags)
{
	struct {
		TegraGM20BAbiSubmit job;
		TegraGM20BAbiPush push;
		TegraGM20BAbiSurfaceUse use;
	} input{};

	input.job = {1, 1, 1, TegraGM20BSubmitVisibilityExplicit};
	input.use = {1, TegraGM20BSurfaceRead | TegraGM20BSurfaceHostWritten, 0};
	EXPECT_TRUE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), false));
	input.job.surface_flags |= TegraGM20BSubmitExternalVisibility;
	EXPECT_TRUE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
	input.job.surface_flags = TegraGM20BSubmitExternalVisibility;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
	input.job.surface_flags = 0;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
	input.use.access &= ~uint32_t(TegraGM20BSurfaceHostWritten);
	EXPECT_TRUE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
	input.job.surface_flags =
		uint32_t(TegraGM20BSubmitVisibilityExplicit) | TegraGM20BExecSurfacesAll;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
}

TEST_F(Fences, RangeExtentRequiresAddressedGpuVaAndNonzeroLength)
{
	struct {
		TegraGM20BAbiSubmit job;
		TegraGM20BAbiPush push;
		TegraGM20BAbiSurfaceUse use;
	} input{};

	input.job = {1, 1, 1, TegraGM20BSubmitVisibilityExplicit};
	input.use = {0x10000,
		     TegraGM20BSurfaceWrite | TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceRange,
		     4096};
	EXPECT_TRUE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), false));
	input.use.reserved = 0;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
	input.use.reserved = 4096;
	input.use.access &= ~uint32_t(TegraGM20BSurfaceGpuAddress);
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
	input.use.access = TegraGM20BSurfaceWrite | TegraGM20BSurfaceGpuAddress;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
	input.use.reserved = 0;
	EXPECT_TRUE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
}

TEST_F(Fences, AddressedEventOnlyJobCarriesFullWidthWaitsAndSignals)
{
	struct {
		TegraGM20BAbiSubmit job;
		TegraGM20BAbiSubmitEvents events;
		TegraGM20BAbiEventUse uses[2];
	} input = {{1, 0, 0, TegraGM20BSubmitHasEvents},
		   {1, 1, 0},
		   {{UINT64_C(0x100000001), UINT64_MAX - 1}, {2, UINT64_MAX}}};

	EXPECT_TRUE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), false));
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input) - 1, true));
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input) + 1, true));
	input.events.reserved = 1;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
	input.events.reserved = 0;
	input.uses[0].handle = 0;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
}

TEST_F(Fences, EventCountsAreBoundedBeforeReadingAnyRecord)
{
	struct {
		TegraGM20BAbiSubmit job;
		TegraGM20BAbiSubmitEvents events;
	} input = {{1, 0, 0, TegraGM20BSubmitHasEvents}, {UINT32_MAX, 1, 0}};

	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
	input.events = {1, UINT32_MAX, 0};
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
	input.events = {16, 17, 0};
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
	input.events = {0, 0, 0};
	EXPECT_TRUE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input.job), true));
}

TEST_F(Fences, EventTrailerFollowsEveryPushAndResource)
{
	struct {
		TegraGM20BAbiSubmit job;
		TegraGM20BAbiPush push;
		TegraGM20BAbiSurfaceUse surface;
		TegraGM20BAbiSubmitEvents events;
		TegraGM20BAbiEventUse uses[TegraGM20BMaxJobEvents];
	} input = {};

	input.job = {1, 1, 1, TegraGM20BSubmitHasEvents | TegraGM20BSubmitVisibilityExplicit};
	input.push = {0x1000, 1, 0};
	input.surface = {1, TegraGM20BSurfaceRead, 0};
	input.events = {16, 16, 0};
	for (auto &use : input.uses)
		use = {1, UINT64_MAX};
	EXPECT_TRUE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
	input.job.surface_flags &= ~TegraGM20BSubmitHasEvents;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
}

TEST_F(Fences, AdmissionTrailerPrecedesEventsAndRequiresExactFraming)
{
	struct {
		TegraGM20BAbiSubmit job;
		TegraGM20BAbiSurfaceUse use;
		TegraGM20BAbiSubmitAdmission admission;
		TegraGM20BAbiSubmitEvents events;
		TegraGM20BAbiEventUse wait;
	} input = {{1, 0, 1, TegraGM20BSubmitAdmissionBegin | TegraGM20BSubmitHasEvents},
		   {1, TegraGM20BSurfaceRead, 0},
		   {UINT64_MAX, 0},
		   {1, 0, 0},
		   {1, 100}};

	EXPECT_TRUE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input) - 1, true));
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), false));
	input.admission.cookie = 0;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
	input.admission.cookie = 1;
	input.admission.reserved = 1;
	EXPECT_FALSE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
}

TEST_F(Fences, AdmissionFlagsRejectMixedAndIncompleteSequences)
{
	struct {
		TegraGM20BAbiSubmit job;
		TegraGM20BAbiSubmitAdmission admission;
	} input = {{1, 0, 0, 0}, {1, 0}};

	constexpr uint32_t accepted[] = {
		TegraGM20BSubmitAdmissionBegin, TegraGM20BSubmitAdmissionContinue,
		TegraGM20BSubmitAdmissionBegin | TegraGM20BSubmitAdmissionEnd,
		TegraGM20BSubmitAdmissionContinue | TegraGM20BSubmitAdmissionEnd,
		TegraGM20BSubmitAdmissionCancel};
	for (const auto flags : accepted) {
		input.job.surface_flags = flags;
		EXPECT_TRUE(tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
	}
	constexpr uint32_t refused[] = {
		0, TegraGM20BSubmitAdmissionEnd,
		TegraGM20BSubmitAdmissionBegin | TegraGM20BSubmitAdmissionContinue,
		TegraGM20BSubmitAdmissionCancel | TegraGM20BSubmitAdmissionBegin,
		TegraGM20BSubmitAdmissionCancel | TegraGM20BSubmitHasEvents};
	for (const auto flags : refused) {
		input.job.surface_flags = flags;
		EXPECT_FALSE(
			tgm_fences::ExecSurfaces::validSubmit(&input.job, sizeof(input), true));
	}
}
