// Every kernel source of the tree that registers a sysctl at run time builds its oid with switch_sysctl_oid, and only
// the kit sets oid_kind or oid_version: an oid without CTLFLAG_OID2 panics arm64 XNU at registration, at boot.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
std::vector<std::filesystem::path> sources()
{
    std::vector<std::filesystem::path> found;
    for (const char *root : {"drivers", "kits", "diagnostics", "kernel/switch"}) {
        if (!std::filesystem::exists(root))
            continue;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(
                 root, std::filesystem::directory_options::follow_directory_symlink)) {
            const std::string ext = entry.path().extension().string();
            if (entry.is_regular_file() && (ext == ".c" || ext == ".cpp" || ext == ".h" || ext == ".mm"))
                found.push_back(entry.path());
        }
    }
    return found;
}

std::string text(const std::filesystem::path &path)
{
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// `field` followed by an assignment (not a comparison): "oid_kind = ", "oid_kind |= ".
bool assignsField(const std::string &source, const std::string &field)
{
    for (size_t at = source.find(field); at != std::string::npos; at = source.find(field, at + 1)) {
        size_t next = at + field.size();
        while (next < source.size() && source[next] == ' ')
            ++next;
        if (next + 1 < source.size() && source[next + 1] == '=' && (source[next] == '|' || source[next] == '&'))
            return true;
        if (next < source.size() && source[next] == '=' && (next + 1 >= source.size() || source[next + 1] != '='))
            return true;
    }
    return false;
}

TEST(SwitchSysctlSource, HandBuiltOidsGoThroughTheKit)
{
    const std::vector<std::filesystem::path> files = sources();
    ASSERT_GT(files.size(), 100u);
    unsigned registering = 0;
    for (const auto &path : files) {
        const std::string source = text(path);
        if (path.generic_string().find("kits/SwitchSysctl/") != std::string::npos)
            continue;
        EXPECT_FALSE(assignsField(source, "oid_kind") || assignsField(source, "oid_version"))
            << path << " sets oid_kind or oid_version itself";
        if (source.find("sysctl_register_oid(") != std::string::npos) {
            ++registering;
            EXPECT_NE(source.find("switch_sysctl_oid("), std::string::npos) << path << " registers an oid by hand";
        }
    }
    EXPECT_GT(registering, 0u);
}
}  // namespace
