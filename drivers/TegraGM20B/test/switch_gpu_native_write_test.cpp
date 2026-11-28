// SPDX-License-Identifier: MIT
#include <vector>
#include "bazel/testing/source_text.h"
#include "switch_gpu_backing_fixture.h"

using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;

namespace
{
class NativeWrite : public Backing
{
protected:
	switch_gpu_backing *logical = nullptr, *native = nullptr;
	volatile uint64_t payload = 1, representation = 5;

	void SetUp() override
	{
		Backing::SetUp();
		logical = backing();
		native = backing(4);
	}

	void TearDown() override
	{
		switch_gpu_backing_release(logical);
		switch_gpu_backing_release(native);
		Backing::TearDown();
	}

	switch_gpu_native_surface_view view()
	{
		switch_gpu_native_surface_view v = {};
		v.logical = logical;
		v.native = native;
		v.payload = {logical, &payload, 1};
		v.representation = {logical, &representation, 5};
		return v;
	}

	struct Job {
		switch_gpu_backing_use uses[8] = {};
		switch_gpu_backing_guard guards[8] = {};
		std::vector<switch_gpu_backing_write_range> writes;
		uint32_t useCount = 0, guardCount = 0;
	};

	// One BO's use as runJobUses resolves a native view: its uses and guards, and for a write the native backing's
	// range, whole and unknown.
	void use(Job &job, uint32_t access)
	{
		ASSERT_EQ(switch_gpu_native_surface_add_use(view(), false, access, job.uses,
							    job.guards, 8, job.useCount,
							    job.guardCount),
			  0);
		if (access & SWITCH_GPU_BACKING_WRITE)
			job.writes.push_back({native, 0, 0});
	}

	int submit(Job &job, const switch_gpu_backing_token &writer)
	{
		switch_gpu_backing_transaction transaction = {};
		int result = switch_gpu_backing_begin_checked(
			job.uses, job.useCount, job.guards, job.guardCount, &writer, &transaction);
		if (result)
			return result;
		result = switch_gpu_backing_write_ranges(&transaction, job.writes.data(),
							 uint32_t(job.writes.size()));
		if (result)
			switch_gpu_backing_abort(&transaction);
		else
			switch_gpu_backing_commit(&transaction);
		return result;
	}

	uint64_t readerDependsOn()
	{
		const auto reader = token(209, 1, SWITCH_GPU_ENGINE_VIC);
		switch_gpu_backing_use readUse = {native, SWITCH_GPU_BACKING_READ, 0};
		switch_gpu_backing_transaction transaction = {};
		EXPECT_EQ(switch_gpu_backing_begin(&readUse, 1, &reader, &transaction), 0);
		switch_gpu_backing_token dependency = {};
		uint32_t needed = 0;
		EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, &dependency, 1, &needed),
			  0);
		switch_gpu_backing_abort(&transaction);
		return needed ? dependency.value : 0;
	}
};

TEST_F(NativeWrite, BothPlanesOfOneNativeSurfaceRenderedInOneJobSubmit)
{
	Job job;
	use(job, SWITCH_GPU_BACKING_WRITE);
	use(job, SWITCH_GPU_BACKING_WRITE);
	const auto writer = token(208, 7);
	ASSERT_EQ(submit(job, writer), 0);
	EXPECT_EQ(readerDependsOn(), writer.value);
	complete(writer);
}

TEST_F(NativeWrite, OnePlaneWrittenAndOneSampledSubmit)
{
	Job job;
	use(job, SWITCH_GPU_BACKING_WRITE);
	use(job, SWITCH_GPU_BACKING_READ);
	const auto writer = token(208, 8);
	ASSERT_EQ(submit(job, writer), 0);
	EXPECT_EQ(readerDependsOn(), writer.value);
	complete(writer);
}

TEST_F(NativeWrite, AWriteUseWithNoRangeNamingItsBackingIsRefused)
{
	Job job;
	use(job, SWITCH_GPU_BACKING_WRITE);
	job.writes.clear();
	EXPECT_EQ(submit(job, token(208, 9)), -22);
	EXPECT_EQ(readerDependsOn(), 0u);
	EXPECT_EQ(publish(native, SWITCH_GPU_BACKING_WRITE, token(210, 1, SWITCH_GPU_ENGINE_VIC)),
		  0)
		<< "the refused job held nothing past its abort";
}

TEST(NativeWriteSource, RunJobUsesGivesANativeWriteItsBackingsRange)
{
	const CodeText native = Block(SourceText("../TegraGM20BUserClientJobSubmit.cpp"),
				      "IOReturn AGXDeviceUserClient::runJobUses(")
					.between("if (bo->nativeView) {", "continue;");
	const CodeText write = native.after("if (access & TegraGM20BSurfaceWrite) {");
	EXPECT_TRUE(write.startsWith("switch_gpu_backing *native = bo->nativeView->native; "
				     "job.writes[writeCount++] = {native, 0, 0}; "
				     "scanoutWrites |= switch_gpu_backing_scanned(native) != 0;"));
}
}  // namespace
