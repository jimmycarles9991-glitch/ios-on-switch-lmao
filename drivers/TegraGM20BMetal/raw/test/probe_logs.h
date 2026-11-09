// SPDX-License-Identifier: MIT
// Host tests: a bucket's probe runs (pack/rawpack.py's probe{256,16,4096}.{out,log}) as nvdec::ProbeSubmission per state, read as
// pack/probelog.py reads them, and mkprograms.py's programs.bin by program id.
#pragma once
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "../runtime/raw_pack.h"
#include "../pack/probe_record.h"
#include "../runtime/raw_record.h"

namespace probelogs {

struct Run {
    std::vector<std::pair<uint32_t, std::string>> probes;  // PROBE <index> <key>
    std::vector<nvdec::ProbeSubmission> submissions;       // the last ones: a probe's each
    std::map<uint32_t, uint32_t> context;                  // S regs 0
};

inline std::vector<std::string> fields(const std::string &line)
{
    std::istringstream in(line);
    std::vector<std::string> out;
    for (std::string f; in >> f;)
        out.push_back(f);
    return out;
}

inline uint32_t hex(const std::string &s)
{
    return uint32_t(strtoull(s.c_str(), nullptr, 16));
}

inline std::vector<uint32_t> words(const std::vector<std::string> &f, size_t from)
{
    std::vector<uint32_t> out;
    for (size_t i = from; i < f.size(); i++)
        out.push_back(hex(f[i]));
    return out;
}

inline std::map<uint32_t, uint32_t> pairs(const std::vector<std::string> &f, size_t from)
{
    std::map<uint32_t, uint32_t> out;
    for (size_t i = from; i < f.size(); i++)
        if (const size_t eq = f[i].find('='); eq != std::string::npos)
            out[hex(f[i].substr(0, eq))] = hex(f[i].substr(eq + 1));
    return out;
}

// probelog.py _record_binding and _record_state
inline void recordLine(nvdec::ProbeRecord &r, const std::vector<std::string> &f)
{
    const std::string &t = f[1];
    if (t == "regs")
        r.regs = pairs(f, 2);
    else if (t == "root")
        r.root.words = words(f, 3);
    else if (t == "set0")
        r.sets[0].words = words(f, 3);
    else if (t.rfind("ptr0.", 0) == 0)
        r.setPointers[0][uint32_t(atoi(t.c_str() + 5))].words = words(f, 3);
    else if (t == "tic") {
        nvdec::ProbeRecord::Header &h = r.tic[uint32_t(atoi(f[2].c_str()))];
        h.name = f[3];
        for (size_t i = 0; i < 8; i++)
            h.w[i] = hex(f[4 + i]);
    } else if (t == "tsc") {
        std::array<uint32_t, 8> &w = r.tsc[uint32_t(atoi(f[2].c_str()))];
        for (size_t i = 0; i < 8; i++)
            w[i] = hex(f[3 + i]);
    } else if (t == "vbinfo")
        r.streams[uint32_t(atoi(f[2].c_str()))].format = hex(f[5]);
    else if (t.rfind("vb", 0) == 0)
        r.streams[uint32_t(atoi(t.c_str() + 2))].words = words(f, 3);
    else if (t == "prog")
        r.programs[atoi(f[2].c_str())] = {f[3], hex(f[4]), f[5] == "known"};
    else if (t == "pushw")
        for (const auto &kv : pairs(f, 2))
            r.pushWords.push_back(kv);
}

// `name`: probe256, probe16, probe4096, probeoff (the bind-offset run)
inline bool loadRun(const std::string &dir, const std::string &name, Run &run)
{
    std::ifstream out(dir + "/" + name + ".out"), log(dir + "/" + name + ".log");
    if (!out || !log)
        return false;
    for (std::string line; std::getline(out, line);)
        if (line.rfind("PROBE ", 0) == 0) {
            const size_t a = line.find(' ', 6);
            std::string key = line.substr(a + 1);
            while (!key.empty() && key.back() == ' ')
                key.pop_back();
            run.probes.push_back({uint32_t(atoi(line.c_str() + 6)), key});
        }
    std::vector<nvdec::ProbeSubmission> subs(1);
    nvdec::ProbeRecord *rec = nullptr;
    bool held = false;  // the submission has a line: one with none is m2v's empty one, not a probe's (probelog.py)
    for (std::string line; std::getline(log, line);) {
        nvdec::ProbeSubmission &s = subs.back();
        if (line.rfind("SUBMIT", 0) == 0) {
            if (held)
                subs.emplace_back();
            held = false;
            continue;
        }
        held = true;
        if (line.rfind("S regs 0 ", 0) == 0)
            run.context = pairs(fields(line), 3);
        else if (line.rfind("DRAW", 0) == 0)
            s.draws++;
        else if (line.rfind("DISPATCH", 0) == 0)
            s.dispatches++;
        else if (line.rfind("CLEAR", 0) == 0)
            s.clears++;
        else if (line.rfind("  rt", 0) == 0 && line.find('=') != std::string::npos) {
            const size_t eq = line.find('=');
            s.targets[fields(line.substr(eq + 1))[0]] = uint32_t(atoi(line.c_str() + 4));
        } else if (line == "P begin") {
            s.records.emplace_back();
            rec = &s.records.back();
        } else if (line == "P end")
            rec = nullptr;
        else if (rec && line.rfind("P ", 0) == 0)
            recordLine(*rec, fields(line));
    }
    subs.pop_back();
    if (subs.size() < run.probes.size())
        return false;
    run.submissions.assign(subs.end() - run.probes.size(), subs.end());
    return true;
}

inline bool loadRun(const std::string &dir, int bytes, Run &run)
{
    return loadRun(dir, "probe" + std::to_string(bytes), run);
}

// mkprograms.py's programs.bin, by the id nvdec names a bound one with
inline std::map<std::string, raw::CapturedProgram> loadPrograms(const std::string &path)
{
    std::map<std::string, raw::CapturedProgram> out;
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (size_t at = 0; at + 16 <= b.size();) {
        uint32_t code, count, data;
        memcpy(&code, &b[at + 4], 4);
        memcpy(&count, &b[at + 8], 4);
        memcpy(&data, &b[at + 12], 4);
        at += 16;
        raw::CapturedProgram p;
        p.cbufTypes.assign(b.begin() + long(at), b.begin() + long(at + count));
        at += count;
        p.image.assign(b.begin() + long(at), b.begin() + long(at + 80 + code));
        p.data.assign(b.begin() + long(at + 80 + code), b.begin() + long(at + 80 + code + data));
        char id[24];
        snprintf(id, sizeof id, "%llx", (unsigned long long)raw::Pack::fnv(&b[at], 80 + code + data));
        p.id = id;
        at += 80 + code + data;
        out[p.id] = p;
    }
    return out;
}

}  // namespace probelogs
