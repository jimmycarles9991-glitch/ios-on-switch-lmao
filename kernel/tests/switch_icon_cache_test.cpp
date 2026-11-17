// kernel/switch/boot/icon_cache.c over a host directory laid out as the Data volume, in the cache layout of the XNU this
// target compiles it for: XNU_12377_121_6 drops 24B119's salt folders' __cache_info__, XNU_10063_141_1 21H461's at the
// cache folder's top.
#include <gtest/gtest.h>

#include <dirent.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" {
#include <switch/icon_cache.h>
}

namespace {
namespace fs = std::filesystem;

const fs::path kCache =
    "containers/Shared/SystemGroup/systemgroup.com.apple.lsd.iconscache/Library/Caches/com.apple.IconsCache";
const char *const kSalts[] = {"28A6C7EA-97A4-3002-8473-901196984E6C", "5D0D51D1-4930-479F-AA26-F31E82658A70"};
const std::string kBuildA = std::string(64, 'a') + "\n", kBuildB = std::string(64, 'b') + "\n";

struct Data {
    fs::path root;
    std::vector<std::string> unlinked;
    int unlinkError = 0;
};

std::string Contents(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void Put(const fs::path &path, const std::string &bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << bytes;
}

int Read(void *context, const char *path, char *buffer, size_t size, size_t *length)
{
    const fs::path file = static_cast<Data *>(context)->root / path;
    if (!fs::exists(file))
        return ENOENT;
    const std::string bytes = Contents(file);
    if (bytes.size() > size)
        return EFBIG;
    bytes.copy(buffer, bytes.size());
    *length = bytes.size();
    return 0;
}

bool Holds(void *context, const char *path, const char *bytes, size_t length)
{
    const fs::path file = static_cast<Data *>(context)->root / path;
    return fs::exists(file) && Contents(file) == std::string(bytes, length);
}

int Write(void *context, const char *path, const char *bytes, size_t length)
{
    Put(static_cast<Data *>(context)->root / path, std::string(bytes, length));
    return 0;
}

int Unlink(void *context, const char *path)
{
    auto *data = static_cast<Data *>(context);
    if (data->unlinkError)
        return data->unlinkError;
    if (::unlink((data->root / path).c_str()))
        return errno;
    data->unlinked.push_back(path);
    return 0;
}

int Folders(void *context, const char *path, int (*visit)(void *walk, const char *name), void *walk)
{
    DIR *directory = opendir((static_cast<Data *>(context)->root / path).c_str());
    if (!directory)
        return errno;
    int error = 0;
    for (const dirent *entry = readdir(directory); entry && !error; entry = readdir(directory)) {
        if (entry->d_type == DT_DIR && entry->d_name[0] != '.')
            error = visit(walk, entry->d_name);
    }
    closedir(directory);
    return error;
}

class SwitchIconCache : public testing::Test {
protected:
    void SetUp() override
    {
        const char *tmp = std::getenv("TEST_TMPDIR");
        std::string pattern = (fs::path(tmp ? tmp : "/tmp") / "icon-cache-XXXXXX").string();
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        data.root = pattern;
        Put(data.root / kCache / "__cache_info__", "top");
        Put(data.root / kCache / "store.index", "index");
        for (const char *salt : kSalts) {
            Put(data.root / kCache / salt / "__cache_info__", salt);
            Put(data.root / kCache / salt / "store.index", "index");
        }
    }

    void TearDown() override
    {
        fs::remove_all(data.root);
    }

    switch_icon_cache_result Drop()
    {
        const switch_icon_cache_ops ops = {&data, Read, Holds, Write, Unlink, Folders};
        switch_icon_cache_result result;
        switch_icon_cache_drop(&ops, &result);
        return result;
    }

    // The __cache_info__ files this XNU's layout drops: 24B119's in each salt folder, 21H461's at the top.
    static std::vector<fs::path> Dropped()
    {
#if XNU_12377_121_6
        return {kCache / kSalts[0] / "__cache_info__", kCache / kSalts[1] / "__cache_info__"};
#else
        return {kCache / "__cache_info__"};
#endif
    }

    std::vector<fs::path> Present() const
    {
        std::vector<fs::path> files;
        for (const auto &entry : fs::recursive_directory_iterator(data.root / kCache))
            files.push_back(fs::relative(entry.path(), data.root));
        std::sort(files.begin(), files.end());
        return files;
    }

    Data data;
};

TEST_F(SwitchIconCache, NoRequestIsNothing)
{
    const auto before = Present();
    const switch_icon_cache_result result = Drop();
    EXPECT_FALSE(result.requested);
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(Present(), before);
    EXPECT_FALSE(fs::exists(data.root / "Switch/icon-cache-reset"));
}

TEST_F(SwitchIconCache, AnUnchangedMarkerRemovesNothing)
{
    Put(data.root / "Switch/icon-cache-gpu", kBuildA);
    Put(data.root / "Switch/icon-cache-reset", kBuildA);
    const auto before = Present();
    const switch_icon_cache_result result = Drop();
    EXPECT_FALSE(result.requested);
    EXPECT_TRUE(data.unlinked.empty());
    EXPECT_EQ(Present(), before);
}

TEST_F(SwitchIconCache, AChangedMarkerRemovesOnlyCacheInfoAndOnlyOnce)
{
    Put(data.root / "Switch/icon-cache-gpu", kBuildB);
    Put(data.root / "Switch/icon-cache-reset", kBuildA);
    auto expected = Present();
    for (const fs::path &file : Dropped())
        std::erase(expected, file);
    const switch_icon_cache_result result = Drop();
    EXPECT_TRUE(result.requested);
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(result.dropped, static_cast<int>(Dropped().size()));
    EXPECT_STREQ(result.build, "bbbbbbbbbbbb");
    EXPECT_EQ(Present(), expected) << "only __cache_info__ goes: the folders and the stores stay";
    EXPECT_EQ(Contents(data.root / "Switch/icon-cache-reset"), kBuildB);
    data.unlinked.clear();
    EXPECT_FALSE(Drop().requested);
    EXPECT_TRUE(data.unlinked.empty()) << "the same build again drops nothing";
}

#if XNU_12377_121_6
TEST_F(SwitchIconCache, Xnu12377DropsEverySaltFolder)
{
    Put(data.root / "Switch/icon-cache-gpu", kBuildB);
    EXPECT_EQ(Drop().dropped, 2);
    for (const char *salt : kSalts)
        EXPECT_FALSE(fs::exists(data.root / kCache / salt / "__cache_info__")) << salt;
    EXPECT_TRUE(fs::exists(data.root / kCache / "__cache_info__")) << "24B119 keeps no cache at the folder's top";
}
#else
TEST_F(SwitchIconCache, Xnu10063DropsTheTopOnly)
{
    Put(data.root / "Switch/icon-cache-gpu", kBuildB);
    EXPECT_EQ(Drop().dropped, 1);
    EXPECT_FALSE(fs::exists(data.root / kCache / "__cache_info__"));
    for (const char *salt : kSalts)
        EXPECT_TRUE(fs::exists(data.root / kCache / salt / "__cache_info__")) << "21H461 has no salt folders";
}
#endif

TEST_F(SwitchIconCache, NoCacheYetRecordsTheBuild)
{
    fs::remove_all(data.root / "containers");
    Put(data.root / "Switch/icon-cache-gpu", kBuildA);
    const switch_icon_cache_result result = Drop();
    EXPECT_TRUE(result.requested);
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(result.dropped, 0);
    EXPECT_EQ(Contents(data.root / "Switch/icon-cache-reset"), kBuildA);
}

TEST_F(SwitchIconCache, AFailedUnlinkLeavesTheMarkerForTheNextBoot)
{
    Put(data.root / "Switch/icon-cache-gpu", kBuildB);
    Put(data.root / "Switch/icon-cache-reset", kBuildA);
    data.unlinkError = EIO;
    const switch_icon_cache_result result = Drop();
    EXPECT_EQ(result.error, EIO);
    EXPECT_EQ(Contents(data.root / "Switch/icon-cache-reset"), kBuildA);
}
}  // namespace
