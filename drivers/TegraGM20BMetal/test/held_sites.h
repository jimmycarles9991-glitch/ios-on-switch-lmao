// SPDX-License-Identifier: MIT
// The blocks a window of a test left allocated (held_sites.c's log of it), by where each was allocated: the frames from
// the first outside the allocator, named by dladdr (a test binary keeps its local symbols), and whose code holds it. A
// block counts as the runtime's when the first frame of this binary on its stack is the runtime's; the test's, the
// harness's and the kernel model's frames (the kernel's on a device) are told apart by their names.
#pragma once
#include "held_sites_log.h"
#include <cxxabi.h>
#include <dlfcn.h>
#include <malloc/malloc.h>
#include <mach-o/dyld.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace held_sites {

enum class Owner {
    Runtime,
    Model,
    Test,
    Apple
};

inline const char *ownerName(Owner o)
{
    switch (o) {
    case Owner::Runtime:
        return "runtime";
    case Owner::Model:
        return "model";
    case Owner::Test:
        return "test";
    case Owner::Apple:
        return "apple";
    }
    return "?";
}

inline double blocksInUse()
{
    malloc_statistics_t stats = {};
    malloc_zone_statistics(nullptr, &stats);
    return double(stats.blocks_in_use);
}

struct Frame {
    std::string image, symbol, mangled;
    uintptr_t offset = 0;
    bool main = false;
};

inline Frame frame(uintptr_t pc)
{
    Frame f;
    Dl_info info = {};
    if (!dladdr(reinterpret_cast<void *>(pc - 1), &info))
        return f;
    const char *slash = info.dli_fname ? strrchr(info.dli_fname, '/') : nullptr;
    f.image = slash ? slash + 1 : info.dli_fname ? info.dli_fname : "?";
    f.main = info.dli_fbase == reinterpret_cast<const void *>(_dyld_get_image_header(0));
    if (info.dli_sname) {
        f.mangled = info.dli_sname;
        int status = 0;
        char *demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
        f.symbol = status == 0 && demangled ? demangled : info.dli_sname;
        free(demangled);
        f.offset = pc - uintptr_t(info.dli_saddr);
    } else {
        f.offset = pc - uintptr_t(info.dli_fbase);
    }
    return f;
}

inline bool startsWith(const std::string &s, const char *prefix)
{
    return s.compare(0, strlen(prefix), prefix) == 0;
}

// libmalloc, operator new, the Objective-C runtime's and Swift's object allocation: not a site
inline bool allocator(const Frame &f)
{
    if (f.image == "libsystem_malloc.dylib" || f.image == "libheld_sites.dylib" || startsWith(f.symbol, "operator new"))
        return true;
    for (const char *alloc :
         {"_objc_rootAlloc", "objc_alloc", "class_createInstance", "objc_opt_new", "+[NSObject alloc", "+[NSObject new",
          "_class_createInstance", "swift_slowAlloc", "swift_allocObject"})
        if (startsWith(f.symbol, alloc))
            return true;
    return false;
}

// Code of this binary that is not the runtime's or the facade's, by its frame's name
inline Owner mainOwner(const Frame &f)
{
    for (const char *model : {"kernel_model::", "timed_gpu::", "model_gpu::", "IOConnect", "IOService", "IORegistry"})
        if (startsWith(f.symbol, model))
            return Owner::Model;
    for (const char *test :
         {"held_blocks_test::", "encode_cost::", "harness::", "scale_frames::", "held_sites::", "testing::", "main",
          "GM20BFacadeHeldBlocks", "-[Harness", "+[Harness", "facade::", "-[EncodeCostGone"})
        if (startsWith(f.symbol, test))
            return Owner::Test;
    return Owner::Runtime;
}

// Standard library code instantiated into this binary (std::__1's, whatever its return type, and the lambdas inside it
// that libc++ 22's map insert and vector growth run, _ZZ local entities): whoever called it decides
inline bool library(const Frame &f)
{
    for (const char *std : {"_ZNSt3__1", "_ZNKSt3__1", "_ZSt", "_ZZNSt3__1", "_ZZNKSt3__1", "__ZNSt3__1", "__ZNKSt3__1",
                            "__ZZNSt3__1", "__ZZNKSt3__1"})
        if (startsWith(f.mangled, std))
            return true;
    return false;
}

struct Site {
    std::vector<uintptr_t> frames;  // from the first outside the allocator, innermost first
    Owner owner = Owner::Apple;     // as its first block's frames decide it
    uintptr_t ownerFrame = 0;       // the frame that decided it (0: none of this binary's)
    long blocks = 0;
    long otherOwner = 0;  // blocks whose own frames decide another owner
    uint64_t bytes = 0;
    std::map<std::string, long> threads;
};

