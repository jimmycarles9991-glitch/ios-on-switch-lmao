// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstddef>
#include <string>
#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20B/TegraGM20BCopyResetABI.h"
#include "drivers/TegraGM20B/TegraGM20BGrPreemptABI.h"
#include "drivers/TegraGM20B/TegraGM20BJobABI.h"
#include "drivers/TegraGM20B/TegraGM20BReadbackABI.h"
#include "drivers/TegraGM20B/TegraGM20BRegDumpABI.h"
#include "drivers/TegraGM20B/TegraGM20BSchedABI.h"
#include "drivers/TegraGM20B/TegraGM20BVolatileABI.h"

TEST(JobAbi, MethodsFollowTheHashedHeadersOnes)
{
	const uint32_t hashed[] = {TegraGM20BMethodGetInfo,      TegraGM20BMethodCpuComplete,
				   TegraGM20BMethodPackImageMap, TegraGM20BMethodReadback,
				   TegraGM20BMethodRegdump,      TegraGM20BMethodCopyReset,
				   TegraGM20BMethodGrPreempt};
	const uint32_t job[] = {
		TegraGM20BMethodResourceSetCreate, TegraGM20BMethodResourceSetRelease,
		TegraGM20BMethodSubmitJob,         TegraGM20BMethodWriterPromise,
		TegraGM20BMethodEventWord,         TegraGM20BMethodChannelProgress};
	for (uint32_t method : job) {
		EXPECT_GT(method, uint32_t(TegraGM20BMethodCpuComplete));
		for (uint32_t other : hashed)
			EXPECT_NE(method, other);
	}
	EXPECT_NE(uint32_t(TegraGM20BMethodGrPreempt), uint32_t(TegraGM20BMethodCopyReset));
	EXPECT_NE(uint32_t(TegraGM20BMethodGrPreempt), uint32_t(TegraGM20BMethodReadback));
	EXPECT_NE(uint32_t(TegraGM20BMethodGrPreempt), uint32_t(TegraGM20BMethodRegdump));
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
	const std::string jobAbi = switch_ios::test::SourceText("../TegraGM20BJobABI.h");
	ASSERT_FALSE(abi.empty());
	ASSERT_FALSE(jobAbi.empty());
	EXPECT_EQ(abi.find("TegraGM20BJobABI.h"), std::string::npos);
	EXPECT_EQ(abi.find("TegraGM20BGrPreemptABI.h"), std::string::npos);
	EXPECT_EQ(abi.find("TegraGM20BVolatileABI.h"), std::string::npos);
	EXPECT_EQ(jobAbi.find("TegraGM20BVolatileABI.h"), std::string::npos);
	EXPECT_EQ(abi.find("TegraGM20BSchedABI.h"), std::string::npos);
	EXPECT_EQ(jobAbi.find("TegraGM20BSchedABI.h"), std::string::npos);

	constexpr uint32_t kInfoBits =
		uint32_t(TegraGM20BInfoSparse) | TegraGM20BInfoHostImport | TegraGM20BInfoZcull |
		TegraGM20BInfoKernelMap | TegraGM20BInfoSurfaceWrites |
		TegraGM20BInfoSurfaceLayout | TegraGM20BInfoPriority | TegraGM20BInfoCompression |
		TegraGM20BInfoKillRecord | TegraGM20BInfoTiledCache | TegraGM20BInfoCpuAccess |
		TegraGM20BInfoSubmitAddress | TegraGM20BInfoCpuPreserveDirty |
		TegraGM20BInfoCanonicalSurfaceLayout | TegraGM20BInfoChannelEpoch |
		TegraGM20BInfoPushFences | TegraGM20BInfoPackImage | TegraGM20BInfoBoPark;
	constexpr uint32_t kJobBits = uint32_t(TegraGM20BJobExternal) | TegraGM20BJobHostWritten |
				      TegraGM20BJobHasEvents | TegraGM20BJobQuiet |
				      TegraGM20BJobAdmission | TegraGM20BJobFulfills |
				      TegraGM20BJobEventReleases;
	EXPECT_EQ(uint32_t(TegraGM20BInfoVolatileWrites) & kInfoBits, 0u);
	EXPECT_EQ(uint32_t(TegraGM20BInfoPriorityReply) &
			  (kInfoBits | TegraGM20BInfoVolatileWrites),
		  0u);
	EXPECT_EQ(uint32_t(TegraGM20BInfoChannelProgress) &
			  (kInfoBits | TegraGM20BInfoVolatileWrites | TegraGM20BInfoPriorityReply),
		  0u);
	EXPECT_EQ(uint32_t(TegraGM20BJobVolatileWrites) & kJobBits, 0u);
	EXPECT_EQ(sizeof(TegraGM20BAbiPriorityReply), 8u);
	EXPECT_EQ(sizeof(TegraGM20BAbiChannelProgressReply), 8u);
	constexpr uint32_t kUsedSlotBytes = sizeof(TegraGM20BAbiFence) +
					    sizeof(TegraGM20BAbiFenceSemantic) +
					    sizeof(TegraGM20BAbiFenceIdentity);
	EXPECT_GE(uint32_t(TegraGM20BFencePageChannelProgressOffset), kUsedSlotBytes);
	EXPECT_LE(uint32_t(TegraGM20BFencePageChannelProgressOffset) + 16u,
		  uint32_t(TegraGM20BFencePageRecordBytes));
	EXPECT_EQ(uint32_t(TegraGM20BFencePageChannelProgressOffset) % 16u, 0u);
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
