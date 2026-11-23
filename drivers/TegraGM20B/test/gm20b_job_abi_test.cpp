// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstddef>
#include <string>
#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20B/TegraGM20BCopyResetABI.h"
#include "drivers/TegraGM20B/TegraGM20BJobABI.h"
#include "drivers/TegraGM20B/TegraGM20BReadbackABI.h"
#include "drivers/TegraGM20B/TegraGM20BRegDumpABI.h"

TEST(JobAbi, MethodsFollowTheHashedHeadersOnes)
{
	const uint32_t hashed[] = {TegraGM20BMethodGetInfo, TegraGM20BMethodCpuComplete,
				   TegraGM20BMethodReadback, TegraGM20BMethodRegdump,
				   TegraGM20BMethodCopyReset};
	const uint32_t job[] = {TegraGM20BMethodResourceSetCreate,
				TegraGM20BMethodResourceSetRelease, TegraGM20BMethodSubmitJob,
				TegraGM20BMethodWriterPromise, TegraGM20BMethodEventWord};
	for (uint32_t method : job) {
		EXPECT_GT(method, uint32_t(TegraGM20BMethodCpuComplete));
		for (uint32_t other : hashed)
			EXPECT_NE(method, other);
	}
}

TEST(JobAbi, RecordsKeepTheirLayout)
{
	EXPECT_EQ(offsetof(TegraGM20BAbiJob, set), 8u);
	EXPECT_EQ(offsetof(TegraGM20BAbiJob, flags), 20u);
	EXPECT_EQ(sizeof(TegraGM20BAbiJob) % 8, 0u);
	EXPECT_EQ(offsetof(TegraGM20BAbiResourceSetCreateReply, generation_offset), 12u);
	EXPECT_EQ(offsetof(TegraGM20BAbiWriterPromise, count), 12u);
	EXPECT_EQ(offsetof(TegraGM20BAbiWriterPromise, channel), 20u);
	EXPECT_EQ(sizeof(TegraGM20BAbiWriterPromise) % 8, 0u);
	EXPECT_EQ(offsetof(TegraGM20BAbiJobPromises, promise), 8u);
}

TEST(JobAbi, TheHashedAbiHeaderNeverIncludesIt)
{
	const std::string abi = switch_ios::test::SourceText("../TegraGM20BABI.h");
	ASSERT_FALSE(abi.empty());
	EXPECT_EQ(abi.find("TegraGM20BJobABI.h"), std::string::npos);
}

// What attachSet and the raw kernel model refuse a method 31 record for before its BO lookup
TEST(JobAbi, ASetRecordIsRefusedForItsReservedWordOrAccess)
{
	const auto refusal = [](uint32_t access, uint32_t reserved) {
		const TegraGM20BAbiSurfaceUse use = {7, access, reserved};
		return switch_gpu_set_record_refusal(&use);
	};
	EXPECT_EQ(refusal(TegraGM20BSurfaceRead, 0), 0u);
	EXPECT_EQ(refusal(TegraGM20BSurfaceWrite | TegraGM20BSurfaceGpuAddress, 0), 0u);
	EXPECT_EQ(refusal(TegraGM20BSurfaceRead, 1), uint32_t(TegraGM20BSetRefusedReserved));
	EXPECT_EQ(refusal(0, 0), uint32_t(TegraGM20BSetRefusedAccess));
	EXPECT_EQ(refusal(TegraGM20BSurfaceGpuAddress, 0), uint32_t(TegraGM20BSetRefusedAccess));
	EXPECT_EQ(refusal(TegraGM20BSurfaceRead | TegraGM20BSurfaceHostWritten, 0),
		  uint32_t(TegraGM20BSetRefusedAccess));
}