struct Window {
    long live = 0;          // blocks handed out in the window and not given back by its end
    long earlierFreed = 0;  // blocks from before the window given back in it
    long allocations = 0;
    bool overflow = false;
    std::vector<Site> sites;  // most blocks first

    long held(Owner o) const
    {
        long n = 0;
        for (const Site &s : sites)
            n += s.owner == o ? s.blocks : 0;
        return n;
    }
};

inline std::string threadName(const held_sites_event &e)
{
    if (e.encoding)
        return "encoding thread";
    const size_t n = strnlen(e.thread, sizeof e.thread);
    return n ? std::string(e.thread, n) : "unnamed thread";
}

// The log's live blocks (`all`: every allocation) grouped by their first `depth` frames past the allocator
inline Window collect(const held_sites_event *events, size_t count, bool overflow, size_t depth, bool all = false)
{
    Window w;
    w.overflow = overflow;
    std::unordered_map<uintptr_t, size_t> live;
    for (size_t i = 0; i < count; i++) {
        const held_sites_event &e = events[i];
        if (!__atomic_load_n(&e.ready, __ATOMIC_ACQUIRE))
            continue;
        if (e.freed) {
            const auto it = live.find(e.freed);
            if (it != live.end())
                live.erase(it);
            else
                w.earlierFreed++;
        }
        if (e.allocated) {
            live[e.allocated] = i;
            w.allocations++;
        }
    }
    w.live = long(live.size());
    std::vector<std::pair<uintptr_t, size_t>> counted(live.begin(), live.end());
    if (all) {
        counted.clear();
        for (size_t i = 0; i < count; i++)
            if (__atomic_load_n(&events[i].ready, __ATOMIC_ACQUIRE) && events[i].allocated)
                counted.emplace_back(events[i].allocated, i);
    }
    std::map<std::vector<uintptr_t>, Site> bySite;
    std::unordered_map<uintptr_t, Frame> named;
    auto name = [&](uintptr_t pc) -> const Frame & {
        auto it = named.find(pc);
        return it != named.end() ? it->second : named.emplace(pc, frame(pc)).first->second;
    };
    for (const auto &[block, at] : counted) {
        (void)block;
        const held_sites_event &e = events[at];
        size_t first = 0;
        while (first < e.frames && allocator(name(e.frame[first])))
            first++;
        std::vector<uintptr_t> key(e.frame + first, e.frame + std::min<size_t>(e.frames, first + depth));
        Owner owner = Owner::Apple;
        uintptr_t ownerFrame = 0;
        for (size_t k = first; k < e.frames; k++) {
            const Frame &f = name(e.frame[k]);
            if (f.main && !library(f)) {
                owner = mainOwner(f);
                ownerFrame = e.frame[k];
                break;
            }
        }
        Site &s = bySite[key];
        if (!s.blocks) {
            s.frames = key;
            s.owner = owner;
            s.ownerFrame = ownerFrame;
        }
        s.blocks++;
        s.otherOwner += owner != s.owner;
        s.bytes += e.size;
        s.threads[threadName(e)]++;
    }
    for (auto &[key, site] : bySite)
        w.sites.push_back(std::move(site));
    std::sort(w.sites.begin(), w.sites.end(), [](const Site &a, const Site &b) {
        return a.blocks > b.blocks;
    });
    return w;
}

// Each site of at least `least` blocks: blocks and bytes per `per`, its owner and its frames by name
inline void print(const Window &w, const char *label, double per, long least)
{
    printf("%s: %ld allocations, %ld held, %ld earlier blocks freed%s\n", label, w.allocations, w.live, w.earlierFreed,
           w.overflow ? " (the log overflowed)" : "");
    for (const Site &s : w.sites) {
        if (s.blocks < least)
            continue;
        printf("  %6.2f blocks %8.1f bytes  %-7s", double(s.blocks) / per, double(s.bytes) / per, ownerName(s.owner));
        for (uintptr_t pc : s.frames) {
            const Frame f = frame(pc);
            printf(" | %s+0x%lx", f.symbol.empty() ? f.image.c_str() : f.symbol.substr(0, 90).c_str(),
                   (unsigned long)f.offset);
        }
        printf("\n");
    }
}

