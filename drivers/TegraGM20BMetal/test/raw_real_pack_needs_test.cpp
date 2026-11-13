// SPDX-License-Identifier: MIT
// Every Core Animation and other framework render state of the corpus, and Core Animation's compute states, made on the
// raw facade over the kernel model with a device build's pack (SWITCH_REAL_PACK, of the OS build SWITCH_PACK_BUILD) and
// the IPSW crawl's libraries (SWITCH_LIB_ROOT, the crawl's root/): which states open m2v and for what, by library.
// Skipped where the pack or the libraries are not given.
#include "raw_facade_harness.h"
#include "../raw/pack/probe_states.h"

#include <gtest/gtest.h>
#include <fstream>
#include <map>
#include <string>

namespace {

using Tally = std::map<std::string, std::map<std::string, unsigned>>;

std::map<std::string, uint64_t> needsOf(id<MTLDevice> device)
{
    std::map<std::string, uint64_t> out;
    NSDictionary *needs = harness::m2vRecord(device)[@"needs"];
    for (NSString *need in needs)
        out[need.UTF8String] = [needs[need] unsignedLongLongValue];
    return out;
}

// Each need the creation asked m2v for, by its library ("none": the state is the raw path's alone)
void tallyState(id<MTLDevice> device, id<MTLLibrary> library, const std::string &name, const std::string &body,
                const std::string &framework, Tally &tally, std::ofstream &rows)
{
    probe::State s;
    if (!library || !probe::parseState(library, body, s, framework)) {
        tally[name][library ? "line not parsed" : "library not read"]++;
        return;
    }
    const auto before = needsOf(device);
    @autoreleasepool {
        NSError *error = nil;
        (void)[device newRenderPipelineStateWithDescriptor:s.d error:&error];
    }
    const auto after = needsOf(device);
    std::string asked;
    for (const auto &[need, count] : after)
        if (count > (before.count(need) ? before.at(need) : 0)) {
            tally[name][need]++;
            asked += (asked.empty() ? "" : "; ") + need;
        }
    if (asked.empty())
        tally[name]["none"]++;
    rows << name << "\t" << (asked.empty() ? "none" : asked) << "\t" << s.label << "\n";
}

void corpusNeeds()
{
    const char *pack = getenv("SWITCH_REAL_PACK"), *root = getenv("SWITCH_LIB_ROOT");
    setenv("PROBE_LIB_ROOT", root, 1);
    id<MTLDevice> device = harness::open(pack);
    ASSERT_TRUE(device != nil);
    NSString *text = [NSString stringWithContentsOfFile:@"drivers/TegraGM20BMetal/precompile/corpus.log"
                                               encoding:NSUTF8StringEncoding
                                                  error:nil];
    ASSERT_TRUE(text != nil);
    const std::string qcPath = std::string(root) + "/System/Library/Frameworks/QuartzCore.framework/default.metallib";
    NSError *error = nil;
    id<MTLLibrary> qc = [device newLibraryWithURL:[NSURL fileURLWithPath:@(qcPath.c_str())] error:&error];
    Tally tally;
    const char *out = getenv("TEST_UNDECLARED_OUTPUTS_DIR");
    std::ofstream rows(std::string(out ? out : harness::dir().c_str()) + "/states.tsv");
    // a line tagged with another OS build than the pack's (SWITCH_PACK_BUILD) is that build's pack's state
    const char *build = getenv("SWITCH_PACK_BUILD");
    const std::string ours = std::string(" os=") + (build ? build : "") + " ";
    for (const std::string &line : probe::corpusLines(text, false)) {
        if (build && line.find(" os=") != std::string::npos && line.find(ours) == std::string::npos)
            continue;
        if (line.rfind("@L ", 0)) {
            tallyState(device, qc, "QuartzCore", line, "", tally, rows);
            continue;
        }
        const size_t space = line.find(' ', 3);
        const std::string path = line.substr(3, space - 3);
        const std::string framework = probe::frameworkName(path);
        tallyState(device, probe::frameworkLibrary(device, path), framework.empty() ? path : framework,
                   line.substr(space + 1), framework, tally, rows);
    }
    // Core Animation's compute states (the corpus's C lines of QuartzCore's functions, no constants), as it makes them
    for (NSString *l in [text componentsSeparatedByString:@"\n"]) {
        if (![l hasPrefix:@"C ok=1 "] || ![l containsString:@"/QuartzCore.framework/"] || [l containsString:@" #"])
            continue;
        const std::string line = l.UTF8String;
        if (build && line.find(" os=") != std::string::npos && line.find(ours) == std::string::npos)
            continue;
        const size_t at = line.find(" | k ") + 5;
        const std::string function = line.substr(at, line.find(' ', at) - at);
        MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
        id<MTLFunction> kernel = [qc newFunctionWithName:@(function.c_str())];
        if (!kernel) {
            tally["QuartzCore compute"]["no such function in this build's library"]++;
            continue;
        }
        d.computeFunction = kernel;
        if (line.find(" label=") != std::string::npos)
            d.label = @(function.c_str());
        const auto before = needsOf(device);
        @autoreleasepool {
            NSError *made = nil;
            if (line.find(" form=f ") != std::string::npos)
                (void)[device newComputePipelineStateWithFunction:d.computeFunction error:&made];
            else
                (void)[device newComputePipelineStateWithDescriptor:d
                                                            options:MTLPipelineOptionNone
                                                         reflection:nil
                                                              error:&made];
        }
        std::string asked;
        for (const auto &[need, count] : needsOf(device))
            if (count > (before.count(need) ? before.at(need) : 0))
                asked += (asked.empty() ? "" : "; ") + need;
        tally["QuartzCore compute"][asked.empty() ? "none" : asked]++;
        rows << "QuartzCore compute\t" << (asked.empty() ? "none" : asked) << "\t" << function << "\n";
    }
    for (const auto &[library, needs] : tally)
        for (const auto &[need, states] : needs)
            printf("%s\t%u\t%s\n", library.c_str(), states, need.c_str());
    EXPECT_FALSE(tally.empty());
}

}  // namespace

TEST(RealPack, CorpusStatesNameWhatOpensM2V)
{
    if (!getenv("SWITCH_REAL_PACK") || !getenv("SWITCH_LIB_ROOT"))
        GTEST_SKIP() << "no SWITCH_REAL_PACK and SWITCH_LIB_ROOT";
    corpusNeeds();
}
