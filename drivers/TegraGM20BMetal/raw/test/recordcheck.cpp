// SPDX-License-Identifier: MIT
// recordcheck: raw::makeRecord (the runtime's completion of a state the pack lacks) against the pack mkpack.py made from the same
// probe runs. Every state the pack holds a record for must come out byte for byte the same (its blob, set-0 entries, push words,
// vertex stream, set size, flags, key fields and both programs' heap slots, and no registers outside the pack's varying ones);
// every state it set aside must be refused, a storage-form state among them.
// usage: recordcheck <bucket dir (probe{256,16,4096}.{out,log}, programs.bin)> <pack.bin>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include "probe_logs.h"
#include "record_compare.h"

namespace {

using probelogs::Run;

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: recordcheck <bucket dir> <pack.bin>\n");
        return 2;
    }
    const std::string dir = argv[1];
    Run r256, r16, r4096, bound;
    raw::Pack pack;
    std::string err;
    if (!probelogs::loadRun(dir, 256, r256) || !probelogs::loadRun(dir, 16, r16) ||
        !probelogs::loadRun(dir, 4096, r4096) || !pack.load(argv[2], 0, err)) {
        fprintf(stderr, "recordcheck: %s\n", err.empty() ? "probe runs unread" : err.c_str());
        return 1;
    }
    const bool offsets =
        probelogs::loadRun(dir, "probeoff", bound);  // (a bucket probed before the bind-offset run has none)
    const std::map<std::string, raw::CapturedProgram> programs = probelogs::loadPrograms(dir + "/programs.bin");
    const raw::PackRules rules = raw::PackRules::of(pack, r256.context);
    auto byIndex = [](const Run &run, uint32_t index) -> const nvdec::ProbeSubmission * {
        for (size_t i = 0; i < run.probes.size(); i++)
            if (run.probes[i].first == index)
                return &run.submissions[i];
        return nullptr;
    };
    uint32_t same = 0, refused = 0, storage = 0, bad = 0;
    for (size_t i = 0; i < r256.probes.size(); i++) {
        const auto &[index, key] = r256.probes[i];
        const raw::ProbeRuns runs = {&r256.submissions[i],
                                     byIndex(r16, index),
                                     byIndex(r4096, index),
                                     index,
                                     key,
                                     offsets ? byIndex(bound, index) : nullptr};
        raw::RuntimeRecord made;
        std::string why;
        const bool ok = raw::makeRecord(
            runs, rules,
            [&](const std::string &id) {
                const auto it = programs.find(id);
                return it == programs.end() ? nullptr : &it->second;
            },
            made, why);
        const raw::PackPipeline *pp = pack.find(raw::packKeyHash(key));
        const bool barrier = pp && (pp->flags & raw::PipelineBarrierForm);
        std::string verdict;
        if (pp && !barrier) {
            verdict = ok ? recordcompare::differences(pack, *pp, made) : " refused (" + why + ")";
            same += verdict.empty();
        } else if (barrier) {
            verdict = !ok && why.rfind("storage form", 0) == 0 ? "" : " not refused as a storage form";
            storage += verdict.empty();
        } else {
            verdict = ok ? " made, and the pack set it aside" : "";
            refused += verdict.empty();
            if (!ok)
                printf("refused %u (%s)\n", index, why.c_str());
        }
        if (!verdict.empty()) {
            bad++;
            printf("DIFFERS %u:%s: %s\n", index, verdict.c_str(), key.substr(0, 120).c_str());
        }
    }
    printf("recordcheck %s: %u states: %u records the same as the pack's, %u storage forms refused, %u set aside and "
           "refused, "
           "%u differ\n",
           dir.c_str(), uint32_t(r256.probes.size()), same, storage, refused, bad);
    return bad ? 1 : 0;
}
