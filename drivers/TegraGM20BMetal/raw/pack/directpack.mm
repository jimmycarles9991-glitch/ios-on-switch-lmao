// SPDX-License-Identifier: MIT
// The pack generator's records made directly (runtime/RawDirect.h): every state of a bucket's corpus compiled from m2v's
// translation and the stage compile (third_party/mesa/src/nouveau/switch_compile, linked in), as the runtime makes a state
// the pack lacks, with no device, pipeline, draw or capture: the translator's own libraries and functions
// (runtime/RawOffline.h). directlink.py makes the pack of them.
// usage: directpack <default.metallib> <corpus.log> <records.bin> [<selectors>]
// (PROBE_OWN_LIB, PROBE_LIB_ROOT and PROBE_LABEL as probe.mm reads them)
//
// records.bin, little-endian, per state: "DREC", the outcome (0 made, 1 refused, 2 failed), the key and the reason (u32 length,
// bytes), then for a made state its PackPipeline (raw_pack.h; vs 0, fs 1, no blob), its key string, its registers (u32 count,
// method and value pairs: raw_emit's), its vertex and fragment programs (header + code, shader data, constant buffer types:
// each a u32 length and bytes), its constexpr sampler headers (u32 count, eight words each), which its entries number from 0,
// and its fragment program's split: u32 0, or 1 and its PackSplit (no program indices), its per-pixel program and its prologue
// (as the programs above; the prologue's header 80 bytes of zeros), its program variants, and the iterated blend program a
// fetch-free form names (u32 length and bytes: ca/fetchfree.py, iterated/table.py; empty for any other record), then its
// argument tables' words (u32 count and words: the vertex table's, then the fragment table's), then m2v's own instantiation
// of the probe draw that binds every buffer slot at its probe offset (RawDirect.h BoundDraw: its push mask, its 32 root push
// words as bytes, and each stage's argument table as u32 count and words, vertex then fragment), then a tessellation
// state's control and evaluation programs (u32 0, or 2 and the programs as above), and the registers outside raw_emit's it
// sets and those its draws set back after it (raw_record.h extras and restore: each u32 count and (method, value) words).
// A record of one of a state's variants has the variant's name after its key (runtime/raw_pack.h variantNameAt); a
// state's fetch-free form is under the state's key (PipelineFetchFreeForm). A record's stride twin follows it as a record
// of its own, of its form, under raw_record.h strideTwinKey(key), which its PackPipeline::strideTwin hashes.
// Then per compute dispatch shape (the corpus's D lines, as the kernel probe read them: kernel_shapes.h): "DKRN", the outcome,
// its raw_compute_key.h key and the reason, then for a made kernel its PackKernel (raw_direct_compute.h; program 0), its code,
// shader data and constant buffer types, its sampler headers, which its sampler entries number from 0 (slot 0x80 | n), its
// argument table's words (u32 count and words), and its state's answers as the runtime's compute prepare gives them
// (runtime/raw_pack.h PackComputeFacts: three u32).
// And per state the corpus names, once: "DRFL", u32 0 for a render state (under its key less any variant name) or 1 for a
// compute state (under a kernel key of it), the key, and its reflection (runtime/raw_reflection_blob.h: u32 length, bytes).
// A corpus line that makes no state is a refused DREC, and a D line that makes no kernel a refused DKRN, keyed by the line.
// PROBE_DATA_LIBS names the directory of the corpus's data: libraries.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstdlib>
#include <set>
#include "../runtime/raw_emit.h"
#include "../runtime/raw_xr_pass.h"
#include "../runtime/raw_zeta.h"
#include "TwoPlaneStandIn.h"
#include "direct_kernels.h"
#include "direct_states.h"
#include "fetch_free_make.h"
#include "../runtime/RawOffline.h"
#include "selector_configs.h"

namespace {

struct Writer {
    FILE *f;

    void u32(uint32_t v)
    {
        fwrite(&v, 4, 1, f);
    }

    void bytes(const void *p, size_t n)
    {
        u32(uint32_t(n));
        fwrite(p, 1, n, f);
    }

    void text(const std::string &s)
    {
        bytes(s.data(), s.size());
    }

