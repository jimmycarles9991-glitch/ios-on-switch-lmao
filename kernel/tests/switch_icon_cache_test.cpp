// kernel/switch/boot/icon_cache.c over a host directory laid out as the Data volume, in the cache layout of the XNU
// this target compiles it for: XNU_12377_121_6 atomically renames 24B119's salt folders aside, into
// Switch/icon-cache-keep for the build the marker names (and back when that build returns) or into
// Switch/icon-cache-trash, swept post-boot; XNU_10063_141_1 keeps unlinking 21H461's top-level __cache_info__.
#include <gtest/gtest.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <switch/icon_cache.h>

namespace {
namespace fs = std::filesystem;

const fs::path kCache =
    "containers/Shared/SystemGroup/systemgroup.com.apple.lsd.iconscache/Library/Caches/com.apple.IconsCache";
const fs::path kTrash = SWITCH_ICON_CACHE_TRASH;
const fs::path kKeep = SWITCH_ICON_CACHE_KEEP;
const char *const kSalts[] = {"28A6C7EA-97A4-3002-8473-901196984E6C", "5D0D51D1-4930-479F-AA26-F31E82658A70"};
const char kSession[] = "3349B3E8-362E-46C3-A562-EB07470C449B";
const std::string kBuildA = std::string(64, 'a') + "\n", kBuildB = std::string(64, 'b') + "\n";
const std::string kBuildC = std::string(64, 'c') + "\n";

struct Data {
    fs::path root;
    std::vector<std::string> unlinked;
    std::vector<std::pair<std::string, std::string>> renamed;
    std::vector<std::pair<std::string, unsigned int>> mkdirs;
    int unlinkError = 0;
    int renameError = 0;
    int renameFailAt = 0;  // 0 = fail every call when renameError != 0; N > 0 = fail Nth call
    int renameCalls = 0;
    int mkdirError = 0;
    int existsCalls = 0;
    const char *writeFailAt = nullptr;  // a path whose write fails with EIO
    const char *session = kSession;
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
    auto *data = static_cast<Data *>(context);
    if (data->writeFailAt && std::string(path) == data->writeFailAt)
        return EIO;
    Put(data->root / path, std::string(bytes, length));
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
    std::vector<std::string> names;
    for (const dirent *entry = readdir(directory); entry; entry = readdir(directory)) {
        if (entry->d_type == DT_DIR && entry->d_name[0] != '.')
            names.emplace_back(entry->d_name);
    }
    closedir(directory);
    std::sort(names.begin(), names.end());
    int error = 0;
    for (const std::string &name : names) {
        error = visit(walk, name.c_str());
        if (error)
            break;
    }
    return error;
}

int Mkdir(void *context, const char *path, unsigned int mode)
{
    auto *data = static_cast<Data *>(context);
    if (data->mkdirError)
        return data->mkdirError;
    const fs::path full = data->root / path;
    data->mkdirs.emplace_back(path, mode);
    if (fs::exists(full))
        return fs::is_directory(full) ? 0 : ENOTDIR;
    fs::create_directories(full.parent_path());
    if (::mkdir(full.c_str(), static_cast<mode_t>(mode)))
        return errno;
    return 0;
}

int Rename(void *context, const char *from, const char *to)
{
    auto *data = static_cast<Data *>(context);
    ++data->renameCalls;
    if (data->renameError && (data->renameFailAt == 0 || data->renameCalls == data->renameFailAt))
        return data->renameError;
    if (::rename((data->root / from).c_str(), (data->root / to).c_str()))
        return errno;
    data->renamed.emplace_back(from, to);
    return 0;
}

bool Exists(void *context, const char *path)
{
    auto *data = static_cast<Data *>(context);
    ++data->existsCalls;
    struct stat st{};
    return ::lstat((data->root / path).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
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
            Put(data.root / kCache / salt / "__METAL_CACHE__" / "shader.bin", "metal");
        }
    }

    void TearDown() override
    {
        fs::remove_all(data.root);
    }

