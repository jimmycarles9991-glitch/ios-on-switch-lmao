// SPDX-License-Identifier: MIT
// See golden_files.h.
#include "drivers/TegraGM20BMetal/test/records/golden_files.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>

namespace records {
namespace {

uint64_t mix(uint64_t key, Kind kind)
{
    const uint64_t words[2] = {key, uint64_t(kind)};
    return raw::Pack::fnv(words, sizeof words);
}

std::vector<uint32_t> classes(const Record &r)
{
    std::vector<uint32_t> out = {uint32_t(r.kind)};
    for (uint32_t bit = 0; bit < TraitRungShift; bit++)
        if (r.traits & (1u << bit))
            out.push_back(0x100 + bit);
    out.push_back(0x200 + (r.traits >> TraitRungShift & 15));
    return out;
}

}  // namespace

std::string keyText(uint64_t key)
{
    char text[17];
    snprintf(text, sizeof text, "%016" PRIx64, key);
    return text;
}

std::vector<Row> rows(const std::vector<Record> &all)
{
    std::vector<Row> out;
    for (const Record &r : all)
        out.push_back({r.key, r.kind, digest(r.sections)});
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<uint8_t> encode(const std::vector<Row> &rows)
{
    std::vector<uint8_t> out(rows.size() * kRowBytes);
    for (size_t i = 0; i < rows.size(); i++) {
        uint8_t *at = out.data() + i * kRowBytes;
        for (int b = 0; b < 8; b++)
            at[b] = uint8_t(rows[i].key >> (8 * b));
        at[8] = uint8_t(rows[i].kind);
        memcpy(at + 9, rows[i].digest.data(), rows[i].digest.size());
    }
    return out;
}

bool decode(const std::vector<uint8_t> &bytes, std::vector<Row> &rows)
{
    if (bytes.size() % kRowBytes)
        return false;
    rows.clear();
    for (size_t at = 0; at < bytes.size(); at += kRowBytes) {
        Row row;
        for (int b = 0; b < 8; b++)
            row.key |= uint64_t(bytes[at + b]) << (8 * b);
        row.kind = Kind(bytes[at + 8]);
        memcpy(row.digest.data(), bytes.data() + at + 9, row.digest.size());
        rows.push_back(row);
    }
    return true;
}

std::vector<RecordId> pickSample(const std::vector<Record> &all, size_t target)
{
    std::set<RecordId> picked;
    const uint64_t below = all.empty() ? 0 : ~uint64_t(0) / all.size() * std::min(target, all.size());
    std::map<uint32_t, std::vector<std::pair<uint64_t, RecordId>>> byClass;
    for (const Record &r : all) {
        if (r.kind == Kind::Pack && r.key != kInitRecord)
            continue;
        const uint64_t m = mix(r.key, r.kind);
        if (m < below)
            picked.insert({r.key, r.kind});
        for (uint32_t c : classes(r))
            byClass[c].push_back({m, {r.key, r.kind}});
    }
    for (auto &[c, members] : byClass) {
        std::sort(members.begin(), members.end());
        for (size_t i = 0; i < members.size() && i < 12; i++)
            picked.insert(members[i].second);
    }
    return {picked.begin(), picked.end()};
}

SectionHashes sectionHashes(const Record &r)
{
    SectionHashes out;
    for (const Section &s : r.sections) {
        const auto full = sha256(s.bytes);
        out[s.name] = hex(full.data(), 16);
    }
    return out;
}

std::string sampleText(const std::vector<Record> &all, const std::vector<RecordId> &sample)
{
    const std::set<RecordId> wanted(sample.begin(), sample.end());
    std::ostringstream out;
    for (const Record &r : all) {
        if (!wanted.count({r.key, r.kind}))
            continue;
        for (const auto &[name, hash] : sectionHashes(r))
            if (name != "blob")
                out << keyText(r.key) << '\t' << int(r.kind) << '\t' << name << '\t' << hash << '\n';
    }
    return out.str();
}

std::string sampleStates(const std::vector<Record> &all, const std::vector<RecordId> &sample)
{
    const std::set<RecordId> wanted(sample.begin(), sample.end());
    std::set<uint64_t> states;
    for (const Record &r : all)
        if (r.kind <= Kind::Variant && wanted.count({r.key, r.kind}))
            states.insert(r.state);
    std::string out;
    for (uint64_t s : states)
        out += keyText(s) + '\n';
    return out;
}

bool parseSample(const std::string &text, std::map<RecordId, SectionHashes> &out)
{
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string key, kind, name, hash;
        if (!std::getline(fields, key, '\t') || !std::getline(fields, kind, '\t') ||
            !std::getline(fields, name, '\t') || !std::getline(fields, hash))
            return false;
        out[{std::stoull(key, nullptr, 16), Kind(std::stoi(kind))}][name] = hash;
    }
    return true;
}

}  // namespace records
