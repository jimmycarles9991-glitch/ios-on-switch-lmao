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

static void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

static std::vector<std::string> split(const std::string &s, const std::string &by = " ")
{
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= s.size()) {
        size_t e = s.find(by, at);
        if (e == std::string::npos)
            e = s.size();
        if (e > at)
            out.push_back(s.substr(at, e - at));
        at = e + by.size();
    }
    return out;
}

// "name:type:hex" or "#index:type:hex" items (the journal's constants text) into constant values.
static MTLFunctionConstantValues *constants(const std::vector<std::string> &items)
{
    MTLFunctionConstantValues *v = [MTLFunctionConstantValues new];
    for (const std::string &item : items) {
        size_t a = item.rfind(':'), b = a == std::string::npos ? a : item.rfind(':', a - 1);
        if (a == std::string::npos || b == std::string::npos)
            continue;
        std::string name = item.substr(0, b), type = item.substr(b + 1, a - b - 1), hex = item.substr(a + 1);
        uint8_t bytes[64] = {};
        for (size_t i = 0; i + 1 < hex.size() && i / 2 < sizeof bytes; i += 2)
            bytes[i / 2] = (uint8_t)strtoul(hex.substr(i, 2).c_str(), nullptr, 16);
        if (name[0] == '#')
            [v setConstantValue:bytes
                           type:(MTLDataType)atoi(type.c_str())
                        atIndex:strtoul(name.c_str() + 1, nullptr, 10)];
        else
            [v setConstantValue:bytes type:(MTLDataType)atoi(type.c_str()) withName:@(name.c_str())];
    }
    return v;
}

// ---- libraries ----
static const std::string kQuartzCore = "/System/Library/Frameworks/QuartzCore.framework/default.metallib";
static std::string gStore, gQuartzCore;
static id<MTLDevice> gDevice;