    switch_icon_cache_result Drop()
    {
        const switch_icon_cache_ops ops = {
            &data, Read, Holds, Write, Unlink, Folders, Mkdir, Rename, Exists, data.session,
        };
        switch_icon_cache_result result;
        switch_icon_cache_drop(&ops, &result);
        return result;
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

TEST_F(SwitchIconCache, NoDeployRecordLeavesTheCacheAlone)
{
    const std::vector<fs::path> before = Present();
    const switch_icon_cache_result result = Drop();
    EXPECT_FALSE(result.requested);
    EXPECT_EQ(result.dropped, 0);
    EXPECT_EQ(result.error, 0);
    EXPECT_FALSE(result.trash_pending);
    EXPECT_TRUE(data.unlinked.empty());
    EXPECT_TRUE(data.renamed.empty());
    EXPECT_EQ(Present(), before);
    EXPECT_FALSE(fs::exists(data.root / "Switch/icon-cache-reset"));
#if XNU_12377_121_6
    EXPECT_EQ(data.existsCalls, 1);
#endif
}

TEST_F(SwitchIconCache, MatchingMarkerDropsNothingAndProbesTrashOnce)
{
    const std::vector<fs::path> before = Present();
    Put(data.root / "Switch/icon-cache-gpu", kBuildA);
    Put(data.root / "Switch/icon-cache-reset", kBuildA);
    const switch_icon_cache_result result = Drop();
    EXPECT_FALSE(result.requested);
    EXPECT_EQ(result.dropped, 0);
    EXPECT_EQ(result.error, 0);
    EXPECT_FALSE(result.trash_pending);
    EXPECT_TRUE(data.unlinked.empty());
    EXPECT_TRUE(data.renamed.empty());
    EXPECT_TRUE(data.mkdirs.empty());
    EXPECT_EQ(Present(), before);
#if XNU_12377_121_6
    EXPECT_EQ(data.existsCalls, 1);
#endif
}

TEST_F(SwitchIconCache, NoCacheYetRecordsTheBuild)
{
    fs::remove_all(data.root / "containers");
    Put(data.root / "Switch/icon-cache-gpu", kBuildA);
    const switch_icon_cache_result result = Drop();
    EXPECT_TRUE(result.requested);
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(result.dropped, 0);
    EXPECT_FALSE(result.trash_pending);
    EXPECT_TRUE(data.mkdirs.empty());
    EXPECT_FALSE(fs::exists(data.root / kTrash));
    EXPECT_EQ(Contents(data.root / "Switch/icon-cache-reset"), kBuildA);
}

#if XNU_12377_121_6
const char kKeyA[] = "aaaaaaaaaaaa", kKeyB[] = "bbbbbbbbbbbb";

TEST_F(SwitchIconCache, Xnu12377KeepsTheMarkersBuildsFoldersAndWritesMarker)
{
    Put(data.root / "Switch/icon-cache-gpu", kBuildB);
    Put(data.root / "Switch/icon-cache-reset", kBuildA);
    const switch_icon_cache_result result = Drop();
    EXPECT_TRUE(result.requested);
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(result.kept, 2);
    EXPECT_EQ(result.dropped, 0);
    EXPECT_EQ(result.restored, 0);
    EXPECT_FALSE(result.trash_pending);
    EXPECT_STREQ(result.build, kKeyB);
    EXPECT_TRUE(data.unlinked.empty()) << "24B119 renames salt folders aside instead of unlinking __cache_info__";
    ASSERT_EQ(data.mkdirs.size(), 2u);
    EXPECT_EQ(data.mkdirs[0].first, SWITCH_ICON_CACHE_KEEP);
    EXPECT_EQ(data.mkdirs[1].first, (kKeep / kKeyA).string());
    EXPECT_EQ(data.mkdirs[1].second, 0700u);
    for (const char *salt : kSalts) {
        EXPECT_FALSE(fs::exists(data.root / kCache / salt)) << salt;
        const fs::path kept = data.root / kKeep / kKeyA / salt;
        EXPECT_EQ(Contents(kept / "__cache_info__"), salt);
        EXPECT_TRUE(fs::exists(kept / "__METAL_CACHE__" / "shader.bin")) << salt;
    }
    EXPECT_TRUE(fs::exists(data.root / kCache / "__cache_info__")) << "24B119 moves salt folders only";
    EXPECT_EQ(Contents(data.root / "Switch/icon-cache-reset"), kBuildB);
    EXPECT_FALSE(fs::exists(data.root / kTrash));
}

TEST_F(SwitchIconCache, Xnu12377AReturningBuildGetsItsFoldersBack)
{
    Put(data.root / "Switch/icon-cache-gpu", kBuildB);
    Put(data.root / "Switch/icon-cache-reset", kBuildA);
    ASSERT_EQ(Drop().kept, 2);
    for (const char *salt : kSalts)
        Put(data.root / kCache / salt / "__cache_info__", std::string("drawn by B ") + salt);
    Put(data.root / "Switch/icon-cache-gpu", kBuildA);
    const switch_icon_cache_result result = Drop();
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(result.kept, 2);
    EXPECT_EQ(result.restored, 2);
    EXPECT_EQ(result.dropped, 0);
    EXPECT_TRUE(result.trash_pending) << "the emptied keep folder goes to the sweep";
    for (const char *salt : kSalts) {
        EXPECT_EQ(Contents(data.root / kCache / salt / "__cache_info__"), salt) << "A's own icons are back";
        EXPECT_EQ(Contents(data.root / kKeep / kKeyB / salt / "__cache_info__"), std::string("drawn by B ") + salt);
    }
    EXPECT_FALSE(fs::exists(data.root / kKeep / kKeyA));
    EXPECT_TRUE(fs::is_empty(data.root / kTrash / (std::string(kSession) + "-keep-" + kKeyA)));
    EXPECT_EQ(Contents(data.root / "Switch/icon-cache-reset"), kBuildA);
#if SWITCH_TEST_RELEASE
    EXPECT_FALSE(fs::exists(data.root / "Switch/icon-cache-restored")) << "a release kernel keeps no diagnostic record";
#else
    EXPECT_EQ(Contents(data.root / "Switch/icon-cache-restored"), kBuildA);
#endif
}

TEST_F(SwitchIconCache, Xnu12377TheKeepHoldsOneBuild)
{
    Put(data.root / "Switch/icon-cache-gpu", kBuildB);
    Put(data.root / "Switch/icon-cache-reset", kBuildA);
    ASSERT_EQ(Drop().kept, 2);
    for (const char *salt : kSalts)
        Put(data.root / kCache / salt / "store.index", "B");
    Put(data.root / "Switch/icon-cache-gpu", kBuildC);
    const switch_icon_cache_result result = Drop();
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(result.kept, 2);
    EXPECT_EQ(result.restored, 0);
    EXPECT_TRUE(result.trash_pending);
    EXPECT_FALSE(fs::exists(data.root / kKeep / kKeyA));
    EXPECT_TRUE(fs::exists(data.root / kKeep / kKeyB));
    EXPECT_TRUE(fs::is_empty(data.root / kTrash / (std::string(kSession) + "-keep-" + kKeyA)));
    for (const char *salt : kSalts) {
        EXPECT_FALSE(fs::exists(data.root / kCache / salt)) << "C starts with an empty cache";
        const fs::path evicted = data.root / kTrash / (std::string(kSession) + "-keep-" + kKeyA + "-" + salt);
        EXPECT_TRUE(fs::exists(evicted / "__METAL_CACHE__" / "shader.bin")) << "at the sweep's depth for a salt folder";
    }
    EXPECT_FALSE(fs::exists(data.root / "Switch/icon-cache-restored"));
}

TEST_F(SwitchIconCache, Xnu12377WithoutAMarkerMovesEverySaltFolderToTrash)
{
    Put(data.root / "Switch/icon-cache-gpu", kBuildB);
    const switch_icon_cache_result result = Drop();
    EXPECT_TRUE(result.requested);
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(result.dropped, 2);
    EXPECT_EQ(result.kept, 0);
    EXPECT_TRUE(result.trash_pending);
    ASSERT_EQ(data.mkdirs.size(), 1u);
    EXPECT_EQ(data.mkdirs[0].first, SWITCH_ICON_CACHE_TRASH);
    EXPECT_EQ(data.mkdirs[0].second, 0700u);
    EXPECT_EQ(data.renamed.size(), 2u);
    for (const char *salt : kSalts) {
        EXPECT_FALSE(fs::exists(data.root / kCache / salt)) << salt;
        const fs::path moved = data.root / kTrash / (std::string(kSession) + "-" + salt);
        EXPECT_TRUE(fs::exists(moved / "__cache_info__")) << salt;
        EXPECT_TRUE(fs::exists(moved / "store.index")) << salt;
        EXPECT_TRUE(fs::exists(moved / "__METAL_CACHE__" / "shader.bin")) << salt;
    }
    EXPECT_EQ(Contents(data.root / "Switch/icon-cache-reset"), kBuildB);

    data.renamed.clear();
    data.mkdirs.clear();
    data.existsCalls = 0;
    const switch_icon_cache_result second = Drop();
    EXPECT_FALSE(second.requested);
    EXPECT_TRUE(second.trash_pending) << "leftover trash is still detected on subsequent boots";
    EXPECT_EQ(data.existsCalls, 1);
    EXPECT_TRUE(data.renamed.empty());
    EXPECT_TRUE(data.mkdirs.empty());
}

TEST_F(SwitchIconCache, OrdinaryBootDetectsLeftoverTrashFromCrashedBoot)
{
    Put(data.root / "Switch/icon-cache-gpu", kBuildA);
    Put(data.root / "Switch/icon-cache-reset", kBuildA);
    Put(data.root / kTrash / "prior-salt/store.index", "stale");
    const switch_icon_cache_result result = Drop();
    EXPECT_FALSE(result.requested);
    EXPECT_EQ(result.error, 0);
    EXPECT_TRUE(result.trash_pending);
    EXPECT_EQ(data.existsCalls, 1);
    EXPECT_TRUE(data.renamed.empty());
}

TEST_F(SwitchIconCache, FailedRenameFailsClosedWithoutAdvancingMarker)
{
    Put(data.root / "Switch/icon-cache-gpu", kBuildB);
    Put(data.root / "Switch/icon-cache-reset", kBuildA);
    data.renameError = EIO;
    data.renameFailAt = 2;
    const switch_icon_cache_result result = Drop();
    EXPECT_TRUE(result.requested);
    EXPECT_EQ(result.error, EIO);
    EXPECT_EQ(result.kept, 1);
    EXPECT_EQ(Contents(data.root / "Switch/icon-cache-reset"), kBuildA);

    data.renameError = 0;
    const switch_icon_cache_result again = Drop();
    EXPECT_EQ(again.error, 0);
    EXPECT_EQ(again.kept, 0) << "a drop began and stopped: whatever the cache holds now is not kept as A's";
    EXPECT_EQ(again.dropped, 1);
    EXPECT_TRUE(again.trash_pending);
    EXPECT_FALSE(fs::exists(data.root / kKeep / kKeyA));
    EXPECT_EQ(Contents(data.root / "Switch/icon-cache-reset"), kBuildB);
}

// Review S4 (a): a drop for A moves A's kept folders back, then stops before the marker is written (here, its write
// fails). The agent then draws into the cache on that boot, and the next deploy names C. The marker still names B, but
// the cache is not B's: it goes to the trash, and keep/B is not given anything.
// Review F1: the same stopped drop for A, but the next deploy names B again, the marker's own build. The drop is redone:
// the cache (A's folders, and what A's agent drew) goes to the trash and B gets its kept folders back.
TEST_F(SwitchIconCache, ADropThatStoppedIsRedoneForTheMarkersOwnBuild)
{
    Put(data.root / "Switch/icon-cache-gpu", kBuildB);
    Put(data.root / "Switch/icon-cache-reset", kBuildA);
    ASSERT_EQ(Drop().kept, 2);
    for (const char *salt : kSalts)
        Put(data.root / kCache / salt / "__cache_info__", std::string("drawn by B ") + salt);
    Put(data.root / "Switch/icon-cache-gpu", kBuildA);
    data.writeFailAt = "Switch/icon-cache-reset";
    ASSERT_NE(Drop().error, 0);
    data.writeFailAt = nullptr;
    for (const char *salt : kSalts)
        Put(data.root / kCache / salt / "store.index", "drawn by A's agent");

    Put(data.root / "Switch/icon-cache-gpu", kBuildB);
    const switch_icon_cache_result again = Drop();
    EXPECT_TRUE(again.requested);
    EXPECT_EQ(again.error, 0);
    EXPECT_EQ(again.kept, 0);
    EXPECT_EQ(again.dropped, 2);
    EXPECT_EQ(again.restored, 2);
    for (const char *salt : kSalts) {
        EXPECT_EQ(Contents(data.root / kCache / salt / "__cache_info__"), std::string("drawn by B ") + salt);
        EXPECT_FALSE(fs::exists(data.root / kCache / salt / "store.index")) << "nothing of A's is left";
    }
    EXPECT_EQ(Contents(data.root / "Switch/icon-cache-reset"), kBuildB);
}

TEST_F(SwitchIconCache, ADropThatStoppedKeepsNothingUnderTheMarkersBuild)
{
    Put(data.root / "Switch/icon-cache-gpu", kBuildB);
    Put(data.root / "Switch/icon-cache-reset", kBuildA);
    ASSERT_EQ(Drop().kept, 2);
    for (const char *salt : kSalts)
        Put(data.root / kCache / salt / "__cache_info__", std::string("drawn by B ") + salt);
    Put(data.root / "Switch/icon-cache-gpu", kBuildA);
    data.writeFailAt = "Switch/icon-cache-reset";
    const switch_icon_cache_result stopped = Drop();
    EXPECT_NE(stopped.error, 0);
    EXPECT_EQ(stopped.restored, 2);
    EXPECT_EQ(Contents(data.root / "Switch/icon-cache-reset"), kBuildB);
    data.writeFailAt = nullptr;

    for (const char *salt : kSalts)
        Put(data.root / kCache / salt / "store.index", "drawn by A's agent, under A's folders");
    Put(data.root / "Switch/icon-cache-gpu", kBuildC);
    const switch_icon_cache_result next = Drop();
    EXPECT_EQ(next.error, 0);
    EXPECT_EQ(next.kept, 0);
    EXPECT_EQ(next.dropped, 2);
    EXPECT_FALSE(fs::exists(data.root / kKeep / kKeyB)) << "B's keep was B's folders before they were given to A";
    EXPECT_EQ(Contents(data.root / "Switch/icon-cache-reset"), kBuildC);
}

TEST_F(SwitchIconCache, FailedMkdirFailsClosedWithoutRenamingOrAdvancingMarker)
{
    Put(data.root / "Switch/icon-cache-gpu", kBuildB);
    Put(data.root / "Switch/icon-cache-reset", kBuildA);
    data.mkdirError = EACCES;
    const switch_icon_cache_result result = Drop();
    EXPECT_EQ(result.error, EACCES);
    EXPECT_EQ(result.dropped, 0);
    EXPECT_EQ(result.kept, 0);
    EXPECT_TRUE(data.renamed.empty());
    EXPECT_EQ(Contents(data.root / "Switch/icon-cache-reset"), kBuildA);
}

TEST_F(SwitchIconCache, RenameEnoentIsNonFatal)
{
    Put(data.root / "Switch/icon-cache-gpu", kBuildB);
    Put(data.root / "Switch/icon-cache-reset", kBuildA);
    data.renameError = ENOENT;
    data.renameFailAt = 1;
    const switch_icon_cache_result result = Drop();
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(result.kept, 1);
    EXPECT_EQ(Contents(data.root / "Switch/icon-cache-reset"), kBuildB);
}

TEST_F(SwitchIconCache, InvalidBootSessionFailsClosed)
{
    Put(data.root / "Switch/icon-cache-gpu", kBuildB);
    data.session = "bad/session";
    const switch_icon_cache_result result = Drop();
    EXPECT_EQ(result.error, EINVAL);
    EXPECT_TRUE(data.renamed.empty());
    EXPECT_FALSE(fs::exists(data.root / "Switch/icon-cache-reset"));
}

struct SweepHost {
    fs::path root;
    std::set<std::string> foreignPaths;
    std::vector<std::string> opsLog;
    std::vector<std::string> openedDirs;
    size_t maxBatchEntries = 64;
};

int SweepReadBatch(void *context, const char *path, char *names, size_t names_size, size_t *count)
{
    auto *host = static_cast<SweepHost *>(context);
    *count = 0;
    if (host->foreignPaths.count(path))
        return EXDEV;
    const fs::path full = host->root / path;
    struct stat st{};
    if (::lstat(full.c_str(), &st) != 0)
        return errno;
    if (S_ISLNK(st.st_mode))
        return ELOOP;
    if (!S_ISDIR(st.st_mode))
        return ENOTDIR;
    host->openedDirs.emplace_back(path);

    DIR *dir = opendir(full.c_str());
    if (!dir)
        return errno;
    std::vector<std::string> entries;
    for (const dirent *ent = readdir(dir); ent; ent = readdir(dir)) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0)
            continue;
        entries.emplace_back(ent->d_name);
    }
    closedir(dir);
    std::sort(entries.begin(), entries.end());

