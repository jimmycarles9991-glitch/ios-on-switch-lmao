// SPDX-License-Identifier: MIT
// directcheck: the direct compile (runtime/raw_direct.h) of every state of a corpus against the pack. Each state's record is made
// from m2v's translation and plans and NVK's stage compile, with no pipeline, command buffer or capture: every state the pack holds
// a record for must come out byte for byte the same (its barrier form made as on a device without the pixel interlock), with the
// pack's split of its fragment program, every one it set aside refused. A state of RGB10A8_2P_XR10 colours is held to the pack in
// its two-plane form too (" | xr2p", raw_xr_pass.h), as directpack.mm makes it. The states are pack/probe.mm's: Core Animation's
// lines, another framework's (lib=, under PROBE_LIB_ROOT) and, with PROBE_OWN_LIB, the raw path's own.
// usage: directcheck <nvk dylib> <cache dir> <default.metallib> <corpus.log> <pack.bin>
//   DIRECTCHECK_MET=<file>: appends the key hash and form of each pack record made the same (a sweep over every bucket checks
//   that it met every record of the pack)
//   DIRECTCHECK_DUMP=<dir>: writes each record made to <dir>/<key hash>.json (its key, programs and blob), what
//   tools/gpu/maxwell/nakstat.py --dump reads, and a state's fetch-free form (pack/fetch_free.h) to <dir>/free/<key hash>.json
//   with its rung, colour and iterated blend program (tools/gpu/maxwell/rungcheck.py)
// Each state the pack set aside that the direct compile refuses too is listed with its reason (REFUSED lines): run over a
// device's harvest against the pack it booted with, they are the states its runtime refused. A state made in the barrier
// form because its storage form was refused is listed with that reason (BARRIER FORM lines).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <chrono>
#include <cstdio>
#include <set>
#include <sys/stat.h>
#include "../pack/direct_states.h"
#include "../pack/fetch_free_make.h"
#include "../runtime/RawDirect.h"
#include "NativeBridgeMetalFrontend.h"
#include "TwoPlaneStandIn.h"
#include "bridge/NativeMetalObjects.h"
#include "record_compare.h"

