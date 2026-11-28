// SPDX-License-Identifier: MIT
#include "../control_memory.h"
#include "../host_access.h"
#include "../kernel.h"
#include "bazel/testing/source_text.h"
#include <IOKit/IOKitLib.h>
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <regex>
#include <string>
#include <vector>

namespace {
struct Model {
    uint64_t nextHandle = 1;
    std::map<uint32_t, std::vector<uint8_t>> pages;
    std::vector<uint32_t> allocFlags;
    std::vector<TegraGM20BAbiBindOp> binds;
    std::vector<TegraGM20BAbiCpuAccess> acquires;
    std::vector<uint32_t> importFlags;
    std::vector<TegraGM20BAbiBoPark> parks;
    std::vector<TegraGM20BAbiBoUnpark> unparks;
    std::vector<TegraGM20BAbiMap> fixedMaps;
    std::vector<uint64_t> frees;
    std::vector<TegraGM20BAbiUnmap> unmaps;
    bool advertiseBoPark = true;
    uint32_t nextUnparkStatus = TegraGM20BUnparkResident;
    uint64_t nextUnparkAddress = 0x5000000;
    uint64_t nextUnparkBytes = 0x10000;
    kern_return_t surfaceWrites = KERN_SUCCESS;
} model;

constexpr uint32_t kInfoFlags = TegraGM20BInfoKernelMap | TegraGM20BInfoSubmitAddress | TegraGM20BInfoCpuAccess;
}  // namespace