    size_t used = 0;
    for (const std::string &name : entries) {
        if (*count >= host->maxBatchEntries)
            break;
        if (used + name.size() + 1 > names_size) {
            if (*count == 0)
                return ENAMETOOLONG;
            break;
        }
        std::memcpy(names + used, name.c_str(), name.size() + 1);
        used += name.size() + 1;
        ++(*count);
    }
    return 0;
}

int SweepInspect(void *context, const char *path, switch_icon_cache_entry_kind *kind)
{
    auto *host = static_cast<SweepHost *>(context);
    *kind = SWITCH_ICON_CACHE_ENTRY_MISSING;
    if (host->foreignPaths.count(path)) {
        *kind = SWITCH_ICON_CACHE_ENTRY_FOREIGN;
        return 0;
    }
    struct stat st{};
    if (::lstat((host->root / path).c_str(), &st) != 0)
        return errno == ENOENT ? 0 : errno;
    if (S_ISLNK(st.st_mode))
        *kind = SWITCH_ICON_CACHE_ENTRY_SYMLINK;
    else if (S_ISREG(st.st_mode))
        *kind = SWITCH_ICON_CACHE_ENTRY_REG;
    else if (S_ISDIR(st.st_mode))
        *kind = SWITCH_ICON_CACHE_ENTRY_DIR;
    else
        *kind = SWITCH_ICON_CACHE_ENTRY_OTHER;
    return 0;
}