// A journal token with its %XX escapes undone (the frontend's journalToken).
static std::string unescape(const std::string &token)
{
    std::string out;
    for (size_t i = 0; i < token.size(); i++)
        if (token[i] == '%' && i + 2 < token.size()) {
            out += (char)strtoul(token.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else
            out += token[i];
    return out;
}

// The host file of the library the journal names `id` ("" is QuartzCore's).
static std::string hostFile(const std::string &id)
{
    std::string path = unescape(id);
    if (path.rfind("/System/Cryptexes/OS/", 0) == 0)
        path = path.substr(20);
    if (path.rfind("/var/", 0) == 0)
        path = "/private" + path;  // the store keeps the Data volume's paths as /private/var
    if (path.empty() || path == kQuartzCore)
        return gQuartzCore;
    if (path.rfind("data:", 0) == 0)
        return gStore + "/data/" + path.substr(5) + ".metallib";
    return gStore + "/root" + path;
}

struct Library {
    id<MTLLibrary> library;
    std::set<std::string> names;
};

static std::map<std::string, Library> gLibraries;  // by host file; nil library: none there, or the frontend refused it

static Library &library(const std::string &file)
{
    auto found = gLibraries.find(file);
    if (found != gLibraries.end())
        return found->second;
    Library &l = gLibraries[file];
    NSError *error = nil;
    if (access(file.c_str(), R_OK) == 0) {
        l.library = [gDevice newLibraryWithURL:[NSURL fileURLWithPath:@(file.c_str())] error:&error];
        if (!l.library)
            fprintf(stderr, "library %s: %s\n", file.c_str(), error.localizedDescription.UTF8String);
        for (NSString *name in l.library.functionNames)
            l.names.insert(name.UTF8String);
    }
    return l;
}

// A stage as the journal writes it (the frontend's stageText): the name, lib=<library>, "spec" for a specialization with no
// constants, then the constants.
struct Stage {
    std::string name, file;
    std::string as;  // the specialized name it was made under (as=)
    bool specialized = false;
    std::vector<std::string> constants;
};

static Stage stage(const std::string &text)
{
    Stage s;
    std::vector<std::string> tokens = split(text);
    if (tokens.empty())
        return s;
    s.name = tokens[0];
    std::string id;
    for (size_t i = 1; i < tokens.size(); i++)
        if (tokens[i].rfind("lib=", 0) == 0)
            id = tokens[i].substr(4);
        else if (tokens[i].rfind("as=", 0) == 0)
            s.as = tokens[i].substr(3);
        else if (tokens[i] == "spec")
            s.specialized = true;
        else
            s.constants.push_back(tokens[i]);
    s.file = hostFile(id);
    s.specialized = s.specialized || !s.constants.empty();
    return s;
}

// What became of a line, counted per library (the vertex function's, the kernel's) and kind.
enum Outcome {
    Made,
    Refused,
    NoLibrary,
    NoFunction
};

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

// The stage's function: newFunctionWithName: where the journal gives no constants and no "spec", else
// newFunctionWithName:constantValues:error: with exactly its constants. Nil with `outcome` when it cannot be made.
static id<MTLFunction> function(const Stage &s, Outcome &outcome, NSError **error)
{
    Library &l = library(s.file);
    if (!l.library) {
        outcome = NoLibrary;
        return nil;
    }
    if (!l.names.count(s.name)) {
        outcome = NoFunction;
        return nil;
    }
    id<MTLFunction> f = nil;
    if (!s.as.empty()) {
        MTLFunctionDescriptor *d = [MTLFunctionDescriptor functionDescriptor];
        d.name = @(s.name.c_str());
        d.specializedName = @(s.as.c_str());
        d.constantValues = constants(s.constants);
        f = [l.library newFunctionWithDescriptor:d error:error];
    } else
        f = s.specialized
                ? [l.library newFunctionWithName:@(s.name.c_str()) constantValues:constants(s.constants) error:error]
                : [l.library newFunctionWithName:@(s.name.c_str())];
    outcome = f ? Made : Refused;
    return f;
}

// The functions an R line's state links, after its stages (" | <vl|vp|fl|fp> <stage>", the frontend's linkedText), into `d`
static Outcome linkedStages(const std::vector<std::string> &parts, MTLRenderPipelineDescriptor *d, NSError **error)
{
    for (size_t i = 3; i < parts.size(); i++) {
        const std::string tag = parts[i].substr(0, 3);
        if (tag != "vl " && tag != "vp " && tag != "fl " && tag != "fp ")
            continue;
        Outcome outcome = Made;
        id<MTLFunction> f = function(stage(parts[i].substr(3)), outcome, error);
        if (outcome != Made)
            return outcome;
        MTLLinkedFunctions *l = tag[0] == 'v' ? d.vertexLinkedFunctions : d.fragmentLinkedFunctions;
        if (tag[1] == 'l')
            l.functions = [(l.functions ?: @[]) arrayByAddingObject:f];
        else
            l.privateFunctions = [(l.privateFunctions ?: @[]) arrayByAddingObject:f];
    }
    return Made;
}

// ---- lines ----
// An R line's descriptor (frontend journalRenderPipeline): samples, topology class, depth/stencil formats, options, label, color
// attachments (format:writeMask:blend:srcRGB:dstRGB:opRGB:srcA:dstA:opA), vertex attributes (format:offset:buffer:stride:step:rate),
// then both functions and the functions they link. The outcome, counted under the vertex function's library.
static Outcome renderState(const std::string &line, std::string &file, std::string &label, NSError **why)
{
    std::vector<std::string> parts = split(line, " | ");
    if (parts.size() < 3 || parts[1].rfind("v ", 0) != 0 || parts[2].rfind("f ", 0) != 0)
        return Refused;
    Stage v = stage(parts[1].substr(2)), f = stage(parts[2].substr(2));
    file = v.file;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    MTLVertexDescriptor *vd = nil;
    MTLPipelineOption options = MTLPipelineOptionNone;
    for (const std::string &h : split(parts[0])) {
        if (h.rfind("label=", 0) == 0) {
            label = h.substr(6);
            if (label != "-")
                d.label = @(label.c_str());
        } else if (h.rfind("samples=", 0) == 0)
            d.rasterSampleCount = strtoul(h.c_str() + 8, nullptr, 10);
        else if (h.rfind("topo=", 0) == 0)
            d.inputPrimitiveTopology = (MTLPrimitiveTopologyClass)strtoul(h.c_str() + 5, nullptr, 10);
        else if (h.rfind("opt=", 0) == 0)
            options = (MTLPipelineOption)strtoull(h.c_str() + 4, nullptr, 10);
        else if (h.rfind("ds=", 0) == 0) {
            unsigned a = 0, b = 0;
            sscanf(h.c_str() + 3, "%u/%u", &a, &b);
            d.depthAttachmentPixelFormat = (MTLPixelFormat)a;
            d.stencilAttachmentPixelFormat = (MTLPixelFormat)b;
        } else if (h[0] == 'c' && isdigit(h[1])) {
            unsigned i, x[9] = {};
            if (sscanf(h.c_str(), "c%u=%u:%u:%u:%u:%u:%u:%u:%u:%u", &i, x, x + 1, x + 2, x + 3, x + 4, x + 5, x + 6,
                       x + 7, x + 8) != 10 ||
                i >= 8)
                continue;
            MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
            c.pixelFormat = (MTLPixelFormat)x[0];
            c.writeMask = x[1];
            c.blendingEnabled = x[2];
            c.sourceRGBBlendFactor = (MTLBlendFactor)x[3];
            c.destinationRGBBlendFactor = (MTLBlendFactor)x[4];
            c.rgbBlendOperation = (MTLBlendOperation)x[5];
            c.sourceAlphaBlendFactor = (MTLBlendFactor)x[6];
            c.destinationAlphaBlendFactor = (MTLBlendFactor)x[7];
            c.alphaBlendOperation = (MTLBlendOperation)x[8];
        } else if (h[0] == 'a' && isdigit(h[1])) {
            unsigned i, x[6] = {};
            if (sscanf(h.c_str(), "a%u=%u:%u:%u:%u:%u:%u", &i, x, x + 1, x + 2, x + 3, x + 4, x + 5) != 7 || i >= 31 ||
                x[2] >= 31)
                continue;
            if (!vd)
                vd = [MTLVertexDescriptor vertexDescriptor];
            vd.attributes[i].format = (MTLVertexFormat)x[0];
            vd.attributes[i].offset = x[1];
            vd.attributes[i].bufferIndex = x[2];
            vd.layouts[x[2]].stride = x[3];
            vd.layouts[x[2]].stepFunction = (MTLVertexStepFunction)x[4];
            vd.layouts[x[2]].stepRate = x[5];
        }
    }
    d.vertexDescriptor = vd;
    Outcome outcome = Made;
    NSError *error = nil;
    d.vertexFunction = function(v, outcome, &error);
    if (f.name != "-" && outcome == Made)
        d.fragmentFunction = function(f, outcome, &error);
    if (outcome == Made)
        outcome = linkedStages(parts, d, &error);
    if (outcome != Made) {
        *why = error;
        return outcome;
    }
    if (![gDevice newRenderPipelineStateWithDescriptor:d options:options reflection:nil error:&error]) {
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
    std::vector<std::string> parts = split(line, " | ");
    std::map<std::string, std::string> fields;
    for (const std::string &h : split(parts.empty() ? std::string() : parts[0])) {
        size_t eq = h.find('=');
        if (eq != std::string::npos)
            fields[h.substr(0, eq)] = h.substr(eq + 1);
    }
    label = fields.count("label") ? unescape(fields["label"]) : "-";
    std::vector<Stage> linked, hidden;
    Stage kernel;
    for (size_t i = 1; i < parts.size(); i++) {
        const std::string &p = parts[i];
        if (p.rfind("k ", 0) == 0)
            kernel = stage(p.substr(2));
        if (p.rfind("l ", 0) == 0)
            linked.push_back(stage(p.substr(2)));
        if (p.rfind("p ", 0) == 0)
            hidden.push_back(stage(p.substr(2)));
    }
    file = kernel.file;
    if (kernel.name.empty())
        return Refused;
    Outcome outcome = Made;
    NSError *error = nil;
    id<MTLFunction> k = function(kernel, outcome, &error);
    if (outcome != Made) {
        *why = error;
        return outcome;
    }
    const MTLPipelineOption options = (MTLPipelineOption)strtoull(fields["opt"].c_str(), nullptr, 10);
    id<MTLComputePipelineState> state = nil;
    if (fields["form"] != "d")
        state = [gDevice newComputePipelineStateWithFunction:k options:options reflection:nil error:&error];
    else {
        MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
        d.computeFunction = k;
        if (label != "-")
            d.label = @(label.c_str());
        d.maxTotalThreadsPerThreadgroup = strtoul(fields["max"].c_str(), nullptr, 10);
        d.threadGroupSizeIsMultipleOfThreadExecutionWidth = fields["tgm"] == "1";
        d.supportIndirectCommandBuffers = fields["icb"] == "1";
        d.maxCallStackDepth = strtoul(fields["depth"].c_str(), nullptr, 10);
        d.supportAddingBinaryFunctions = fields["abf"] == "1";
        const std::string &mutability = fields["mut"];
        for (size_t i = 0; i < 31 && i < mutability.size(); i++)
            d.buffers[i].mutability = (MTLMutability)(mutability[i] - '0');
        NSMutableArray *visible = [NSMutableArray array], *private_ = [NSMutableArray array];
        for (auto &[stages, into] :
             std::vector<std::pair<std::vector<Stage> *, NSMutableArray *>>{{&linked, visible}, {&hidden, private_}})
            for (const Stage &s : *stages) {
                id<MTLFunction> f = function(s, outcome, &error);
                if (outcome != Made)
                    return outcome;
                [into addObject:f];
            }
        if (visible.count || private_.count) {
            MTLLinkedFunctions *functions = [MTLLinkedFunctions linkedFunctions];
            functions.functions = visible;
            functions.privateFunctions = private_;
            d.linkedFunctions = functions;
        }
        state = [gDevice newComputePipelineStateWithDescriptor:d options:options reflection:nil error:&error];
    }
    if (!state) {
        fprintf(stderr, "compute state %s %s: %s\n", kernel.name.c_str(), label.c_str(),
                error.localizedDescription.UTF8String);
        *why = error;
        if (label == "-")
            label = kernel.name;
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
