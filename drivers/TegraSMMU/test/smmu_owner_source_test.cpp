// The MC SMMU's PTB_ASID/PTB_DATA/TLB_FLUSH/PTC_FLUSH registers are global to every swgroup: only TegraSMMU.cpp
// programs them (under its lock). This reads every driver's C++ (the tree's sources, in the test's data) for their
// names; and TegraVIC attaches through TegraSMMU with a TegraSMMUDomain.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::PackagePath;
using switch_ios::test::SourceText;
using testing::HasSubstr;

TEST(TegraSMMUSource, OneOwnerOfTheGlobalRegisters)
{
    std::string root = PackagePath("../../");  // the drivers/ directory of the tree, as the test's data holds it
    if (!root.empty() && root.back() == '/')
        root.pop_back();
    const std::filesystem::path drivers = root;
    unsigned units = 0;
    bool sawOwner = false;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(drivers)) {
        const std::filesystem::path &path = entry.path();
        if (path.extension() != ".cpp" || !std::filesystem::is_regular_file(path))
            continue;
        const std::string name = path.lexically_relative(drivers).generic_string();
        if (name == "TegraSMMU/TegraSMMU.cpp") {
            sawOwner = true;
            continue;
        }
        ++units;
        std::ifstream file(path, std::ios::binary);
        std::ostringstream text;
        text << file.rdbuf();
        for (const char *global : {"MC_SMMU_PTB_ASID", "MC_SMMU_PTB_DATA", "MC_SMMU_TLB_FLUSH", "MC_SMMU_PTC_FLUSH"})
            EXPECT_EQ(text.str().find(global), std::string::npos) << name << " touches " << global;
    }
    EXPECT_TRUE(sawOwner) << "TegraSMMU/TegraSMMU.cpp is not in the test's data";
    EXPECT_GT(units, 50u) << "the drivers' sources are not in the test's data";
}

TEST(TegraSMMUSource, VicAttachesThroughTheSharedDomain)
{
    const std::string vic = SourceText("../../TegraVIC/TegraVIC.cpp");
    EXPECT_THAT(vic, HasSubstr("tegra_smmu_attach("));
    EXPECT_THAT(vic, HasSubstr("TegraSMMUDomain"));
}