int SweepUnlink(void *context, const char *path)
{
    auto *host = static_cast<SweepHost *>(context);
    if (host->foreignPaths.count(path))
        return EXDEV;
    if (::unlink((host->root / path).c_str()) != 0)
        return errno;
    host->opsLog.push_back(std::string("unlink:") + path);
    return 0;
}

int SweepRmdir(void *context, const char *path)
{
    auto *host = static_cast<SweepHost *>(context);
    if (host->foreignPaths.count(path))
        return EXDEV;
    if (::rmdir((host->root / path).c_str()) != 0)
        return errno;
    host->opsLog.push_back(std::string("rmdir:") + path);
    return 0;
}

switch_icon_cache_sweep_result RunSweep(SweepHost &host, size_t stride = 256)
{
    std::vector<char> scratch((SWITCH_ICON_CACHE_SWEEP_MAX_DEPTH + 1) * stride, 0);
    const switch_icon_cache_sweep_ops ops = {
        &host, scratch.data(), stride, SweepReadBatch, SweepInspect, SweepUnlink, SweepRmdir,
    };
    switch_icon_cache_sweep_result result;
    switch_icon_cache_sweep(&ops, &result);
    return result;
}

TEST_F(SwitchIconCache, SweepDeletesLeavesBeforeDirsAndRemovesTrashDirLast)
{
    Put(data.root / kTrash / "s1/store.index", "idx");
    Put(data.root / kTrash / "s1/__cache_info__", "info");
    Put(data.root / kTrash / "s1/__METAL_CACHE__/a.bin", "a");
    Put(data.root / kTrash / "s1/__METAL_CACHE__/b.bin", "b");
    Put(data.root / kTrash / "s2/d2/d3/d4/deep.bin", "depth4-leaf");

    SweepHost host{};
    host.root = data.root;
    host.maxBatchEntries = 2;  // exercises multi-batch readdir loops
    const switch_icon_cache_sweep_result result = RunSweep(host);
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(result.files_removed, 5u);
    EXPECT_EQ(result.dirs_removed, 7u);  // trash, s1, __METAL_CACHE__, s2, d2, d3, d4
    EXPECT_FALSE(fs::exists(data.root / kTrash));
    ASSERT_FALSE(host.opsLog.empty());
    EXPECT_EQ(host.opsLog.back(), std::string("rmdir:") + SWITCH_ICON_CACHE_TRASH);

    for (size_t i = 0; i < host.opsLog.size(); ++i) {
        if (host.opsLog[i].rfind("rmdir:", 0) != 0)
            continue;
        const std::string dir = host.opsLog[i].substr(6) + "/";
        for (size_t j = i + 1; j < host.opsLog.size(); ++j) {
            const size_t colon = host.opsLog[j].find(':');
            const std::string later = host.opsLog[j].substr(colon + 1);
            EXPECT_NE(later.rfind(dir, 0), 0u) << host.opsLog[j] << " ran after " << host.opsLog[i];
        }
    }

    const switch_icon_cache_sweep_result second = RunSweep(host);
    EXPECT_EQ(second.error, 0);
    EXPECT_EQ(second.files_removed, 0u);
    EXPECT_EQ(second.dirs_removed, 0u);
}

