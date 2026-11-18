// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "drivers/SwitchBootstrap/payload.h"

#include <filesystem>
#include <fstream>
#include <cassert>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" {
#include "drivers/SwitchBootstrap/archive.h"
struct archive *archive_write_new(void);
int archive_write_set_format_pax_restricted(struct archive *);
int archive_write_open_filename(struct archive *, const char *);
int archive_write_header(struct archive *, struct archive_entry *);
ssize_t archive_write_data(struct archive *, const void *, size_t);
int archive_write_close(struct archive *);
struct archive_entry *archive_entry_new(void);
void archive_entry_set_pathname(struct archive_entry *, const char *);
void archive_entry_set_uid(struct archive_entry *, int64_t);
void archive_entry_set_gid(struct archive_entry *, int64_t);
void archive_entry_set_mode(struct archive_entry *, mode_t);
void archive_entry_set_mtime(struct archive_entry *, time_t, long);
void archive_entry_set_size(struct archive_entry *, int64_t);
void archive_entry_set_hardlink(struct archive_entry *, const char *);
void archive_entry_set_symlink(struct archive_entry *, const char *);
void archive_entry_free(struct archive_entry *);
}

struct SwitchArchiveFile {
    std::string path;
    std::string contents;
    mode_t mode = S_IFREG | 0644;
    std::string link;
    bool hard = false;

    SwitchArchiveFile(std::string path, std::string contents, mode_t mode = S_IFREG | 0644, std::string link = {},
                      bool hard = false)
        : path(std::move(path)), contents(std::move(contents)), mode(mode), link(std::move(link)), hard(hard)
    {}
};

class SwitchBootstrapArchive {
public:
    SwitchBootstrapArchive()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "switch-archive-XXXXXX").string();
        const char *directory = mkdtemp(pattern.data());
        assert(directory != nullptr);
        char canonical[PATH_MAX];
        assert(realpath(directory, canonical) != nullptr);
        root = canonical;
        destination = root / "destination";
        std::filesystem::create_directory(destination);
        archive = root / "payload.tar";
    }

    ~SwitchBootstrapArchive()
    {
        std::filesystem::remove_all(root);
    }

    void write(const std::vector<SwitchArchiveFile> &files)
    {
        struct archive *output = archive_write_new();
        assert(output != nullptr);
        assert(archive_write_set_format_pax_restricted(output) == 0);
        assert(archive_write_open_filename(output, archive.c_str()) == 0);
        for (const auto &file : files) {
            struct archive_entry *entry = archive_entry_new();
            assert(entry != nullptr);
            archive_entry_set_pathname(entry, file.path.c_str());
            archive_entry_set_uid(entry, getuid());
            archive_entry_set_gid(entry, getgid());
            archive_entry_set_mode(entry, file.mode);
            archive_entry_set_mtime(entry, 123456789, 0);
            archive_entry_set_size(entry, static_cast<int64_t>(file.contents.size()));
            if (!file.link.empty()) {
                if (file.hard)
                    archive_entry_set_hardlink(entry, file.link.c_str());
                else
                    archive_entry_set_symlink(entry, file.link.c_str());
            }
            assert(archive_write_header(output, entry) == 0);
            if (!file.contents.empty())
                assert(archive_write_data(output, file.contents.data(), file.contents.size()) ==
                       static_cast<ssize_t>(file.contents.size()));
            archive_entry_free(entry);
        }
        assert(archive_write_close(output) == 0);
        assert(archive_write_free(output) == 0);
    }

    int extract() const
    {
        return switch_bootstrap_extract(archive.c_str(), destination.c_str());
    }

    std::filesystem::path root;
    std::filesystem::path destination;
    std::filesystem::path archive;
};