inline std::string shortName(uintptr_t pc)
{
    if (!pc)
        return "no frame of this binary";
    const Frame f = frame(pc);
    std::string name = f.symbol.empty() ? f.image : f.symbol;
    for (size_t at; (at = name.find("(anonymous namespace)::")) != std::string::npos;)
        name.erase(at, strlen("(anonymous namespace)::"));
    const size_t close = name.rfind(')');
    if (close != std::string::npos && name.find("-[") == std::string::npos && name.find("+[") == std::string::npos) {
        int depth = 0;
        for (size_t i = close + 1; i-- > 0;) {
            depth += name[i] == ')' ? 1 : name[i] == '(' ? -1 : 0;
            if (!depth) {
                if (i)
                    name.resize(i);
                break;
            }
        }
    }
    if (name.size() > 90)
        name = name.substr(0, 40) + "..." + name.substr(name.size() - 47);
    char offset[24];
    snprintf(offset, sizeof offset, "+0x%lx", (unsigned long)f.offset);
    return name + offset;
}

struct ArmCounts {
    long n[2] = {};
};

inline std::string countsLine(const char *label, const std::map<std::string, ArmCounts> &counts)
{
    std::string out = label;
    for (const auto &[name, arms] : counts)
        out += " " + name + " " + std::to_string(arms.n[0]) + " vs " + std::to_string(arms.n[1]) + ";";
    return out + "\n";
}

// The sites, matched by frames, whose blocks `owner` takes differ between windows `a` and `b` of `per` `unit`s each,
// most different first: the counts, each window's owner of the site and the frame that decided it, the site's blocks
// by thread in both, the blocks whose own frames decide another owner, and the site's frames (innermost first)
inline std::string differences(const Window &a, const Window &b, Owner owner, const std::string &aName,
                               const std::string &bName, double per, const char *unit, size_t most = 12)
{
    struct Pair {
        const Site *site[2] = {};
        long counted[2] = {};
    };

    std::map<std::vector<uintptr_t>, Pair> bySite;
    std::map<std::string, ArmCounts> byThread;
    const Window *windows[2] = {&a, &b};
    for (int k = 0; k < 2; k++)
        for (const Site &s : windows[k]->sites) {
            Pair &p = bySite[s.frames];
            p.site[k] = &s;
            if (s.owner != owner)
                continue;
            p.counted[k] = s.blocks;
            for (const auto &thread : s.threads)
                byThread[thread.first].n[k] += thread.second;
        }
    std::vector<const Pair *> differing;
    for (const auto &entry : bySite)
        if (entry.second.counted[0] != entry.second.counted[1])
            differing.push_back(&entry.second);
    std::sort(differing.begin(), differing.end(), [](const Pair *x, const Pair *y) {
        return std::labs(x->counted[1] - x->counted[0]) > std::labs(y->counted[1] - y->counted[0]);
    });
    char line[256];
    snprintf(line, sizeof line, "%s allocations, %s vs %s, over %g %ss each%s\n", ownerName(owner), aName.c_str(),
             bName.c_str(), per, unit, a.overflow || b.overflow ? " (the log overflowed)" : "");
    std::string out = line + countsLine("  by thread:", byThread);
    out += "  " + std::to_string(differing.size()) + " sites differ:\n";
    for (size_t i = 0; i < differing.size() && i < most; i++) {
        const Pair &p = *differing[i];
        std::string owners[2] = {"absent", "absent"}, decided[2] = {"absent", "absent"};
        std::map<std::string, ArmCounts> threads;
        long other[2] = {};
        for (int k = 0; k < 2; k++) {
            const Site *s = p.site[k];
            if (!s)
                continue;
            owners[k] = ownerName(s->owner);
            decided[k] = shortName(s->ownerFrame);
            other[k] = s->otherOwner;
            for (const auto &thread : s->threads)
                threads[thread.first].n[k] += thread.second;
        }
        snprintf(line, sizeof line, "    %ld vs %ld (%+.2f per %s), owner %s vs %s, decided by %s", p.counted[0],
                 p.counted[1], double(p.counted[1] - p.counted[0]) / per, unit, owners[0].c_str(), owners[1].c_str(),
                 decided[0].c_str());
        out += line;
        out += decided[0] == decided[1] ? "\n" : " vs " + decided[1] + "\n";
        out += countsLine("      threads:", threads);
        if (other[0] || other[1])
            out += "      blocks another owner's frames decide: " + std::to_string(other[0]) + " vs " +
                   std::to_string(other[1]) + "\n";
        const std::vector<uintptr_t> &frames = (p.site[0] ? p.site[0] : p.site[1])->frames;
        out += "      frames:";
        for (size_t f = 0; f < frames.size(); f++)
            out += (f ? " < " : " ") + shortName(frames[f]);
        out += "\n";
    }
    if (differing.size() > most)
        out += "    (" + std::to_string(differing.size() - most) + " more)\n";
    return out;
}

}  // namespace held_sites
