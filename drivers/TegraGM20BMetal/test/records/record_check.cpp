// SPDX-License-Identifier: MIT
// A pack's records against its kind's goldens (golden_files.h). With --sample: each sampled record section by section, so
// a moved record names what moved, and then a copy of the pack with one constant buffer type of a sampled record's
// fragment program changed must move that record (the check's negative control). Without: every record's digest, a moved
// one by its key. Run as a test:
//   record_check <pack.bin> <records_<kind>.bin> <records_<kind>_sample.tsv> <kind> [--sample]
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "drivers/TegraGM20BMetal/test/records/golden_files.h"

namespace {

using Golden = std::map<records::RecordId, records::SectionHashes>;

std::string slurp(const char *path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void regenerate(const char *kind)
{
    printf("records that moved on purpose: bazel run //drivers/TegraGM20BMetal/test/records:record_goldens with this "
           "test's --config (it builds //gpu/seed:raw_pack and rewrites testdata/records_%s*)\n",
           kind);
}

struct Moved {
    records::RecordId id;
    std::string label, section;  // section empty: the record is not in the pack
};

std::string text(const Moved &m)
{
    const std::string name = records::keyText(m.id.first) + " kind " + std::to_string(int(m.id.second));
    return m.section.empty() ? name + ": not in the pack" : name + " " + m.label + ": section " + m.section + " moved";
}

// Each sampled section the pack's records do not carry as the goldens do, and each sampled record it lacks
std::vector<Moved> movedSections(const std::vector<records::Record> &all, const Golden &golden)
{
    std::map<records::RecordId, const records::Record *> byId;
    for (const records::Record &r : all)
        byId[{r.key, r.kind}] = &r;
    std::vector<Moved> out;
    for (const auto &[id, sections] : golden) {
        const auto found = byId.find(id);
        if (found == byId.end()) {
            out.push_back({id, "", ""});
            continue;
        }
        const records::SectionHashes now = records::sectionHashes(*found->second);
        for (const auto &[section, hash] : sections) {
            const auto at = now.find(section);
            if (at == now.end() || at->second != hash)
                out.push_back({id, found->second->label.substr(0, 80), section});
        }
    }
    return out;
}

// The negative control: the pack's bytes with the last constant buffer type of a sampled record's fragment program
// changed, checked as the pack was. It must move that record's fs section.
bool mutantCaught(const char *packPath, const raw::Pack &pack, const Golden &golden)
{
    const raw::PackPipeline *p = pack.section<raw::PackPipeline>(raw::PackPipelines);
    const raw::PackProgram *programs = pack.section<raw::PackProgram>(raw::PackPrograms);
    for (uint32_t i = 0; i < pack.header()->pipelines; i++) {
        const raw::PackPipeline &pp = p[i];
        const records::RecordId id = {pp.keyHash, records::Kind(uint8_t(raw::recordForm(pp)))};
        if ((pp.flags & raw::PipelineVariantRecord) || pp.fs >= pack.header()->programs || !programs[pp.fs].cbufs ||
            !golden.count(id) || !golden.at(id).count("fs"))
            continue;
        const uint32_t slot = programs[pp.fs].cbufs - 1;
        std::string bytes = slurp(packPath);
        bytes[pack.header()->section[raw::PackPrograms].offset + pp.fs * sizeof(raw::PackProgram) +
              offsetof(raw::PackProgram, cbufType) + slot] ^= 1;
        const char *tmp = getenv("TEST_TMPDIR") ? getenv("TEST_TMPDIR") : getenv("TMPDIR");
        const std::string path = std::string(tmp ? tmp : ".") + "/record_check_mutant.bin";
        std::ofstream(path, std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));
        raw::Pack mutant;
        std::string err;
        if (!mutant.load(path.c_str(), 0, err)) {
            printf("negative control: %s: %s\n", path.c_str(), err.c_str());
            return false;
        }
        const auto moved = movedSections(records::records(mutant), golden);
        bool caught = false;
        for (const Moved &m : moved)
            caught |= m.id == id && m.section == "fs";
        printf("negative control: cbufType[%u] of %s kind %d's fragment program changed in a copy of the pack: %zu "
               "sampled sections moved, its fs %s\n",
               slot, records::keyText(id.first).c_str(), int(id.second), moved.size(),
               caught ? "among them" : "NOT among them");
        return caught;
    }
    printf("negative control: no sampled record has a fragment program with a constant buffer\n");
    return false;
}

int sample(const char *packPath, const raw::Pack &pack, const std::vector<records::Record> &all,
           const std::string &goldenText, const char *kind)
{
    Golden golden;
    if (!records::parseSample(goldenText, golden)) {
        printf("the sample file does not parse\n");
        return 1;
    }
    const auto moved = movedSections(all, golden);
    for (size_t i = 0; i < moved.size() && i < 40; i++)
        printf("%s\n", text(moved[i]).c_str());
    printf("%zu sampled records, %zu moved sections\n", golden.size(), moved.size());
    if (!moved.empty()) {
        regenerate(kind);
        return 1;
    }
    return mutantCaught(packPath, pack, golden) ? 0 : 1;
}

int corpus(const std::vector<records::Record> &all, const std::string &bytes, const char *kind)
{
    std::vector<records::Row> golden;
    if (!records::decode({bytes.begin(), bytes.end()}, golden)) {
        printf("the digest file is not whole rows\n");
        return 1;
    }
    const auto now = records::rows(all);
    std::map<records::RecordId, records::Digest> want, got;
    for (const auto &r : golden)
        want[{r.key, r.kind}] = r.digest;
    for (const auto &r : now)
        got[{r.key, r.kind}] = r.digest;
    int moved = 0, missing = 0, extra = 0;
    for (const auto &[id, d] : want) {
        const auto at = got.find(id);
        const bool gone = at == got.end();
        if (gone ? ++missing : at->second != d ? ++moved : 0) {
            if (moved + missing <= 40)
                printf("%s kind %d: %s\n", records::keyText(id.first).c_str(), int(id.second),
                       gone ? "not in the pack" : "moved");
        }
    }
    for (const auto &[id, d] : got)
        if (!want.count(id) && ++extra <= 10)
            printf("%s kind %d: not in the goldens\n", records::keyText(id.first).c_str(), int(id.second));
    printf("%zu records: %d moved, %d missing, %d new\n", want.size(), moved, missing, extra);
    if (moved || missing || extra) {
        regenerate(kind);
    }
    return moved || missing || extra ? 1 : 0;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: record_check <pack.bin> <records.bin> <sample.tsv> <kind> [--sample]\n");
        return 2;
    }
    raw::Pack pack;
    std::string err;
    if (!pack.load(argv[1], 0, err)) {
        printf("%s: %s\n", argv[1], err.c_str());
        return 1;
    }
    const auto all = records::records(pack);
    return argc > 5 && std::string(argv[5]) == "--sample" ? sample(argv[1], pack, all, slurp(argv[3]), argv[4])
                                                          : corpus(all, slurp(argv[2]), argv[4]);
}
