// SPDX-License-Identifier: MIT
// A draw's root table as the raw device writes it (raw_heap_tables.h writeDrawRoots, RenderEncoder::rootTable), for
// argcheck.py's interpreter runs: one line of four hex words (host, guest, size, bind offset) per entry.
//   rootdump <table va> <slots> <sink va> <identity va> [<slot n>=<va>:<bytes>:<bind offset> ...]
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../runtime/raw_heap_tables.h"

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: rootdump <table va> <slots> <sink va> <identity va> [n=va:bytes:offset ...]\n");
        return 2;
    }
    const uint64_t va = strtoull(argv[1], nullptr, 0), sink = strtoull(argv[3], nullptr, 0),
                   identity = strtoull(argv[4], nullptr, 0);
    const uint32_t slots = uint32_t(strtoul(argv[2], nullptr, 0));
    if (!slots)
        return 2;
    raw::RootSlot slot[32];
    for (int i = 5; i < argc; i++) {
        unsigned n;
        uint64_t at, bytes, offset;
        if (sscanf(argv[i], "%u=%" SCNi64 ":%" SCNi64 ":%" SCNi64, &n, &at, &bytes, &offset) != 4 || n >= 32)
            return 2;
        slot[n] = {at, bytes, offset};
    }
    std::vector<raw::RootEntry> table(raw::drawRootEntries(slots));
    raw::writeDrawRoots(table.data(), va, slots, slot, sink, identity);
    for (const raw::RootEntry &e : table)
        printf("%" PRIx64 " %" PRIx64 " %" PRIx64 " %" PRIx64 "\n", e.host, e.guest, e.size, e.bindOffset);
    return 0;
}
