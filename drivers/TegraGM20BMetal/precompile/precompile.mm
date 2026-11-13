// The shader cache the device's processes write on the Switch, made on this Mac before the boot. Every function specialization
// (S lines), render pipeline state (R lines) and compute pipeline state (C lines) of the device journals (pipelines.log, the
// frontend's, merged by harvest.py) is made again through the shipped frontend + m2v (host build of the same sources: same cache
// build directory) and NVK (host build of the same sources, its LC_UUID set to the iOS dylib's: same pipelineCacheUUID), over the
// GM20B mock (mock_iokit.c, chipset 0x12e as the T210B01 reports), into an empty cache directory: m2v's translations, its creation
// answers, and the driver record (NAK's SM53 code) of every pipeline a state's creation makes (m2v makes the pipeline its first
// draw is predicted to take inside the creation, pipeline/predicted.rs). No draw is needed for Core Animation's states: on the r3v
// and r4d boots every driver record backboardd wrote is one a creation made.
//
// Each function is made from the library the journal names (lib=): a file by its device path, read from the library store
// (--libs, libraries.py: the 21H461 OS volume's and cryptex's libraries, app bundles' from the Data volume), or a library made from
// bytes by its SHA-256 (data:<sha>, the frontend's <cache>/libs/ dumps, harvested). A function without lib= is QuartzCore's (the
// given default.metallib): backboardd's journals from before the journal named libraries. A line whose library the store does not
// have, or whose function its library lacks, is skipped and counted; one m2v refuses is printed and counted. Fails (exit 1) only
// when a QuartzCore line of a journal before --derived is refused: m2v would refuse it on the device too, in the render server.
// Lines of the journals after --derived are guesses (tools/gpu/shaders/derive.py), and a refused guess is only counted.
// PRECOMPILE_PROGRESS=<file>: the line counts by kind ("lines <S> <R> <C> <D>"), then "<kind> <index>" of each line before
// it is made, so a crash names the line it died in.
// The cache is shared by every process: m2v's names hash the build, the key and the device's profile, none of them per process
// (r4d-b: iconservicesagent and backboardd wrote the same record names, byte for byte).
// The output: the summary line ("precompile: ..."), one "library" line per library with what was replayed (ok), refused (ref) and
// skipped (skip) of each kind, then the frontend's rates line over the replay (cache=h<hits>/m<misses>/w<writes>, drv=l<driver
// loads>/...): with a seed beneath an empty cache (METAL2VULKAN_SHADER_CACHE_SEED), every state of the seed shows as a hit.
// usage: precompile [--libs <store>] <host libvulkan_nouveau.dylib> <cache dir> <QuartzCore default.metallib> <journal>...
//        [--derived <journal>...] [--guess-dispatch]
// D lines (the frontend's first dispatch of each shape a compute state takes) are made by dispatching the state at that shape.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <map>
#include <regex>
#include <set>
#include <stdio.h>
#include <string>
#include <vector>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
#include "PrecompileCompute.h"

using namespace precompile;

static void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

struct Counts {
    unsigned made = 0, refused = 0, skipped = 0;
};

static std::map<std::string, std::map<char, Counts>> gCounts;
static bool gQuartzCoreRefused = false;

static void count(const std::string &file, char kind, Outcome outcome, const std::string &what, NSError *error,
                  bool derived)
{
    Counts &c = gCounts[file][kind];
    if (outcome == Made)
        c.made++;
    if (outcome == NoLibrary || outcome == NoFunction)
        c.skipped++;
    if (outcome == Refused) {
        c.refused++;
        // A corpus state of QuartzCore's library is one the render server made: the device refuses it too.
        const bool corpusQuartzCore = file == gQuartzCore && !derived;
        gQuartzCoreRefused = gQuartzCoreRefused || corpusQuartzCore;
        fprintf(stderr, "%s %c %s (%s): %s\n", corpusQuartzCore ? "REFUSED-CORPUS" : "REFUSED", kind, what.c_str(),
                file.c_str(), error.localizedDescription.UTF8String ?: "a function");
    }
}

static Outcome renderState(const std::string &line, std::string &file, std::string &label, NSError **why)
{
    RenderInput input;
    const Outcome outcome = renderInput(line, input, why);
    file = input.file;
    label = input.label;
    if (outcome != Made)
        return outcome;
    NSError *error = nil;
    if (![gDevice newRenderPipelineStateWithDescriptor:input.descriptor
                                               options:input.options
                                            reflection:nil
                                                 error:&error]) {
        *why = error;
        fprintf(stderr, "render state %s: %s\n", label.c_str(), error.localizedDescription.UTF8String);
        return Refused;
    }
    return Made;
}