TEST_F(SwitchIconCache, SweepRefusesTraversalPastMaxDepth4)
{
    Put(data.root / kTrash / "d1/d2/d3/d4/d5/too-deep.bin", "keep");
    SweepHost host{};
    host.root = data.root;
    const switch_icon_cache_sweep_result result = RunSweep(host);
    EXPECT_EQ(result.error, ELOOP);
    EXPECT_TRUE(fs::exists(data.root / kTrash / "d1/d2/d3/d4/d5/too-deep.bin"));
    EXPECT_EQ(std::count(host.openedDirs.begin(), host.openedDirs.end(),
                         std::string(SWITCH_ICON_CACHE_TRASH) + "/d1/d2/d3/d4/d5"),
              0);
}

TEST_F(SwitchIconCache, SweepRefusesSymlinksWithoutDeletingTargets)
{
    Put(data.root / "outside/keep.txt", "precious");
    fs::create_directories(data.root / kTrash / "s1");
    ASSERT_EQ(::symlink((data.root / "outside").c_str(), (data.root / kTrash / "s1/escape").c_str()), 0);

    SweepHost host{};
    host.root = data.root;
    const switch_icon_cache_sweep_result result = RunSweep(host);
    EXPECT_EQ(result.error, ELOOP);
    EXPECT_TRUE(fs::exists(data.root / "outside/keep.txt"));
    EXPECT_TRUE(fs::is_symlink(data.root / kTrash / "s1/escape"));
}

