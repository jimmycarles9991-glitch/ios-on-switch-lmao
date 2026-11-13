// SPDX-License-Identifier: MIT
// The checked-in record goldens of one IPSW kind (testdata/records_<kind>.bin and records_<kind>_sample.tsv):
//  - every record's digest, as rows of {key 8 bytes, kind 1 byte, digest 16 bytes} (little-endian) in (key, kind) order;
//  - for a sample of the records, chosen once by a fixed hash of (key, kind) and kept by key, each section's sha256
//    (its first 16 bytes) as "<key>\t<kind>\t<section>\t<hash>" lines, so a moved record names the section that moved.
//    The sample is what a pack of only its states (the sample pack) holds the same: no state's blob (its registers
//    section stands for it) and of the pack's own records only the context init.
#pragma once
#include <map>
#include <string>
#include <vector>
#include "drivers/TegraGM20BMetal/test/records/record_digest.h"

namespace records {

struct Row {
    uint64_t key = 0;
    Kind kind = Kind::Default;
    Digest digest{};

    bool operator<(const Row &o) const
    {
        return key != o.key ? key < o.key : kind < o.kind;
    }
};

using RecordId = std::pair<uint64_t, Kind>;
using SectionHashes = std::map<std::string, std::string>;

constexpr size_t kRowBytes = 25;

std::vector<Row> rows(const std::vector<Record> &all);
std::vector<uint8_t> encode(const std::vector<Row> &rows);
bool decode(const std::vector<uint8_t> &bytes, std::vector<Row> &rows);

// About `target` records hashed in, then the 12 lowest-hashed of each kind, trait and fetch rung the pack has
std::vector<RecordId> pickSample(const std::vector<Record> &all, size_t target);
SectionHashes sectionHashes(const Record &r);
std::string sampleText(const std::vector<Record> &all, const std::vector<RecordId> &sample);
// The keyHash of each state the sample's pipeline records belong to, one per line: what sample_corpus.py picks the
// sample pack's corpus lines by
std::string sampleStates(const std::vector<Record> &all, const std::vector<RecordId> &sample);
bool parseSample(const std::string &text, std::map<RecordId, SectionHashes> &out);

std::string keyText(uint64_t key);

}  // namespace records