// A C line (frontend journalComputePipeline): made from its kernel alone (form=f) or from a descriptor (form=d: its fields, the
// kernel k, visible linked functions l and private ones p), with its options. The outcome, counted under the kernel's library.
static Outcome computeState(const std::string &line, std::string &file, std::string &label, NSError **why,
                            id<MTLComputePipelineState> *made = nullptr)
{
    ComputeInput input;
    const Outcome outcome = computeInput(line, input, why);
    file = input.file;
    label = input.label;
    if (outcome != Made)
        return outcome;
    NSError *error = nil;
    id<MTLComputePipelineState> state =
        input.descriptorForm ? [gDevice newComputePipelineStateWithDescriptor:input.descriptor
                                                                      options:input.options
                                                                   reflection:nil
                                                                        error:&error]
                             : [gDevice newComputePipelineStateWithFunction:input.descriptor.computeFunction
                                                                    options:input.options
                                                                 reflection:nil
                                                                      error:&error];
    if (!state) {
        fprintf(stderr, "compute state %s %s: %s\n", input.kernel.name.c_str(), label.c_str(),
                error.localizedDescription.UTF8String);
        *why = error;
        if (label == "-")
            label = input.kernel.name;
        return Refused;
    }
    if (made)
        *made = state;
    return Made;
}

// One dispatch of `state` with nothing bound, committed and waited for: it makes the VkPipeline of each region the shape names
// (the threadgroup size is a specialization; dispatchThreads adds an edge region per axis with a remainder), which only a dispatch
// makes, and m2v writes its driver record as the device's first dispatch of that shape would.
static bool dispatchShape(id<MTLComputePipelineState> state, const unsigned group[3], const unsigned edge[3],
                          bool threads)
{
    static id<MTLCommandQueue> queue = [gDevice newCommandQueue];
    id<MTLCommandBuffer> buffer = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [buffer computeCommandEncoder];
    [encoder setComputePipelineState:state];
    const MTLSize size = MTLSizeMake(group[0], group[1], group[2]);
    if (threads)
        [encoder dispatchThreads:MTLSizeMake(group[0] + edge[0], group[1] + edge[1], group[2] + edge[2])
            threadsPerThreadgroup:size];
    else
        [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:size];
    [encoder endEncoding];
    [buffer commit];
    [buffer waitUntilCompleted];
    return buffer.status == MTLCommandBufferStatusCompleted;
}

// A D line (frontend journalDispatch): the state as its C line names it, dispatched at the journaled shape.
static Outcome dispatchLine(const std::string &line, std::string &file, std::string &label, NSError **why)
{
    const size_t bar = line.find(" | ");
    unsigned group[3] = {}, edge[3] = {}, threads = 0;
    if (bar == std::string::npos || sscanf(line.c_str(), "D ok=1 tg=%u,%u,%u edge=%u,%u,%u threads=%u", group,
                                           group + 1, group + 2, edge, edge + 1, edge + 2, &threads) != 7)
        return Refused;
    id<MTLComputePipelineState> state = nil;
    const Outcome outcome = computeState("C ok=1 " + line.substr(bar + 3), file, label, why, &state);
    if (outcome != Made)
        return outcome;
    return dispatchShape(state, group, edge, threads) ? Made : Refused;
}

// --guess-dispatch: each compute state is also dispatched at the shape Apple's documentation computes for a 2D grid,
// threadExecutionWidth by maxTotalThreadsPerThreadgroup / threadExecutionWidth, for states no journal gave a D line of.
static bool gGuessDispatch;
static unsigned gGuessed, gGuessRefused;

