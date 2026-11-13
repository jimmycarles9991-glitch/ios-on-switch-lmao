// SPDX-License-Identifier: MIT
// seedpack <directory> <name bytes> <identity: 64 hex digits> <list>: writes the seed (SwitchSeedPack.h) of the list's
// entries, one per line: <tier>\t<name: hex>\t<path>.
#include "SwitchSeedPackWriter.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace {

bool unhex(const std::string &text, std::string &out)
{
    const auto digit = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    out.clear();
    for (size_t i = 0; i + 1 < text.size(); i += 2) {
        const int high = digit(text[i]), low = digit(text[i + 1]);
        if (high < 0 || low < 0)
            return false;
        out += char(high << 4 | low);
    }
    return text.size() % 2 == 0;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: seedpack <directory> <name bytes> <identity: 64 hex digits> <list>\n");
        return 2;
    }
    const uint32_t nameBytes = uint32_t(strtoul(argv[2], nullptr, 10));
    std::string identity;
    std::ifstream list(argv[4]);
    if (!unhex(argv[3], identity) || identity.size() != switch_seed_pack::kIdentityBytes || !list) {
        fprintf(stderr, "seedpack: bad identity or unreadable list\n");
        return 2;
    }
    std::vector<switch_seed_pack::Entry> entries;
    for (std::string line; std::getline(list, line);) {
        const size_t tab = line.find('\t'), second = line.find('\t', tab + 1);
        switch_seed_pack::Entry entry;
        if (tab != 1 || second == std::string::npos || (line[0] != '0' && line[0] != '1') ||
            !unhex(line.substr(tab + 1, second - tab - 1), entry.name)) {
            fprintf(stderr, "seedpack: bad line: %s\n", line.c_str());
            return 1;
        }
        entry.tier = uint32_t(line[0] - '0');
        entry.path = line.substr(second + 1);
        entries.push_back(std::move(entry));
    }
    std::string error;
    if (!switch_seed_pack::write(argv[1], nameBytes, reinterpret_cast<const uint8_t *>(identity.data()),
                                 std::move(entries), error)) {
        fprintf(stderr, "seedpack: %s\n", error.c_str());
        return 1;
    }
    return 0;
}
