// SPDX-License-Identifier: MIT
// Core Image's kernel lookup over the selected IPSW's own libraries: CI::PrecompiledKernels::new_specialzied_function asks
// ci_stdlib, then ci_filters, then ci_raw for a kernel and takes the first library that makes it. The kernels the device
// logs EntryNotFound for (iconservicesagent's _asgDownH and _asgDownV, HW Test's _gaussianBlur13, _cubicUpsample10 and
// _gaussianReduce4) are ci_filters' entries and none of ci_stdlib's, so the first ask misses on a real device as well:
// the frontend answers it as Metal does (NBLibrary's functionNotFound) and the second makes the kernel.
#include "drivers/NativeMetal/MetallibEntries.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>
#include "MetalABI.h"

namespace {

struct Libraries {
    std::string_view kind;
    size_t stdlib, filters;  // their entries
};

constexpr Libraries kLibraries[] = {{"21H461", 200, 563}, {"24B119", 233, 615}};

std::string coreImage(const char *name)
{
    const std::string quartzCore = getenv("SWITCH_QUARTZCORE") ?: "";
    const std::string framework = "QuartzCore.framework/default.metallib";
    if (quartzCore.size() < framework.size())
        return {};
    return quartzCore.substr(0, quartzCore.size() - framework.size()) + "CoreImage.framework/" + name;
}

std::set<std::string> entries(const char *name)
{
    std::ifstream in(coreImage(name), std::ios::binary);
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::set<std::string> names;
    switch_metal::forEachMetallibEntry(file.data(), file.size(), [&](const switch_metal::MetallibEntry &entry) {
        names.insert(entry.name);
        return true;
    });
    return names;
}

TEST(CoreImageProbe, TheKernelsTheDeviceMissedAreCiFiltersAndNoneOfCiStdlibs)
{
    const Libraries *kind = nullptr;
    for (const Libraries &l : kLibraries)
        if (l.kind == SWITCH_METAL_KIND)
            kind = &l;
    ASSERT_NE(kind, nullptr) << SWITCH_METAL_KIND;
    const std::set<std::string> stdlib = entries("ci_stdlib.metallib"), filters = entries("ci_filters.metallib");
    EXPECT_EQ(stdlib.size(), kind->stdlib);
    EXPECT_EQ(filters.size(), kind->filters);
    for (const char *kernel : {"_asgDownH", "_asgDownV", "_gaussianBlur13", "_cubicUpsample10", "_gaussianReduce4"}) {
        EXPECT_EQ(stdlib.count(kernel), 0u) << kernel;
        EXPECT_EQ(filters.count(kernel), 1u) << kernel;
    }
}

}  // namespace
