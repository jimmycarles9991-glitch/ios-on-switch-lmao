// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "switch_nvjpg_engine_model.h"
#include <new>
#define IONewZero(type, count)             new (std::nothrow) type[count]()
#define IONewData(type, count)             new (std::nothrow) type[count]
#define IODelete(pointer, type, count)     delete[] (pointer)
#define IODeleteData(pointer, type, count) delete[] (pointer)
#include "../TegraNVJPGMapping.cpp"

void switch_nvjpg_count_mapping(int)
{}

struct switch_gpu_backing {
    switch_gpu_backing_info info;
    void *context = nullptr;
    void (*destroy)(void *) = nullptr;
};

extern "C" void *switch_gpu_backing_mapping_context(const switch_gpu_backing *backing, uint32_t engine)
{
    EXPECT_EQ(engine, uint32_t(SWITCH_GPU_ENGINE_NVJPG));
    return backing ? backing->context : nullptr;
}

extern "C" void switch_gpu_backing_describe(switch_gpu_backing *backing, switch_gpu_backing_info *info)
{
    *info = backing->info;
}

extern "C" int switch_gpu_backing_mapping_install(switch_gpu_backing *backing, uint32_t engine, uint64_t generation,
                                                  void *context, void (*destroyContext)(void *))
{
    EXPECT_EQ(engine, uint32_t(SWITCH_GPU_ENGINE_NVJPG));
    EXPECT_EQ(generation, backing->info.allocation_generation);
    EXPECT_EQ(backing->context, nullptr);
    backing->context = context;
    backing->destroy = destroyContext;
    return 0;
}

TEST_F(NVJPGGate, RepeatedDecodesUseTheBackingsMappingWithoutLocksOrTableWrites)
{
    const std::vector<uint64_t> pages{0x91000000, 0x92000000};
    switch_gpu_backing backing{};
    backing.info.allocation_id = 1;
    backing.info.allocation_generation = 2;
    backing.info.pages = pages.data();
    backing.info.page_count = static_cast<uint32_t>(pages.size());
    switch_nvjpg_mapping_start(0x93000000);
    gSmmuRangeFlushes.clear();
    SwitchNVJPGMapping *mapping = switch_nvjpg_mapping(&backing, 2);
    ASSERT_NE(mapping, nullptr);
    EXPECT_EQ(mapping->pages, pages.data());
    EXPECT_EQ(mapping->count, 2u);
    EXPECT_EQ(mapping->mapped, 258u);
    EXPECT_EQ(gDomain.translate(mapping->address), pages[0]);
    EXPECT_EQ(gDomain.translate(mapping->address + 4096), pages[1]);
    EXPECT_EQ(gDomain.translate(mapping->address + 8192), 0x93000000u);
    EXPECT_EQ(gDomain.translate(mapping->address + uint64_t(mapping->mapped) * 4096), UINT64_MAX);
    ASSERT_EQ(gSmmuRangeFlushes.size(), 1u);
    switch_test_lock_taken = +[](IOLock *) {
        ADD_FAILURE() << "retained mapping took a lock";
    };
    for (unsigned i = 0; i < 20; ++i)
        EXPECT_EQ(switch_nvjpg_mapping(&backing, 2), mapping);
    switch_test_lock_taken = nullptr;
    EXPECT_EQ(gSmmuRangeFlushes.size(), 1u);
    const uint64_t address = mapping->address;
    backing.destroy(backing.context);
    EXPECT_EQ(gDomain.translate(address), UINT64_MAX);
    ASSERT_EQ(gSmmuRangeFlushes.size(), 2u);
}

TEST_F(NVJPGGate, ShorterAliasesPutMcuPaddingInTheSinkInsteadOfAdjacentBackingPages)
{
    const std::vector<uint64_t> pages{0x94000000, 0x95000000};
    switch_gpu_backing backing{};
    backing.info.allocation_id = 3;
    backing.info.allocation_generation = 4;
    backing.info.pages = pages.data();
    backing.info.page_count = static_cast<uint32_t>(pages.size());
    switch_nvjpg_mapping_start(0x96000000);
    SwitchNVJPGMapping *shorter = switch_nvjpg_mapping(&backing, 1);
    SwitchNVJPGMapping *whole = switch_nvjpg_mapping(&backing, 2);
    ASSERT_NE(shorter, nullptr);
    ASSERT_NE(whole, nullptr);
    EXPECT_NE(shorter->address, whole->address);
    EXPECT_EQ(gDomain.translate(shorter->address + 4096), 0x96000000u);
    EXPECT_EQ(gDomain.translate(whole->address + 4096), pages[1]);
    const size_t flushes = gSmmuRangeFlushes.size();
    switch_test_lock_taken = +[](IOLock *) {
        ADD_FAILURE() << "retained alias mapping took a lock";
    };
    EXPECT_EQ(switch_nvjpg_mapping(&backing, 1), shorter);
    EXPECT_EQ(switch_nvjpg_mapping(&backing, 2), whole);
    switch_test_lock_taken = nullptr;
    EXPECT_EQ(gSmmuRangeFlushes.size(), flushes);
    const uint64_t first = shorter->address, second = whole->address;
    backing.destroy(backing.context);
    EXPECT_EQ(gDomain.translate(first), UINT64_MAX);
    EXPECT_EQ(gDomain.translate(second), UINT64_MAX);
}