static void guessDispatch(id<MTLComputePipelineState> state)
{
    const NSUInteger width = state.threadExecutionWidth, most = state.maxTotalThreadsPerThreadgroup;
    if (!width || most < width)
        return;
    const unsigned group[3] = {(unsigned)width, (unsigned)(most / width), 1}, edge[3] = {};
    (dispatchShape(state, group, edge, false) ? gGuessed : gGuessRefused)++;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        int first = 1;
        if (argc > 2 && !strcmp(argv[1], "--libs")) {
            gStore = argv[2];
            first = 3;
        }
        if (argc - first < 4) {
            fprintf(
                stderr,
                "usage: precompile [--libs <store>] <nvk dylib> <cache dir> <default.metallib> <pipelines.log>...\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        setenv("NVK_DARWIN_SERVICE", "AGXAccelerator", 0);
        gDevice = native_bridge_metal_open(argv[first], argv[first + 1], 0, missing);
        if (!gDevice) {
            fprintf(stderr, "NO DEVICE: %s\n", nmb_last_error());
            return 1;
        }
        gQuartzCore = argv[first + 2];
        if (!library(gQuartzCore).library)
            return 1;
        char rates[1024];
        native_bridge_metal_rates(rates, sizeof rates);  // the replay's rates start here
        const char *progressPath = getenv("PRECOMPILE_PROGRESS");
        FILE *progress = progressPath ? fopen(progressPath, "w") : nullptr;

        // The journals, oldest first: each line once, in the order the device first made it; specializations first.
        std::vector<std::string> lines[4];  // S, R, C, D
        std::vector<bool> derived[4];
        std::set<std::string> seen;
        bool derivedJournals = false;
        for (int a = first + 3; a < argc; a++) {
            if (!strcmp(argv[a], "--derived")) {
                derivedJournals = true;
                continue;
            }
            if (!strcmp(argv[a], "--guess-dispatch")) {
                gGuessDispatch = true;
                continue;
            }
            NSString *text = [NSString stringWithContentsOfFile:@(argv[a]) encoding:NSUTF8StringEncoding error:nil];
            if (!text)
                text = [NSString stringWithContentsOfFile:@(argv[a]) encoding:NSISOLatin1StringEncoding error:nil];
            if (!text) {
                fprintf(stderr, "cannot read %s\n", argv[a]);
                return 1;
            }
            for (NSString *l in [text componentsSeparatedByString:@"\n"]) {
                std::string line = l.UTF8String ?: "";
                const char *kinds = "SRCD";
                const char *kind = line.size() > 2 && line[1] == ' ' ? strchr(kinds, line[0]) : nullptr;
                if (!kind || line.find(" ok=1 ") == std::string::npos)
                    continue;
                // Deduplicated by what the line makes: without its time, thread, costs, label and callers.
                std::string key;
                for (const std::string &p : split(line, " | "))
                    if (p.rfind("qc ", 0) != 0)
                        key += p + " | ";
                key = std::regex_replace(key, std::regex(" (t|tid|main|us|cpu|sys|flt|pin|csw|label)=\\S*"), "");
                if (seen.insert(key).second) {
                    lines[kind - kinds].push_back(line);
                    derived[kind - kinds].push_back(derivedJournals);
                }
            }
        }
        if (progress) {
            fprintf(progress, "lines %zu %zu %zu %zu\n", lines[0].size(), lines[1].size(), lines[2].size(),
                    lines[3].size());
            fflush(progress);
        }
        unsigned made[4] = {}, refused[4] = {}, skipped[4] = {};
        double ms[4] = {};
        std::set<std::string> dispatched;  // the states of D lines, which --guess-dispatch leaves alone
        for (const std::string &line : lines[3])
            if (size_t bar = line.find(" | "); bar != std::string::npos)
                dispatched.insert(line.substr(bar + 3));
        for (int kind = 0; kind < 4; kind++) {
            const uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            for (size_t i = 0; i < lines[kind].size(); i++) {
                const std::string &line = lines[kind][i];
                if (progress) {
                    fprintf(progress, "%c %zu\n", "SRCD"[kind], i);
                    fflush(progress);
                }
                std::string file, label;
                Outcome outcome;
                NSError *error = nil;
                if (kind == 0) {
                    size_t n = line.find(" name="), k = line.find(" k=", n == std::string::npos ? 0 : n),
                           bar = line.find(" | ", k == std::string::npos ? 0 : k);
                    if (n == std::string::npos || k == std::string::npos)
                        continue;
                    size_t lib = line.rfind(" lib=", n);
                    Stage s =
                        stage(line.substr(n + 6, k - n - 6) +
                              (lib == std::string::npos ? std::string() : " " + line.substr(lib + 1, n - lib - 1)) +
                              " spec " + line.substr(k + 3, (bar == std::string::npos ? line.size() : bar) - k - 3));
                    file = s.file;
                    label = s.name;
                    function(s, outcome, &error);
                } else if (kind == 1)
                    outcome = renderState(line, file, label, &error);
                else if (kind == 3)
                    outcome = dispatchLine(line, file, label, &error);
                else {
                    id<MTLComputePipelineState> state = nil;
                    outcome = computeState(line, file, label, &error, &state);
                    const size_t bar = line.find(" | ");
                    const std::string named = bar == std::string::npos ? line : line.substr(line.find(' ', 2) + 1);
                    if (state && gGuessDispatch && !dispatched.count(named))
                        guessDispatch(state);
                }
                count(file, "SRCD"[kind], outcome, label, error, derived[kind][i]);
                (outcome == Made ? made : outcome == Refused ? refused : skipped)[kind]++;
            }
            ms[kind] = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - began) / 1e6;
        }
        printf(
            "precompile: %u specializations (%u failed) in %.0f ms, %u states (%u refused) in %.0f ms, %u compute "
            "states (%u refused) in %.0f ms, %u dispatches (%u refused) in %.0f ms, %u guessed (%u refused); skipped "
            "(library or function not in the store): %u/%u/%u/%u\n",
            made[0], refused[0], ms[0], made[1], refused[1], ms[1], made[2], refused[2], ms[2], made[3], refused[3],
            ms[3], gGuessed, gGuessRefused, skipped[0], skipped[1], skipped[2], skipped[3]);
        for (auto &[file, kinds] : gCounts) {
            std::string name = file == gQuartzCore ? "QuartzCore (default)" : file;
            if (!gStore.empty() && name.rfind(gStore, 0) == 0)
                name = name.substr(gStore.size() + 1);
            printf("library %-90s", name.c_str());
            for (char kind : std::string("SRCD"))
                if (kinds.count(kind))
                    printf(" %c ok=%u ref=%u skip=%u", kind, kinds[kind].made, kinds[kind].refused,
                           kinds[kind].skipped);
            printf("%s\n", library(file).library ? "" : " (not in the store)");
        }
        if (native_bridge_metal_rates(rates, sizeof rates))
            printf("rates: %s\n", rates);
        return gQuartzCoreRefused ? 1 : 0;
    }
}