extern "C" kern_return_t IOConnectCallStructMethod(mach_port_t, uint32_t method, const void *input, size_t,
                                                   void *output, size_t *)
{
    switch (method) {
    case TegraGM20BMethodGetInfo: {
        auto &info = *static_cast<TegraGM20BAbiInfo *>(output);
        info = {};
        info.abi_version = TegraGM20BAbiVersion;
        info.flags = kInfoFlags | (model.advertiseBoPark ? uint32_t(TegraGM20BInfoBoPark) : 0u);
        info.va_start = 0x1000000;
        info.va_end = 0x100000000;
        info.page_size = 4096;
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoAlloc: {
        const auto &in = *static_cast<const TegraGM20BAbiBoAlloc *>(input);
        model.allocFlags.push_back(in.flags);
        const uint32_t token = uint32_t(model.nextHandle);
        model.pages[token].assign(in.bytes, 0);
        *static_cast<TegraGM20BAbiBoAllocReply *>(output) = {model.nextHandle++, token, 0};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoImport:
        model.importFlags.push_back(static_cast<const TegraGM20BAbiBoImport *>(input)->flags);
        *static_cast<TegraGM20BAbiBoAllocReply *>(output) = {model.nextHandle++, 0, 0};
        return KERN_SUCCESS;
    case TegraGM20BMethodSurfaceWrites:
        return model.surfaceWrites;
    case TegraGM20BMethodMap: {
        const auto &in = *static_cast<const TegraGM20BAbiMap *>(input);
        auto &bytes = model.pages[in.token];
        if (in.flags & TegraGM20BMapFixed) {
            model.fixedMaps.push_back(in);
            *static_cast<TegraGM20BAbiMapReply *>(output) = {in.address, bytes.size()};
            return KERN_SUCCESS;
        }
        *static_cast<TegraGM20BAbiMapReply *>(output) = {uint64_t(uintptr_t(bytes.data())), bytes.size()};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodVmBind:
        model.binds.push_back(static_cast<const TegraGM20BAbiVmBind *>(input)->ops[0]);
        return KERN_SUCCESS;
    case TegraGM20BMethodCpuAcquire:
        model.acquires.push_back(*static_cast<const TegraGM20BAbiCpuAccess *>(input));
        return KERN_SUCCESS;
    case TegraGM20BMethodBoPark:
        model.parks.push_back(*static_cast<const TegraGM20BAbiBoPark *>(input));
        return KERN_SUCCESS;
    case TegraGM20BMethodBoUnpark: {
        model.unparks.push_back(*static_cast<const TegraGM20BAbiBoUnpark *>(input));
        *static_cast<TegraGM20BAbiBoUnparkReply *>(output) = {model.nextUnparkAddress, model.nextUnparkBytes,
                                                              model.nextUnparkStatus, 0};
        return KERN_SUCCESS;
    }
    case TegraGM20BMethodBoFree:
        model.frees.push_back(static_cast<const TegraGM20BAbiBoFree *>(input)->handle);
        return KERN_SUCCESS;
    case TegraGM20BMethodUnmap:
        model.unmaps.push_back(*static_cast<const TegraGM20BAbiUnmap *>(input));
        return KERN_SUCCESS;
    case TegraGM20BMethodCpuRelease:
        return KERN_SUCCESS;
    default:
        std::abort();
    }
}

namespace gm20b {
void reportCpuReleaseFailure()
{}
}  // namespace gm20b

extern "C" io_service_t IOServiceGetMatchingService(mach_port_t, CFDictionaryRef matching)
{
    CFRelease(matching);
    return 111;
}

extern "C" kern_return_t IOServiceOpen(io_service_t, task_port_t, uint32_t, io_connect_t *connection)
{
    *connection = 42;
    return KERN_SUCCESS;
}

extern "C" kern_return_t IOObjectRelease(io_object_t)
{
    return KERN_SUCCESS;
}

extern "C" kern_return_t IOServiceClose(io_connect_t)
{
    return KERN_SUCCESS;
}

class GM20BKernelMemory : public testing::Test {
protected:
    gm20b::Kernel kernel;

    void SetUp() override
    {
        model = {};
        std::string err;
        ASSERT_TRUE(kernel.open(err)) << err;
        ASSERT_TRUE(kernel.exactJobs());
    }
};

TEST(GM20BControlMemory, MemoryTheGpuAloneTouchesPerPixelOrDrawIsCached)
{
    using gm20b::ControlUse;
    for (ControlUse use :
         {ControlUse::InterlockTickets, ControlUse::RootTable, ControlUse::ZeroPage, ControlUse::ColorTable,
          ControlUse::ArgumentRoots, ControlUse::HandleTables, ControlUse::ZcullStorage})
        EXPECT_EQ(gm20b::controlMapping(use), gm20b::ControlMapping::Cached) << int(use);
    for (ControlUse use : {ControlUse::TextureHeaders, ControlUse::SamplerHeaders, ControlUse::Stamps, ControlUse::Ring,
                           ControlUse::Bytes, ControlUse::TransferSlot, ControlUse::StateSegments})
        EXPECT_EQ(gm20b::controlMapping(use), gm20b::ControlMapping::Volatile) << int(use);
}

// The interlock's ticket words are the stage compile's (SR_AFFINITY & 0xff) * 256 + (ticket & 0xff): the buffer holds
// every word that index reaches, as no fault would show one past it
TEST(GM20BControlMemory, TheTicketBufferHoldsEveryWordTheInterlockIndexes)
{
    const std::string lowering =
        switch_ios::test::SourceText("../../../third_party/mesa/src/nouveau/compiler/nak_nir_lower_fs_interlock.c");
    std::smatch affinity, ticket, stride;
    ASSERT_TRUE(std::regex_search(lowering, affinity, std::regex(R"(gpc_id, 0, (\d+)\))")));
    ASSERT_TRUE(std::regex_search(lowering, ticket, std::regex(R"(ordering_ticket, 0, (\d+)\))")));
    ASSERT_TRUE(std::regex_search(lowering, stride, std::regex(R"(nir_imul_imm\(b, gpc_id, (0x[0-9a-f]+)\))")));
    const uint64_t perGpc = std::stoull(stride[1].str(), nullptr, 16);
    ASSERT_GE(perGpc, uint64_t(1) << std::stoi(ticket[1].str()));
    const uint64_t words = (uint64_t(1) << std::stoi(affinity[1].str())) * perGpc;
    const std::string device = switch_ios::test::SourceText("../gpu.cpp");
    std::smatch bytes;
    ASSERT_TRUE(std::regex_search(device, bytes, std::regex(R"(kTicketBytes = (\d+) \* (\d+) \* (\d+);)")));
    EXPECT_GE(std::stoull(bytes[1].str()) * std::stoull(bytes[2].str()) * std::stoull(bytes[3].str()), words * 4);
    EXPECT_TRUE(std::regex_search(device, std::regex(R"(allocControl\(kTicketBytes, tickets_,)")));
}

// Device::init's allocation of each of its fixed control BOs names the use that maps it
TEST(GM20BControlMemory, DeviceAllocatesEachFixedBufferForItsUse)
{
    const std::string source = switch_ios::test::SourceText("../gpu.cpp");
    const std::pair<const char *, const char *> wanted[] = {
        {"tickets_", "InterlockTickets"},
        {"root_", "RootTable"},
        {"zero_", "ZeroPage"},
        {"colorTable_", "ColorTable"},
        {"tic_", "TextureHeaders"},
        {"tsc_", "SamplerHeaders"},
        {"stamps_", "Stamps"},
    };
    for (const auto &[member, use] : wanted) {
        const std::regex call(std::string("allocControl\\([^;()]*(\\([^()]*\\))?[^;()]*,\\s*") + member +
                              ",\\s*ControlUse::(\\w+)\\)");
        std::smatch match;
        ASSERT_TRUE(std::regex_search(source, match, call)) << member << " is not allocated for a named use";
        EXPECT_EQ(match[2].str(), use) << member;
    }
}

TEST_F(GM20BKernelMemory, CachedControlMemoryBindsThroughTheL2FromTheControlReserve)
{
    gm20b::Bo tickets, ring;
    ASSERT_TRUE(kernel.allocControl(0x1000, tickets, gm20b::ControlUse::InterlockTickets));
    ASSERT_TRUE(kernel.allocControl(0x10000, ring, gm20b::ControlUse::Ring));
    ASSERT_EQ(model.binds.size(), 2u);
    EXPECT_EQ(model.binds[0].op, uint32_t(TegraGM20BBindMap));
    EXPECT_EQ(model.binds[1].op, uint32_t(TegraGM20BBindMapVolatile));
    EXPECT_FALSE(tickets.gpuVolatile);
    EXPECT_TRUE(ring.gpuVolatile);
    ASSERT_EQ(model.allocFlags.size(), 2u);
    for (uint32_t flags : model.allocFlags)
        EXPECT_EQ(flags, uint32_t(TegraGM20BBoCoherent | TegraGM20BBoControl));
    kernel.free(tickets);
    kernel.free(ring);
}

// An MTLHeap's arena: host pages imported and bound at a VA reserved for them (arena.cpp bindArena)
TEST_F(GM20BKernelMemory, AnImportBoundInAReservationLeasesItsBytesToTheCpuAndLeavesItsVa)
{
    constexpr uint64_t kBytes = 0x20000;
    auto *cpu = static_cast<uint8_t *>(aligned_alloc(0x4000, kBytes));
    ASSERT_NE(cpu, nullptr);
    const uint64_t va = kernel.reserveVa(kBytes);
    ASSERT_NE(va, 0u);
    gm20b::Bo bo;
    ASSERT_TRUE(kernel.importPages(cpu, kBytes, bo));
    ASSERT_TRUE(kernel.bindImportAt(cpu, bo, va, 0));
    {
        gm20b::HostAccess access;
        ASSERT_TRUE(access.acquire(kernel, bo, cpu + 0x1100, 64, true));
        ASSERT_EQ(model.acquires.size(), 1u);
        EXPECT_EQ(model.acquires[0].handle, bo.handle);
        EXPECT_EQ(model.acquires[0].offset, 0x1100u);
        EXPECT_EQ(model.acquires[0].bytes, 64u);
        EXPECT_TRUE(access.finish());
    }
    uint64_t offset = 0;
    EXPECT_TRUE(gm20b::boCpuOffset(bo, cpu + kBytes - 16, 16, offset));
    EXPECT_EQ(offset, kBytes - 16);
    EXPECT_FALSE(gm20b::boCpuOffset(bo, cpu + kBytes - 16, 17, offset));
    const uint64_t used = kernel.vaInUse();
    kernel.free(bo);
    EXPECT_EQ(kernel.vaInUse(), used);
    kernel.releaseVa(va, kBytes);
    EXPECT_EQ(kernel.vaInUse(), used - kBytes);
    free(cpu);
}

// A surface import says the GPU only reads it until a SurfaceWrites the kernel refused: from then on every import of the
// connection is one the GPU may write, so no GPU write of a surface goes unmarked
TEST_F(GM20BKernelMemory, ASurfaceWritesTheKernelRefusedMarksEveryLaterImportWritten)
{
    alignas(4096) static uint8_t pages[3][4096];
    gm20b::Bo bo;
    ASSERT_TRUE(kernel.importPages(pages[0], 4096, bo, 7));
    ASSERT_TRUE(kernel.surfaceGpuWrites(7));
    ASSERT_TRUE(kernel.importPages(pages[1], 4096, bo, 8));
    model.surfaceWrites = kIOReturnUnsupported;
    EXPECT_FALSE(kernel.surfaceGpuWrites(8));
    ASSERT_TRUE(kernel.importPages(pages[2], 4096, bo, 9));
    ASSERT_EQ(model.importFlags.size(), 3u);
    EXPECT_EQ(model.importFlags[0] & TegraGM20BImportGpuReads, uint32_t(TegraGM20BImportGpuReads));
    EXPECT_EQ(model.importFlags[1] & TegraGM20BImportGpuReads, uint32_t(TegraGM20BImportGpuReads));
    EXPECT_EQ(model.importFlags[2] & TegraGM20BImportGpuReads, 0u);
}

TEST_F(GM20BKernelMemory, BoParkUnparkMapAtAndFreeParkedSkipUnmap)
{
    EXPECT_TRUE(kernel.supportsBoPark());
    gm20b::Bo bo;
    ASSERT_TRUE(kernel.alloc(0x10000, false, bo));
    const uint64_t handle = bo.handle;
    const uint32_t token = bo.token;

    ASSERT_EQ(kernel.park(handle, true), KERN_SUCCESS);
    ASSERT_EQ(kernel.park(handle, false), KERN_SUCCESS);
    ASSERT_EQ(model.parks.size(), 2u);
    EXPECT_EQ(model.parks[0].handle, handle);
    EXPECT_EQ(model.parks[0].flags, uint32_t(TegraGM20BParkGuard));
    EXPECT_EQ(model.parks[1].flags, 0u);

    uint64_t address = 0, bytes = 0;
    uint32_t status = 0;
    model.nextUnparkStatus = TegraGM20BUnparkResident;
    ASSERT_EQ(kernel.unpark(handle, address, bytes, status), KERN_SUCCESS);
    EXPECT_EQ(address, model.nextUnparkAddress);
    EXPECT_EQ(bytes, model.nextUnparkBytes);
    EXPECT_EQ(status, uint32_t(TegraGM20BUnparkResident));

    model.nextUnparkStatus = TegraGM20BUnparkReclaimed;
    ASSERT_EQ(kernel.unpark(handle, address, bytes, status), KERN_SUCCESS);
    EXPECT_EQ(status, uint32_t(TegraGM20BUnparkReclaimed));

    constexpr uint64_t kFixedVa = 0x70000000;
    ASSERT_TRUE(kernel.mapAt(token, 0x10000, kFixedVa));
    ASSERT_EQ(model.fixedMaps.size(), 1u);
    EXPECT_EQ(model.fixedMaps[0].token, token);
    EXPECT_EQ(model.fixedMaps[0].flags, uint32_t(TegraGM20BMapFixed));
    EXPECT_EQ(model.fixedMaps[0].address, kFixedVa);

    const size_t unmapsBefore = model.unmaps.size();
    bo.parked = true;
    kernel.free(bo);
    EXPECT_EQ(model.unmaps.size(), unmapsBefore);
    ASSERT_FALSE(model.frees.empty());
    EXPECT_EQ(model.frees.back(), handle);

    gm20b::Bo sysBo;
    ASSERT_TRUE(kernel.alloc(0x10000, true, sysBo, 0, 0x10000, false, false, true, 0, true));
    ASSERT_FALSE(model.allocFlags.empty());
    EXPECT_EQ(model.allocFlags.back(), uint32_t(TegraGM20BBoCoherent | TegraGM20BBoSystem));
    kernel.free(sysBo);

    model = {};
    model.advertiseBoPark = false;
    gm20b::Kernel noPark;
    std::string err;
    ASSERT_TRUE(noPark.open(err)) << err;
    EXPECT_FALSE(noPark.supportsBoPark());
    EXPECT_EQ(noPark.park(1, true), kIOReturnUnsupported);
    EXPECT_EQ(noPark.unpark(1, address, bytes, status), kIOReturnUnsupported);
}