TEST_F(SwitchIconCache, SweepRefusesForeignMounts)
{
    Put(data.root / kTrash / "s1/mounted/keep.txt", "foreign");
    SweepHost host{};
    host.root = data.root;
    host.foreignPaths.insert(std::string(SWITCH_ICON_CACHE_TRASH) + "/s1/mounted");
    const switch_icon_cache_sweep_result result = RunSweep(host);
    EXPECT_EQ(result.error, EXDEV);
    EXPECT_TRUE(fs::exists(data.root / kTrash / "s1/mounted/keep.txt"));
    EXPECT_EQ(std::count(host.openedDirs.begin(), host.openedDirs.end(),
                         std::string(SWITCH_ICON_CACHE_TRASH) + "/s1/mounted"),
              0);
}

TEST(SwitchIconCacheState, LaunchesOnceOnlyWhenTrashPendingAndBootCompleted)
{
    uint32_t state = 0, next = 0;

    // Clean boot: boot complete without pending trash does zero work.
    EXPECT_FALSE(switch_icon_cache_state_step(state, SWITCH_ICON_CACHE_STATE_BOOT_DONE, &next));
    EXPECT_EQ(next, SWITCH_ICON_CACHE_STATE_BOOT_DONE);

    // Deploy boot: arm(pending) before boot complete starts once on boot complete, never again.
    state = 0;
    EXPECT_FALSE(switch_icon_cache_state_step(state, SWITCH_ICON_CACHE_STATE_PENDING, &next));
    state = next;
    EXPECT_TRUE(switch_icon_cache_state_step(state, SWITCH_ICON_CACHE_STATE_BOOT_DONE, &next));
    state = next;
    EXPECT_FALSE(switch_icon_cache_state_step(state, SWITCH_ICON_CACHE_STATE_BOOT_DONE, &next));
    EXPECT_EQ(next, state);
    EXPECT_FALSE(switch_icon_cache_state_step(state, SWITCH_ICON_CACHE_STATE_PENDING, &next));
    EXPECT_EQ(next, state);

    // Reverse order: boot complete before arm(pending) still starts once when armed.
    state = 0;
    EXPECT_FALSE(switch_icon_cache_state_step(state, SWITCH_ICON_CACHE_STATE_BOOT_DONE, &next));
    state = next;
    EXPECT_TRUE(switch_icon_cache_state_step(state, SWITCH_ICON_CACHE_STATE_PENDING, &next));
    state = next;
    EXPECT_FALSE(switch_icon_cache_state_step(state, SWITCH_ICON_CACHE_STATE_BOOT_DONE, &next));
}
#else
TEST_F(SwitchIconCache, NewBuildDropsOnlyCacheInfoAndWritesTheMarker)
{
    std::vector<fs::path> expected = Present();
    expected.erase(std::find(expected.begin(), expected.end(), kCache / "__cache_info__"));
    Put(data.root / "Switch/icon-cache-gpu", kBuildB);
    Put(data.root / "Switch/icon-cache-reset", kBuildA);
    const switch_icon_cache_result result = Drop();
    EXPECT_TRUE(result.requested);
    EXPECT_EQ(result.error, 0);
    EXPECT_EQ(result.dropped, 1);
    EXPECT_FALSE(result.trash_pending);
    EXPECT_STREQ(result.build, "bbbbbbbbbbbb");
    EXPECT_EQ(Present(), expected) << "only __cache_info__ goes: the folders and the stores stay";
    EXPECT_EQ(Contents(data.root / "Switch/icon-cache-reset"), kBuildB);
    data.unlinked.clear();
    EXPECT_FALSE(Drop().requested);
    EXPECT_TRUE(data.unlinked.empty()) << "the same build again drops nothing";
}

TEST_F(SwitchIconCache, Xnu10063DropsTheTopOnly)
{
    Put(data.root / "Switch/icon-cache-gpu", kBuildB);
    EXPECT_EQ(Drop().dropped, 1);
    EXPECT_FALSE(fs::exists(data.root / kCache / "__cache_info__"));
    for (const char *salt : kSalts)
        EXPECT_TRUE(fs::exists(data.root / kCache / salt / "__cache_info__")) << "21H461 has no salt folders";
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
#endif
}  // namespace