    template <typename T> void list(const std::vector<T> &v)
    {
        bytes(v.data(), v.size() * sizeof(T));
    }
};

// The record's head; false: no more follows (it was not made)
bool head(Writer &w, const char *tag, const std::string &key, raw::CompileOutcome outcome, const std::string &why)
{
    fwrite(tag, 1, 4, w.f);
    w.u32(outcome == raw::CompileOutcome::Made ? 0 : outcome == raw::CompileOutcome::Refused ? 1 : 2);
    w.text(key);
    w.text(why);
    return outcome == raw::CompileOutcome::Made;
}

// `twinRegisters`: the registers of `rec`'s stride twin (Direct::strideTwinRegisters), which follows it as a record of its
// own under strideTwinKey(key), of its form
void record(Writer &w, const std::string &key, raw::CompileOutcome outcome, const std::string &why,
            const raw::RuntimeRecord &rec, const std::map<uint32_t, uint32_t> &registers, const raw::BoundDraw &bound,
            const std::map<uint32_t, uint32_t> &twinRegisters, const std::string &iterated = "")
{
    if (!head(w, "DREC", key, outcome, why))
        return;
    raw::PackPipeline pipeline = rec.pipeline;
    const std::string twinKey = raw::strideTwinKey(key);
    pipeline.strideTwin = rec.strideTwin ? raw::packKeyHash(twinKey) : 0;
    w.bytes(&pipeline, sizeof pipeline);
    w.text(rec.keyString);
    std::vector<uint32_t> pairs;
    for (const auto &[m, v] : registers) {
        pairs.push_back(m);
        pairs.push_back(v);
    }
    w.list(pairs);
    auto program = [&](const raw::CapturedProgram &p) {
        w.list(p.image);
        w.list(p.data);
        w.list(p.cbufTypes);
        w.list(p.farUbos);
        w.u32(p.gprs | uint32_t(p.boundTextures) << 8);
    };
    for (const raw::CapturedProgram &p : rec.programs)
        program(p);
    w.list(rec.samplers);
    w.u32(rec.split);
    if (rec.split) {
        w.bytes(&rec.splitRecord, sizeof rec.splitRecord);
        program(rec.splitPrograms[0]);
        program(rec.splitPrograms[1]);
    }
    w.u32(uint32_t(rec.variants.size()));
    for (const raw::RuntimeRecord::Variant &v : rec.variants) {
        w.u32(v.assume);
        program(v.program);
    }
    w.text(iterated);
    w.list(rec.spill);
    w.u32(bound.pushMask);
    w.bytes(bound.push, sizeof bound.push);
    w.list(bound.spill[0]);
    w.list(bound.spill[1]);
    const bool tess = raw::tessellates(rec.pipeline);
    w.u32(tess ? 2 : 0);
    for (uint32_t k = 0; tess && k < 2; k++)
        program(rec.tessPrograms[k]);
    for (const auto *list : {&rec.extras, &rec.restore}) {
        std::vector<uint32_t> words;
        for (const auto &[m, v] : *list)
            words.insert(words.end(), {m, v});
        w.list(words);
    }
    w.list(rec.far);
    w.u32(uint32_t(rec.selectors.size()));
    if (!rec.selectors.empty())
        w.u32(rec.selectorAssume);
    for (const raw::RuntimeRecord::Selector &selector : rec.selectors) {
        w.u32(selector.id);
        program(selector.program);
    }
    w.list(rec.selectorGeneric);
    if (!rec.strideTwin)
        return;
    raw::RuntimeRecord twin = *rec.strideTwin;
    twin.pipeline.flags |= rec.pipeline.flags & raw::PipelineFetchFreeForm;
    for (uint32_t c = 0; c < 8; c++)
        twin.pipeline.fetchRung[c] =
            (rec.pipeline.flags & raw::PipelineFetchFreeForm)
                ? rec.pipeline.fetchRung[c]
                : uint8_t(twin.pipeline.fetchRung[c] | (rec.pipeline.fetchRung[c] & ~raw::kFetchRungMask));
    record(w, twinKey, raw::CompileOutcome::Made, "", twin, twinRegisters, bound, {}, iterated);
}

// The rung a Core Animation state's fragment blend is a candidate for (rawpack.py's split tags its line " rung=<2|3>":
// ca/caset.py fetch_rung), marked on each color the state fetches and writes with no pipeline blend: what a device probe
// would have to confirm before such a fetch leaves the interlock
void markCandidates(const std::string &line, raw::PackPipeline &p)
{
    const size_t at = line.find(" rung=");
    const int rung = at == std::string::npos ? 0 : atoi(line.c_str() + at + 6);
    const uint8_t candidate = rung == 2 ? raw::kFetchCandidateBlend : rung == 3 ? raw::kFetchCandidateIterated : 0;
    const std::string head = line.substr(0, line.find(" | "));
    for (size_t c = head.find(" c"); candidate && c != std::string::npos; c = head.find(" c", c + 2)) {
        unsigned color = 0, x[9] = {};
        if (sscanf(head.c_str() + c, " c%u=%u:%u:%u:%u:%u:%u:%u:%u:%u", &color, x, x + 1, x + 2, x + 3, x + 4, x + 5,
                   x + 6, x + 7, x + 8) != 10 ||
            color >= 8)
            continue;
        const bool replaces = x[3] == 1 && x[4] == 0 && x[5] == 0 && x[6] == 1 && x[7] == 0 && x[8] == 0;
        if ((p.fetchRung[color] & raw::kFetchRungMask) == raw::FetchInterlock && (!x[2] || replaces))
            p.fetchRung[color] |= candidate;
    }
}

// The depth and stencil test classes a state's draws ask its variant for (RawMetalCubic.mm drawnRecord): a state that fetches
// beside a depth or stencil attachment without the storage form, each class raw_fragment_tests.h FragmentTests names above Any
// (Pass, ReadOnly, DepthWrite, StencilWrite: 1-4) that its attachments' formats can reach (fragmentTests answers DepthWrite
// only of a zeta with depth, StencilWrite only of one with stencil)
std::vector<uint8_t> testClasses(const raw::PackPipeline &p, const std::string &key)
{
    unsigned depth = 0, stencil = 0;
    const size_t ds = key.find("ds=");
    if (!(p.flags & raw::PipelineFetches) || (p.flags & raw::PipelineStorageMetadata) || ds == std::string::npos ||
        sscanf(key.c_str() + ds, "ds=%u/%u", &depth, &stencil) != 2 || !(depth || stencil))
        return {};
    std::vector<uint8_t> classes = {1, 2};
    if (raw::zetaHasDepth(depth) || raw::zetaHasDepth(stencil))
        classes.push_back(3);
    if (raw::zetaHasStencil(depth) || raw::zetaHasStencil(stencil))
        classes.push_back(4);
    return classes;
}

struct Counts {
    uint32_t made = 0, refused = 0, failed = 0, dropped = 0, split = 0, variants = 0, planes = 0, planesRefused = 0,
             sampled = 0;
    uint32_t free[4] = {}, freeFetching = 0, freeRefused = 0, freeBarrier = 0;
    uint32_t written = 0, twins = 0;
    uint32_t reflected[2] = {}, unreflected[2] = {};  // render, compute
};

// The state's reflection under `key`, once a state; a state the translator gives none of is counted and left out
void reflection(Writer &w, bool compute, const std::string &key, bool made, const std::string &blob,
                const std::string &why, Counts &n)
{
    if (!made) {
        if (!n.unreflected[compute]++)
            fprintf(stderr, "directpack: no reflection of %s: %s\n", key.c_str(), why.c_str());
        return;
    }
    n.reflected[compute]++;
    fwrite("DRFL", 1, 4, w.f);
    w.u32(compute);
    w.text(key);
    w.text(blob);
}

// The state's fetch-free form (fetch_free_make.h), under the state's key
void makeFetchFree(raw::Direct &direct, Writer &w, id<MTLLibrary> qc, const std::string &line, const std::string &key,
                   raw::ProbeTextures textures, Counts &n)
{
    probe::FetchFree form;
    raw::RuntimeRecord rec;
    std::map<uint32_t, uint32_t> registers;
    std::string why;
    bool barrier = false;
    const probe::FreeOutcome outcome =
        probe::makeFetchFree(direct, qc, line, textures, form, rec, &registers, why, barrier);
    if (outcome == probe::FreeOutcome::None)
        return;
    if (outcome != probe::FreeOutcome::Made) {
        const bool fetches = outcome == probe::FreeOutcome::Fetches;
        (fetches ? n.freeFetching : n.freeRefused)++;
        why = fetches ? "it still fetches" : why;
        fprintf(stderr, "directpack: no fetch-free form (%s): %s\n", why.c_str(), key.substr(0, 160).c_str());
        return;
    }
    rec.pipeline.flags |= raw::PipelineFetchFreeForm;
    rec.pipeline.fetchRung[form.colour] = form.rung == 2 ? raw::FetchBlend : raw::FetchIteratedBlend;
    n.free[form.rung]++;
    n.freeBarrier += barrier;
    record(w, key, raw::CompileOutcome::Made, why, rec, registers, direct.boundDraw(), direct.strideTwinRegisters(),
           form.rung == 3 ? form.program : "");
}

// A state's records: its own, its barrier form where its own is the storage form (one that computes the pipeline's blend
// has its twin only for a debug build's probe: raw_gpu.cpp Device::pipeline), its fetch-free form, and one per test class
// its draws can ask for (named " | tests=<class>" after its key, as the runtime names them); then, for a state of
// RGB10A8_2P_XR10 colours, all but the fetch-free form again of its two-plane form (raw_xr_pass.h), each named " | xr2p"
// after. A line naming a sampled two-plane or a written variant (" | xrs=", " | wtex=") makes that variant's records
// alone, its name last.
void makeState(raw::Direct &direct, Writer &w, id<MTLLibrary> qc, const std::string &line, raw::ProbeTextures textures,
               const probe::State &s, Counts &n)
{
    const auto make = [&](const std::string &key, raw::PackForm form, const raw::ProbeTextures &t,
                          raw::RuntimeRecord &rec) {
        std::map<uint32_t, uint32_t> registers;
        std::string why;
        const raw::CompileOutcome outcome = direct.make(s.d, key, rec, why, t, &registers, true, form);
        markCandidates(line, rec.pipeline);
        record(w, key, outcome, why, rec, registers, direct.boundDraw(), direct.strideTwinRegisters());
        n.twins += outcome == raw::CompileOutcome::Made && rec.strideTwin;
        return outcome;
    };
    const auto forms = [&](raw::ProbeTextures t, const char *suffix) {
        raw::RuntimeRecord rec;
        const raw::CompileOutcome outcome = make(s.key + suffix, raw::PackForm::Default, t, rec);
        if (outcome != raw::CompileOutcome::Made)
            return outcome;
        n.split += rec.split;
        if (rec.pipeline.flags & raw::PipelineStorageMetadata) {
            raw::RuntimeRecord barrier;
            make(s.key + suffix, raw::PackForm::Barrier, t, barrier);
        }
        // under a variant's name it would be the variant's own record to Pack::variantRecord, which tells only barrier
        // forms apart
        if (!*suffix && (rec.pipeline.flags & raw::PipelineFetches))
            makeFetchFree(direct, w, qc, line, s.key, t, n);
        for (const uint8_t tests : testClasses(rec.pipeline, s.key)) {
            raw::RuntimeRecord variant;
            t.fragmentTests = tests;
            n.variants += make(s.key + " | tests=" + std::to_string(tests) + suffix, raw::PackForm::Default, t,
                               variant) == raw::CompileOutcome::Made;
        }
        return outcome;
    };
    const std::string sampled =
        (s.xrSlots ? raw::xrSampledVariant(s.xrSlots) : std::string()) + raw::writtenVariant(s.written);
    textures.xrSlots = s.xrSlots;
    textures.written = s.written;
    const raw::CompileOutcome outcome = forms(textures, sampled.c_str());
    (outcome == raw::CompileOutcome::Made ? n.made : outcome == raw::CompileOutcome::Refused ? n.refused : n.failed)++;
    n.sampled += s.xrSlots && outcome == raw::CompileOutcome::Made;
    n.written += !s.written.empty() && outcome == raw::CompileOutcome::Made;
    if (outcome != raw::CompileOutcome::Made || !probe::twoPlaneColors(s.key))
        return;
    raw::ProbeTextures planes = textures;
    planes.twoPlane = true;
    const std::string planesName = raw::kXrPlanesVariant + sampled;
    (forms(planes, planesName.c_str()) == raw::CompileOutcome::Made ? n.planes : n.planesRefused)++;
}

void kernel(Writer &w, const std::string &key, raw::CompileOutcome outcome, const std::string &why,
            const raw::ComputeRecord &rec, const uint32_t facts[3])
{
    if (!head(w, "DKRN", key, outcome, why))
        return;
    w.bytes(&rec.kernel, sizeof rec.kernel);
    w.list(rec.code);
    w.list(rec.data);
    w.bytes(rec.cbufType, rec.cbufs);
    w.list(rec.samplers);
    w.list(rec.spill);
    w.bytes(facts, 3 * sizeof *facts);
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 4 && argc != 5) {
            fprintf(stderr, "usage: directpack <default.metallib> <corpus.log> <records.bin> [<selectors>]\n");
            return 2;
        }
        id<MTLDevice> libraries = (id<MTLDevice>)[RawOfflineDevice new];
        NSError *error = nil;
        id<MTLLibrary> qc = [libraries newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
        NSString *text = [NSString stringWithContentsOfFile:@(argv[2]) encoding:NSUTF8StringEncoding error:nil];
        Writer w = {fopen(argv[3], "wb")};
        if (!qc || !text || !w.f) {
            fprintf(stderr, "directpack: library, corpus or output unread\n");
            return 1;
        }
        id<MTLLibrary> own = nil, standIn = nil;
        if (const char *path = getenv("PROBE_OWN_LIB"); path && *path) {
            own = [libraries newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&error];
            dispatch_data_t data = dispatch_data_create(kTwoPlaneStandInLibrary, sizeof kTwoPlaneStandInLibrary,
                                                        nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            standIn = own ? [libraries newLibraryWithData:data error:&error] : nil;
            if (!own || !standIn) {
                fprintf(stderr, "directpack: own library: %s\n", error.localizedDescription.UTF8String);
                return 1;
            }
        }
        // every register raw_emit writes, over no baseline: directlink.py sets them over the context
        raw::PackRules rules;
        rules.varying = raw::emittableMethods();
        raw::Direct direct(std::move(rules), false, offline::translator());
        direct.makeVariants(true);
        std::map<uint64_t, raw::SelectorConfigs> selectors;
        if (argc == 5) {
            NSString *table = [NSString stringWithContentsOfFile:@(argv[4]) encoding:NSUTF8StringEncoding error:nil];
            std::string why;
            if (!table || !pack::readSelectorConfigs(table.UTF8String, selectors, why)) {
                fprintf(stderr, "directpack: selectors: %s\n", table ? why.c_str() : "unread table");
                return 1;
            }
            direct.selectorConfigs(&selectors);
        }
        direct.keepBoundDraws(true);
        Counts counts;
        std::set<std::string> reflected;
        probe::eachState(
            libraries, qc, own, standIn, probe::corpusLines(text, own != nil),
            [&](const std::string &line, bool ours, probe::State &s) {
                raw::ProbeTextures textures = probe::probeTextures(line, ours);
                textures.cubic = s.cubic;
                makeState(direct, w, qc, line, textures, s, counts);
                const std::string state = s.key.substr(0, raw::variantNameAt(s.key));
                if (reflected.insert(state).second) {
                    std::string blob, why;
                    const bool made = offline::renderReflection(s.d, blob, why);
                    reflection(w, false, state, made, blob, why, counts);
                }
            },
            [&](const std::string &line, const std::string &why) {
                head(w, "DREC", line, raw::CompileOutcome::Refused, why);
                counts.dropped++;
            });
        uint32_t kernels[3] = {}, kernelsDropped = 0;
        probe::eachKernel(
            libraries, text,
            [](const probe::Kernel &k, std::string &why) {
                return offline::kernelMade(k.descriptor, why);
            },
            [&](const probe::Kernel &k) {
                std::unique_ptr<raw::KernelDispatch> ask;
                probe::dispatchOf(k.shape, k.shape.threads, k.textures, ask);
                raw::ComputeRecord rec;
                std::string why;
                uint32_t facts[3] = {};
                const raw::CompileOutcome outcome = !offline::kernelFacts(k.descriptor, facts, why)
                                                        ? raw::CompileOutcome::Refused
                                                        : direct.kernel(k.descriptor, ask->dispatch(), k.key, rec, why);
                kernel(w, k.key, outcome, why, rec, facts);
                kernels[uint32_t(outcome)]++;
                if (reflected.insert(k.key.substr(0, k.key.find(" | tg="))).second) {
                    std::string blob, unmade;
                    const bool made = offline::computeReflection(k.descriptor, blob, unmade);
                    reflection(w, true, k.key, made, blob, unmade, counts);
                }
            },
            [&](const std::string &line, const std::string &why) {
                head(w, "DKRN", line, raw::CompileOutcome::Refused, why);
                kernelsDropped++;
            });
        fclose(w.f);
        fprintf(stderr,
                "directpack: %u states made (%u split, %u test variants, %u two-plane forms, %u refused, %u sampled "
                "two-plane variants, %u written-format variants), %u refused, %u failed, %u corpus lines making no "
                "state; %u kernels made, %u refused, %u failed, %u dispatch lines making no kernel\n",
                counts.made, counts.split, counts.variants, counts.planes, counts.planesRefused, counts.sampled,
                counts.written, counts.refused, counts.failed, counts.dropped, kernels[0], kernels[1], kernels[2],
                kernelsDropped);
        fprintf(stderr,
                "directpack: fetch-free forms: %u on rung 2, %u on rung 3 (%u reading other colours behind a barrier); "
                "%u still fetch, %u otherwise refused; %u stride twins\n",
                counts.free[2], counts.free[3], counts.freeBarrier, counts.freeFetching, counts.freeRefused,
                counts.twins);
        fprintf(stderr, "directpack: reflections: %u render (%u none), %u compute (%u none)\n", counts.reflected[0],
                counts.unreflected[0], counts.reflected[1], counts.unreflected[1]);
        return 0;
    }
}
