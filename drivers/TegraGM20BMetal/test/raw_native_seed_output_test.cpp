// SPDX-License-Identifier: MIT
// raw/pack/native_seed_manifest.h openOutput: the native seed tool's output directory. A step that fails prints one
// line naming the step, the path and errno; a directory that already has the mode the outputs need is not chmodded, so
// one another user made (here: one the user-immutable flag keeps from chmod, which refuses with EPERM as for another
// user's) passes that step, and fails only where the tool cannot write into it. The flag binds root too, as a remote
// worker may run as root.
#include "../raw/pack/native_seed_manifest.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdlib>
#include <string>

namespace {

constexpr uint64_t kStack = 0x1234;

std::string scratch(const char *name)
{
    const char *tmp = getenv("TEST_TMPDIR");
    const std::string dir = std::string(tmp ? tmp : "/tmp") + "/" + name;
    mkdir(dir.c_str(), 0755);
    return dir;
}

// The flag off again whatever the test did, so the test's directory can be removed
struct Immutable {
    std::string path;

    Immutable(const std::string &p, mode_t mode) : path(p)
    {
        mkdir(path.c_str(), mode);
        chmod(path.c_str(), mode);
        chflags(path.c_str(), UF_IMMUTABLE);
    }

    ~Immutable()
    {
        chflags(path.c_str(), 0);
        chmod(path.c_str(), 0755);
    }
};

std::string opened(const std::string &output, bool &ok)
{
    raw::RecordCache cache;
    testing::internal::CaptureStderr();
    ok = rawseed::openOutput(output, kStack, cache);
    return testing::internal::GetCapturedStderr();
}

TEST(NativeSeedOutput, AFreshDirectoryOpensSilently)
{
    bool ok = false;
    const std::string said = opened(scratch("fresh") + "/out", ok);
    EXPECT_TRUE(ok);
    EXPECT_EQ(said, "");
}

TEST(NativeSeedOutput, ADirectoryItCannotCreateNamesTheStepThePathAndErrno)
{
    const Immutable parent(scratch("immutable-parent") + "/in", 0755);
    bool ok = true;
    const std::string said = opened(parent.path + "/out", ok);
    EXPECT_FALSE(ok);
    EXPECT_EQ(said,
              "native-seed: create the output directory " + parent.path + "/out: Operation not permitted (errno 1)\n");
}

TEST(NativeSeedOutput, AReadOnlyDirectoryItCannotChmodNamesTheChmod)
{
    const Immutable dir(scratch("readonly") + "/out", 0555);
    bool ok = true;
    const std::string said = opened(dir.path, ok);
    EXPECT_FALSE(ok);
    EXPECT_EQ(said, "native-seed: chmod " + dir.path + ": Operation not permitted (errno 1)\n");
}

TEST(NativeSeedOutput, ADirectoryAtItsModeIsNotChmoddedAndFailsOnlyWhereItCannotWrite)
{
    const Immutable dir(scratch("at-mode") + "/out", 0755);
    bool ok = true;
    const std::string said = opened(dir.path, ok);
    EXPECT_FALSE(ok);
    EXPECT_EQ(said, "native-seed: attach the record cache in " + dir.path + ": Operation not permitted (errno 1)\n");
}

}  // namespace
