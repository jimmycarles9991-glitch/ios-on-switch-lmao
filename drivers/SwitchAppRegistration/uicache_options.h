// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <cerrno>
#include <cstdlib>
#include <limits.h>
#include <string>
#include <vector>

struct SwitchUICacheOptions {
    bool all = false;
    bool list = false;
    bool respring = false;
    bool help = false;
    std::vector<std::string> paths;
    std::vector<std::string> unregister;
    std::string error;
};

static inline SwitchUICacheOptions switch_uicache_parse(int argc, const char *const *argv)
{
    SwitchUICacheOptions options;
    std::vector<std::string> *paths = nullptr;
    bool needs_path = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (!argument.empty() && argument.front() != '-' && paths) {
            paths->push_back(argument);
            needs_path = false;
            continue;
        }
        if (needs_path) {
            options.error = "missing application path";
            return options;
        }
        paths = nullptr;
        if (argument == "-a" || argument == "--all")
            options.all = true;
        else if (argument == "-l" || argument == "--list")
            options.list = true;
        else if (argument == "-r" || argument == "--respring")
            options.respring = true;
        else if (argument == "-f" || argument == "--force" || argument == "-v" || argument == "--verbose")
            continue;
        else if (argument == "-h" || argument == "--help")
            options.help = true;
        else if (argument == "-p" || argument == "--path" || argument == "-u" || argument == "--unregister") {
            paths = argument == "-p" || argument == "--path" ? &options.paths : &options.unregister;
            needs_path = true;
        } else {
            options.error = "unsupported argument: " + argument;
            return options;
        }
    }
    if (needs_path)
        options.error = "missing application path";
    else if (!options.all && !options.list && !options.respring && !options.help && options.paths.empty() &&
             options.unregister.empty())
        options.error = "an operation is required";
    return options;
}

static inline std::string switch_uicache_app_path(const std::string &root, std::string path)
{
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    for (const std::string &prefix : {root + "/Applications/", std::string("/var/jb/Applications/"),
                                     std::string("/private/var/jb/Applications/")}) {
        if (path.compare(0, prefix.size(), prefix) != 0)
            continue;
        const std::string name = path.substr(prefix.size());
        if (name.size() > 4 && name.size() < 256 && name.find('/') == std::string::npos &&
            name.find('\0') == std::string::npos && name.substr(name.size() - 4) == ".app")
            return root + "/Applications/" + name;
    }
    return "";
}