namespace {

void missing(const char *cls, const char *sel)
{
    fprintf(stderr, "MISSING -[%s %s]\n", cls, sel);
}

std::string hexOf(const std::vector<uint8_t> &bytes)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

std::string jsonString(const std::string &text)
{
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

void dumpRecord(const char *dir, const std::string &key, const raw::RuntimeRecord &rec,
                const probe::FetchFree *free = nullptr)
{
    char name[32];
    snprintf(name, sizeof name, "/%016llx.json", (unsigned long long)raw::packKeyHash(key));
    const std::string folder = std::string(dir) + (free ? "/free" : "");
    if (free)
        mkdir(folder.c_str(), 0755);
    const std::string path = folder + name;
    FILE *f = fopen(path.c_str(), "w");
    if (!f)
        return;
    std::string blob;
    for (size_t i = 0; i < rec.blob.size(); i++)
        blob += (i ? "," : "") + std::to_string(rec.blob[i]);
    std::string residuals;
    for (const raw::PackUniformRange &u : rec.pipeline.uniform)
        if (u.entry < rec.pipeline.entries && raw::bufferResidualWord(rec.pipeline.entry[u.entry]) >= 0)
            residuals += std::string(residuals.empty() ? "" : ", ") +
                         std::to_string(0x700 + 4 * raw::bufferResidualWord(rec.pipeline.entry[u.entry]));
    std::string uniform, entries;
    for (const raw::PackUniformRange &u : rec.pipeline.uniform)
        uniform += std::string(uniform.empty() ? "" : ", ") + "[" + std::to_string(u.entry) + ", " +
                   std::to_string(u.bank) + ", " + std::to_string(u.first) + ", " + std::to_string(u.end) + "]";
    for (uint32_t k = 0; k < rec.pipeline.entries; k++) {
        const raw::PackEntry &e = rec.pipeline.entry[k];
        entries += std::string(k ? ", " : "") + "[" + std::to_string(e.kind) + ", " + std::to_string(e.stage) + ", " +
                   std::to_string(e.slot) + ", " + std::to_string(e.samplerKind) + ", " + std::to_string(e.offset) +
                   "]";
    }
    std::string setBanks;
    for (size_t b = 0; b < rec.programs[1].cbufTypes.size(); b++)
        if (raw::cbufKind(rec.programs[1].cbufTypes[b]) == raw::CbufSet)
            setBanks += std::string(setBanks.empty() ? "" : ", ") + std::to_string(b);
    std::string cbufs;
    for (size_t b = 0; b < rec.programs[1].cbufTypes.size(); b++)
        cbufs += std::string(b ? ", " : "") + std::to_string(rec.programs[1].cbufTypes[b]);
    std::string variants;
    for (size_t i = 0; i < rec.variants.size(); i++) {
        const raw::RuntimeRecord::Variant &v = rec.variants[i];
        variants += std::string(i ? ", " : "") + "{\"stage\": " + (i < rec.pipeline.variantCount[0] ? "0" : "1") +
                    ", \"assume\": " + std::to_string(v.assume) + ", \"gprs\": " + std::to_string(v.program.gprs) +
                    ", \"bound_textures\": " + std::to_string(v.program.boundTextures) + ", \"code\": \"" +
                    hexOf(v.program.image) + "\"}";
    }
    fprintf(f,
            "{\"key\": %s, \"vs\": \"%s\", \"fs\": \"%s\", \"fs_gprs\": %u, \"fs_bound_textures\": %u, \"blob\": [%s], "
            "\"split_fs\": \"%s\", \"split_prologue\": \"%s\", \"residual_words\": [%s], \"variants\": [%s], "
            "\"storage_mask\": %u, \"storage_blend_mask\": %u, \"storage_blend_model\": %u, "
            "\"half_blend_color_word\": %u, \"free_rung\": %d, \"free_colour\": %u, \"free_program\": %s, "
            "\"uniform\": [%s], \"entries\": [%s], \"fs_set_banks\": [%s], \"storage_write_mask\": %u, "
            "\"fs_cbufs\": [%s], \"fs_data\": \"%s\"}\n",
            jsonString(key).c_str(), hexOf(rec.programs[0].image).c_str(), hexOf(rec.programs[1].image).c_str(),
            rec.programs[1].gprs, rec.programs[1].boundTextures, blob.c_str(),
            rec.split ? hexOf(rec.splitPrograms[0].image).c_str() : "",
            rec.split ? hexOf(rec.splitPrograms[1].image).c_str() : "", residuals.c_str(), variants.c_str(),
            rec.pipeline.storageMask, rec.pipeline.storageBlendMask, rec.pipeline.storageBlendModel,
            rec.pipeline.halfBlendColorWord, free ? free->rung : 0, free ? free->colour : 0,
            jsonString(free ? free->program : "").c_str(), uniform.c_str(), entries.c_str(), setBanks.c_str(),
            rec.pipeline.storageWriteMask, cbufs.c_str(), hexOf(rec.programs[1].data).c_str());
    fclose(f);
}

// The state's fetch-free form, made as directpack.mm makes it (fetch_free_make.h)
void dumpFetchFree(raw::Direct &direct, id<MTLLibrary> qc, const std::string &line, const std::string &key,
                   raw::ProbeTextures textures, const char *dir)
{
    probe::FetchFree form;
    raw::RuntimeRecord rec;
    std::string why;
    bool barrier = false;
    if (probe::makeFetchFree(direct, qc, line, textures, form, rec, nullptr, why, barrier) == probe::FreeOutcome::Made)
        dumpRecord(dir, key, rec, &form);
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 6) {
            fprintf(stderr, "usage: directcheck <nvk dylib> <cache dir> <default.metallib> <corpus.log> <pack.bin>\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> dev = native_bridge_metal_open(argv[1], argv[2], 0, missing);
        raw::Pack pack;
        std::string err;
        if (!dev || !pack.load(argv[5], 0, err)) {
            fprintf(stderr, "directcheck: %s\n", dev ? err.c_str() : nmb_last_error());
            return 1;
        }
        NSError *error = nil;
        id<MTLLibrary> qc = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error];
        NSString *text = [NSString stringWithContentsOfFile:@(argv[4]) encoding:NSUTF8StringEncoding error:nil];
        if (!qc || !text) {
            fprintf(stderr, "directcheck: library or corpus unread\n");
            return 1;
        }
        id<MTLLibrary> own = nil, standIn = nil;
        if (const char *path = getenv("PROBE_OWN_LIB"); path && *path) {
            own = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&error];
            dispatch_data_t data = dispatch_data_create(kTwoPlaneStandInLibrary, sizeof kTwoPlaneStandInLibrary,
                                                        nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            standIn = own ? [dev newLibraryWithData:data error:&error] : nil;
            if (!own || !standIn) {
                fprintf(stderr, "directcheck: own library: %s\n", error.localizedDescription.UTF8String);
                return 1;
            }
        }
        raw::Direct direct(pack);
        direct.makeVariants(true);
        FILE *met = getenv("DIRECTCHECK_MET") ? fopen(getenv("DIRECTCHECK_MET"), "a") : nullptr;
        uint32_t states = 0, same = 0, barriers = 0, refused = 0, bad = 0, splits = 0, planes = 0;
        double ms = 0;
        std::map<std::string, uint32_t> differences, reasons;
        // DIRECTCHECK_BARRIER=1: each state's barrier form against the pack's
        const bool barrierForm = getenv("DIRECTCHECK_BARRIER") && *getenv("DIRECTCHECK_BARRIER") == '1';
        // One record of a state, `key` its pack key with the variant names directpack.mm gives it
        const auto check = [&](const std::string &line, const probe::State &s, const std::string &key,
                               const raw::ProbeTextures &textures) {
            states++;
            planes += textures.twoPlane;
            const size_t named = raw::variantNameAt(key);
            const uint64_t keyHash = raw::packKeyHash(key.substr(0, named));
            const raw::PackPipeline *pp = named < key.size() ? pack.variantRecord(keyHash, key.substr(named))
                                          : barrierForm      ? pack.find(keyHash, raw::PackForm::Barrier)
                                                             : pack.find(keyHash);
            const bool barrier = pp && (pp->flags & raw::PipelineBarrierForm);
            raw::RuntimeRecord rec;
            std::string why;
            const auto began = std::chrono::steady_clock::now();
            const raw::CompileOutcome outcome =
                direct.make(s.d, key, rec, why, textures, nullptr, true,
                            barrierForm ? raw::PackForm::Barrier : raw::PackForm::Default);
            if (pp && named < key.size()) {
                rec.pipeline.keyHash = keyHash;
                rec.pipeline.flags |= raw::PipelineVariantRecord;
            }
            ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
            if (outcome == raw::CompileOutcome::Made && !direct.storageRefusal().empty())
                printf("BARRIER FORM (%s): %s\n", direct.storageRefusal().c_str(), key.substr(0, 160).c_str());
            if (const char *dir = getenv("DIRECTCHECK_DUMP");
                dir && *dir && outcome == raw::CompileOutcome::Made && !textures.twoPlane) {
                dumpRecord(dir, key, rec);
                dumpFetchFree(direct, qc, line, key, textures, dir);
            }
            std::string verdict;
            if (outcome == raw::CompileOutcome::Failed) {
                verdict = " failed (" + why + ")";
            } else if (pp) {
                verdict = outcome == raw::CompileOutcome::Made ? recordcompare::differences(pack, *pp, rec) +
                                                                     recordcompare::splitDifferences(pack, *pp, rec)
                                                               : " refused (" + why + ")";
                splits += verdict.empty() && rec.split;
                (barrier ? barriers : same) += verdict.empty();
                if (met && verdict.empty())
                    fprintf(met, "%016llx %u\n", (unsigned long long)pp->keyHash, pp->flags & raw::PipelineBarrierForm);
            } else {
                verdict = outcome == raw::CompileOutcome::Made ? " made, and the pack set it aside" : "";
                refused += verdict.empty();
                if (verdict.empty()) {
                    reasons[why.substr(0, 90)]++;
                    printf("REFUSED (%s): %s\n", why.c_str(), s.key.substr(0, 160).c_str());
                }
            }
            if (!verdict.empty()) {
                bad++;
                differences[verdict.substr(0, verdict.find('['))]++;
                printf("DIFFERS%s%s%s: %s\n", barrier ? " (barrier)" : "", textures.twoPlane ? " (two-plane)" : "",
                       verdict.c_str(), s.key.substr(0, 160).c_str());
            }
            return outcome;
        };
        probe::eachState(dev, qc, own, standIn, probe::corpusLines(text, own != nil),
                         [&](const std::string &line, bool ours, probe::State &s) {
                             raw::ProbeTextures textures = probe::probeTextures(line, ours);
                             textures.cubic = s.cubic;
                             // DIRECTCHECK_TESTS=<n>: each state's variant beside depth or stencil tests of class n
                             // (raw_fragment_tests.h FragmentTests), the one a fetch without a storage form takes,
                             // against the pack's record of it (PipelineVariantRecord)
                             std::string tests;
                             if (const char *n = getenv("DIRECTCHECK_TESTS"); n && *n) {
                                 textures.fragmentTests = uint8_t(atoi(n));
                                 tests = " | tests=" + std::to_string(textures.fragmentTests);
                             }
                             textures.xrSlots = s.xrSlots;
                             textures.written = s.written;
                             const std::string sampled =
                                 (s.xrSlots ? raw::xrSampledVariant(s.xrSlots) : std::string()) +
                                 raw::writtenVariant(s.written);
                             if (check(line, s, s.key + tests + sampled, textures) != raw::CompileOutcome::Made ||
                                 !probe::twoPlaneColors(s.key))
                                 return;
                             textures.twoPlane = true;
                             check(line, s, s.key + tests + raw::kXrPlanesVariant + sampled, textures);
                         });
        if (met)
            fclose(met);
        for (const auto &[what, n] : differences)
            printf("  %u x%s\n", n, what.c_str());
        for (const auto &[why, n] : reasons)
            printf("  %u x refused: %s\n", n, why.c_str());
        printf("directcheck: %u states made directly: %u records the same as the pack's, %u the same as its barrier "
               "forms, "
               "%u set aside and refused, %u differ; %u with the pack's split; %.1f ms a state; %u of them two-plane "
               "forms\n",
               states, same, barriers, refused, bad, splits, states ? ms / states : 0.0, planes);
        return bad || !states ? 1 : 0;
    }
}
