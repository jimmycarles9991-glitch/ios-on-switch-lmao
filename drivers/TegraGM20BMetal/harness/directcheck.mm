// SPDX-License-Identifier: MIT
// directcheck compares native translator/compiler records with a pack's corpus, including two-plane forms.
// usage: directcheck <default.metallib> <corpus.log> <pack.bin>
// DIRECTCHECK_MET appends record hashes; DIRECTCHECK_DUMP writes the JSON consumed by nakstat.py and rungcheck.py.
// DIRECTCHECK_SCAN: the whole pack scanned as the runtime trusts it (pack_scan.h), one "SCAN " line a finding.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <chrono>
#include <cstdio>
#include <set>
#include <sys/stat.h>
#include "../pack/direct_states.h"
#include "../pack/fetch_free_make.h"
#include "../TegraGM20BMetalDirect.h"
#include "../TegraGM20BMetalOffline.h"
#include "../pack_scan.h"
#include "TwoPlaneStandIn.h"
#include "record_compare.h"

namespace {

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

void dumpRecord(const char *dir, const std::string &key, const gm20b::RuntimeRecord &rec,
                const probe::FetchFree *free = nullptr)
{
    char name[32];
    snprintf(name, sizeof name, "/%016llx.json", (unsigned long long)gm20b::packKeyHash(key));
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
    const gm20b::SetEntries set = gm20b::entriesOf(rec);
    std::string residuals;
    for (const gm20b::PackUniformRange &u : rec.pipeline.uniform)
        if (u.entry < set.size() && gm20b::bufferResidualWord(set[u.entry]) >= 0)
            residuals += std::string(residuals.empty() ? "" : ", ") +
                         std::to_string(0x700 + 4 * gm20b::bufferResidualWord(set[u.entry]));
    std::string uniform, entries;
    for (const gm20b::PackUniformRange &u : rec.pipeline.uniform)
        uniform += std::string(uniform.empty() ? "" : ", ") + "[" + std::to_string(u.entry) + ", " +
                   std::to_string(u.bank) + ", " + std::to_string(u.first) + ", " + std::to_string(u.end) + "]";
    for (uint32_t k = 0; k < gm20b::entryCount(rec.pipeline); k++) {
        const gm20b::PackEntry &e = set[k];
        entries += std::string(k ? ", " : "") + "[" + std::to_string(e.kind) + ", " + std::to_string(e.stage) + ", " +
                   std::to_string(e.slot) + ", " + std::to_string(e.samplerKind) + ", " + std::to_string(e.offset) +
                   "]";
    }
    std::string setBanks;
    for (size_t b = 0; b < rec.programs[1].cbufTypes.size(); b++)
        if (gm20b::cbufKind(rec.programs[1].cbufTypes[b]) == gm20b::CbufSet)
            setBanks += std::string(setBanks.empty() ? "" : ", ") + std::to_string(b);
    std::string cbufs;
    for (size_t b = 0; b < rec.programs[1].cbufTypes.size(); b++)
        cbufs += std::string(b ? ", " : "") + std::to_string(rec.programs[1].cbufTypes[b]);
    std::string variants;
    for (size_t i = 0; i < rec.variants.size(); i++) {
        const gm20b::RuntimeRecord::Variant &v = rec.variants[i];
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
            "\"fs_cbufs\": [%s], \"fs_data\": \"%s\"",
            jsonString(key).c_str(), hexOf(rec.programs[0].image).c_str(), hexOf(rec.programs[1].image).c_str(),
            rec.programs[1].gprs, rec.programs[1].boundTextures, blob.c_str(),
            rec.split ? hexOf(rec.splitPrograms[0].image).c_str() : "",
            rec.split ? hexOf(rec.splitPrograms[1].image).c_str() : "", residuals.c_str(), variants.c_str(),
            rec.pipeline.storageMask, rec.pipeline.storageBlendMask, rec.pipeline.storageBlendModel,
            rec.pipeline.halfBlendColorWord, free ? free->rung : 0, free ? free->colour : 0,
            jsonString(free ? free->program : "").c_str(), uniform.c_str(), entries.c_str(), setBanks.c_str(),
            rec.pipeline.storageWriteMask, cbufs.c_str(), hexOf(rec.programs[1].data).c_str());
    fprintf(f, "}\n");
    fclose(f);
}

// The state's fetch-free form, made as directpack.mm makes it (fetch_free_make.h)
void dumpFetchFree(gm20b::Direct &direct, const probe::State &s, const std::string &line, const std::string &key,
                   gm20b::ProbeTextures textures, const char *dir)
{
    probe::FetchFree form;
    gm20b::RuntimeRecord rec;
    std::string why;
    bool barrier = false;
    if (probe::makeFetchFree(direct, s, line, textures, form, rec, nullptr, why, barrier) == probe::FreeOutcome::Made)
        dumpRecord(dir, key, rec, &form);
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc != 4) {
            fprintf(stderr, "usage: directcheck <default.metallib> <corpus.log> <pack.bin>\n");
            return 2;
        }
        id<MTLDevice> dev = (id<MTLDevice>)[GM20BOfflineDevice new];
        gm20b::Pack pack;
        std::string err;
        if (!pack.load(argv[3], 0, err)) {
            fprintf(stderr, "directcheck: %s\n", err.c_str());
            return 1;
        }
        uint32_t scanned = 0;
        if (getenv("DIRECTCHECK_SCAN"))
            for (const std::string &line : gm20b::packScan(pack)) {
                printf("SCAN %s\n", line.c_str());
                scanned++;
            }
        NSError *error = nil;
        id<MTLLibrary> qc = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
        NSString *text = [NSString stringWithContentsOfFile:@(argv[2]) encoding:NSUTF8StringEncoding error:nil];
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
        gm20b::Direct direct(gm20b::PackRules::of(pack), false, offline::translator());
        direct.makeVariants(true);
        FILE *met = getenv("DIRECTCHECK_MET") ? fopen(getenv("DIRECTCHECK_MET"), "a") : nullptr;
        uint32_t states = 0, same = 0, barriers = 0, refused = 0, bad = 0, splits = 0;
        double ms = 0;
        std::map<std::string, uint32_t> differences, reasons;
        // DIRECTCHECK_BARRIER=1: each state's barrier form against the pack's
        const bool barrierForm = getenv("DIRECTCHECK_BARRIER") && *getenv("DIRECTCHECK_BARRIER") == '1';
        // One record of a state, `key` its pack key with the variant names directpack.mm gives it
        const auto check = [&](const std::string &line, const probe::State &s, const std::string &key,
                               const gm20b::ProbeTextures &textures) {
            states++;
            const size_t named = gm20b::variantNameAt(key);
            const uint64_t keyHash = gm20b::packKeyHash(key.substr(0, named));
            const gm20b::PackForm requested = barrierForm ? gm20b::PackForm::Barrier : gm20b::PackForm::Default;
            const gm20b::PackPipeline *pp = named < key.size()
                                                ? pack.variantRecord(keyHash, key.substr(named), requested)
                                                : pack.find(keyHash, requested);
            if (!pp && named < key.size() && requested == gm20b::PackForm::Default)
                pp = pack.variantRecord(keyHash, key.substr(named), gm20b::PackForm::Barrier);
            const bool barrier = pp && (pp->flags & gm20b::PipelineBarrierForm);
            gm20b::RuntimeRecord rec;
            std::string why;
            const auto began = std::chrono::steady_clock::now();
            const gm20b::CompileOutcome outcome = direct.make(s.d, key, rec, why, textures, nullptr, true, requested);
            if (pp && named < key.size()) {
                rec.pipeline.keyHash = keyHash;
                rec.pipeline.flags |= gm20b::PipelineVariantRecord;
            }
            ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
            if (outcome == gm20b::CompileOutcome::Made && !direct.storageRefusal().empty())
                printf("BARRIER FORM (%s): %s\n", direct.storageRefusal().c_str(), key.substr(0, 160).c_str());
            if (const char *dir = getenv("DIRECTCHECK_DUMP"); dir && *dir && outcome == gm20b::CompileOutcome::Made) {
                dumpRecord(dir, key, rec);
                dumpFetchFree(direct, s, line, key, textures, dir);
            }
            std::string verdict;
            if (outcome == gm20b::CompileOutcome::Failed) {
                verdict = " failed (" + why + ")";
            } else if (pp) {
                verdict = outcome == gm20b::CompileOutcome::Made
                              ? recordcompare::differences(pack, *pp, rec) +
                                    recordcompare::splitDifferences(pack, *pp, rec, direct.splittable())
                              : " refused (" + why + ")";
                splits += verdict.empty() && rec.split;
                (barrier ? barriers : same) += verdict.empty();
                if (met && verdict.empty())
                    fprintf(met, "%016llx %u\n", (unsigned long long)pp->keyHash,
                            pp->flags & gm20b::PipelineBarrierForm);
            } else {
                verdict = outcome == gm20b::CompileOutcome::Made ? " made, and the pack set it aside" : "";
                refused += verdict.empty();
                if (verdict.empty()) {
                    reasons[why.substr(0, 90)]++;
                    printf("REFUSED (%s): %s\n", why.c_str(), s.key.substr(0, 160).c_str());
                }
            }
            if (!verdict.empty()) {
                bad++;
                differences[verdict.substr(0, verdict.find('['))]++;
                printf("DIFFERS%s%s: %s\n", barrier ? " (barrier)" : "", verdict.c_str(), s.key.substr(0, 160).c_str());
            }
            return outcome;
        };
        probe::eachState(
            dev, qc, own, standIn, probe::corpusLines(text, own != nil),
            [&](const std::string &line, bool ours, probe::State &s) {
                gm20b::ProbeTextures textures = probe::probeTextures(line, ours);
                textures.cubic = s.cubic;
                // DIRECTCHECK_TESTS=<n>: each state's variant beside depth or stencil tests of class n
                // (fragment_tests.h FragmentTests), the one a fetch without a storage form takes,
                // against the pack's record of it (PipelineVariantRecord)
                std::string tests;
                if (const char *n = getenv("DIRECTCHECK_TESTS"); n && *n) {
                    textures.fragmentTests = uint8_t(atoi(n));
                    tests = " | tests=" + std::to_string(textures.fragmentTests);
                }
                textures.xrSlots = s.xrSlots;
                textures.written = s.written;
                const std::string sampled =
                    (s.xrSlots ? gm20b::xrSampledVariant(s.xrSlots) : std::string()) + gm20b::writtenVariant(s.written);
                check(line, s, s.key + tests + sampled, textures);
            },
            [&](const std::string &line, const std::string &why) {
                printf("NO STATE (%s): %s\n", why.c_str(), line.substr(0, 160).c_str());
                reasons["no state: " + why]++;
            });
        if (met)
            fclose(met);
        for (const auto &[what, n] : differences)
            printf("  %u x%s\n", n, what.c_str());
        for (const auto &[why, n] : reasons)
            printf("  %u x refused: %s\n", n, why.c_str());
        printf("directcheck: %u states made directly: %u records the same as the pack's, %u the same as its barrier "
               "forms, %u set aside and refused, %u differ; %u with the pack's split; %.1f ms a state\n",
               states, same, barriers, refused, bad, splits, states ? ms / states : 0.0);
        return bad || scanned || !states ? 1 : 0;
    }
}
