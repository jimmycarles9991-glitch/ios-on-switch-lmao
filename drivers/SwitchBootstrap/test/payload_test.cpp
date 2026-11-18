// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "archive_fixture.h"

static constexpr const char *switch_bootstrap_test_sileo = "usr/share/Switch/bootstrap-packages/sileo.deb";
static constexpr const char *switch_bootstrap_test_ellekit = "usr/share/Switch/bootstrap-packages/ellekit.deb";
static constexpr const char *switch_bootstrap_test_uikittools = "usr/share/Switch/bootstrap-packages/uikittools.deb";
static constexpr const char *switch_bootstrap_test_launchctl = "usr/share/Switch/bootstrap-packages/launchctl.deb";

int main()
{
    {
        SwitchBootstrapArchive fixture;
        fixture.write({{"bin", "", S_IFDIR | 0755},
                       {"bin/tool", "payload", S_IFREG | 0751},
                       {"bin/alias", "", S_IFLNK | 0777, "tool"},
                       {"bin/hard", "", S_IFREG | 0751, "bin/tool", true}});
        const auto before = std::filesystem::current_path();
        assert(fixture.extract() == 0);
        assert(std::filesystem::current_path() == before);
        struct stat tool, hard;
        assert(stat((fixture.destination / "bin/tool").c_str(), &tool) == 0);
        assert(stat((fixture.destination / "bin/hard").c_str(), &hard) == 0);
        assert((tool.st_mode & 07777) == 0751);
        assert(tool.st_uid == getuid() && tool.st_gid == getgid());
        assert(tool.st_mtime == 123456789 && tool.st_ino == hard.st_ino);
        assert(std::filesystem::read_symlink(fixture.destination / "bin/alias") == "tool");
        std::ifstream input(fixture.destination / "bin/tool");
        std::string contents;
        input >> contents;
        assert(contents == "payload");
    }
    {
        SwitchBootstrapArchive fixture;
        for (const auto &path : {"../escaped", "/tmp/switch-archive-escaped"}) {
            fixture.write({{path, "bad"}});
            assert(fixture.extract() != 0);
            assert(!std::filesystem::exists(fixture.root / "escaped"));
        }
        const auto outside = fixture.root / "outside";
        std::filesystem::create_directory(outside);
        fixture.write({{"escape", "", S_IFLNK | 0777, outside.string()}, {"escape/file", "bad"}});
        assert(fixture.extract() != 0);
        assert(!std::filesystem::exists(outside / "file"));
    }
    {
        SwitchBootstrapArchive fixture;
        fixture.write({{"file", "", S_IFREG | 0644, "../outside", true}});
        assert(fixture.extract() != 0);
        assert(!std::filesystem::exists(fixture.destination / "file"));
    }
    {
        SwitchBootstrapArchive fixture;
        fixture.write({{"file", std::string(4096, 'x')}});
        std::filesystem::resize_file(fixture.archive, 700);
        const auto before = std::filesystem::current_path();
        assert(fixture.extract() != 0);
        assert(std::filesystem::current_path() == before);
        assert(switch_bootstrap_extract(fixture.archive.c_str(), (fixture.root / "missing").c_str()) != 0);
        std::ofstream(fixture.archive, std::ios::trunc) << "invalid tar";
        assert(fixture.extract() != 0);
    }
    {
        SwitchBootstrapArchive fixture;
        const auto outside = fixture.root / "outside";
        std::filesystem::create_directory(outside);
        fixture.write({{"../escaped", "skip"},
                       {"usr/share/Switch/bootstrap-packages", "", S_IFLNK | 0777, outside.string()},
                       {"other", std::string(4096, 'x')},
                       {switch_bootstrap_test_sileo, "sileo"},
                       {switch_bootstrap_test_ellekit, "ellekit"},
                       {switch_bootstrap_test_uikittools, "uikittools"},
                       {switch_bootstrap_test_launchctl, "launchctl"},
                       {std::string(switch_bootstrap_test_sileo) + ".partial", "skip"}});
        const auto before = std::filesystem::current_path();
        assert(switch_bootstrap_extract_core(fixture.archive.c_str(), fixture.destination.c_str()) == 0);
        assert(std::filesystem::current_path() == before);
        assert(!std::filesystem::exists(fixture.root / "escaped"));
        assert(std::filesystem::is_empty(outside));
        assert(!std::filesystem::exists(fixture.destination / "other"));
        assert(!std::filesystem::exists(fixture.destination / (std::string(switch_bootstrap_test_sileo) + ".partial")));
        for (const char *path : {switch_bootstrap_test_sileo, switch_bootstrap_test_ellekit,
                                 switch_bootstrap_test_uikittools, switch_bootstrap_test_launchctl}) {
            struct stat info;
            const auto file = fixture.destination / path;
            assert(lstat(file.c_str(), &info) == 0 && S_ISREG(info.st_mode));
            assert(info.st_nlink == 1 && info.st_uid == getuid() && info.st_gid == getgid());
            assert((info.st_mode & 07777) == 0644 && info.st_size > 0);
        }
    }
    for (const auto &sileo : std::vector<std::vector<SwitchArchiveFile>>{
             {},
             {{switch_bootstrap_test_sileo, "first"}, {switch_bootstrap_test_sileo, "duplicate"}},
             {{switch_bootstrap_test_sileo, "", S_IFLNK | 0777, "outside"}},
             {{switch_bootstrap_test_sileo, "", S_IFREG | 0644, switch_bootstrap_test_ellekit, true}},
             {{switch_bootstrap_test_sileo, "writable", S_IFREG | 0666}},
         }) {
        SwitchBootstrapArchive fixture;
        std::vector<SwitchArchiveFile> files = {{switch_bootstrap_test_ellekit, "ellekit"},
                                                {switch_bootstrap_test_uikittools, "uikittools"},
                                                {switch_bootstrap_test_launchctl, "launchctl"}};
        files.insert(files.end(), sileo.begin(), sileo.end());
        fixture.write(files);
        const auto before = std::filesystem::current_path();
        assert(switch_bootstrap_extract_core(fixture.archive.c_str(), fixture.destination.c_str()) != 0);
        assert(std::filesystem::current_path() == before);
    }
}
