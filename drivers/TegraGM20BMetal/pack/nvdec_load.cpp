// SPDX-License-Identifier: MIT
#include "nvdec.h"
#include "nvdec_methods.h"

#include <cstring>

namespace nvdec {

bool Decoder::loadLayouts(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        std::vector<std::string> w;
        for (char *tok = strtok(line, " \n"); tok; tok = strtok(nullptr, " \n"))
            w.push_back(tok);
        if (w.size() < 3)
            continue;
        Layout l;
        l.bytes = uint32_t(strtoul(w[2].c_str(), nullptr, 16));
        for (size_t i = 3; i < w.size(); i++) {
            const uint32_t off = uint32_t(strtoul(w[i].c_str(), nullptr, 10));
            l.ranges.push_back({off, w[i].back() == 'u' ? 16u : 4u});
            if (w[i].back() == 't')
                l.textureOnly.insert(off);
        }
        layouts[w[0] + " " + w[1]] = l;
    }
    fclose(f);
    return true;
}

bool Decoder::loadPrograms(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    std::vector<uint8_t> b;
    uint8_t chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        b.insert(b.end(), chunk, chunk + n);
    fclose(f);
    for (size_t at = 0; at + 16 <= b.size();) {
        uint32_t code, count, data;
        if (memcmp(&b[at], "NVPG", 4))
            return false;
        memcpy(&code, &b[at + 4], 4);
        memcpy(&count, &b[at + 8], 4);
        memcpy(&data, &b[at + 12], 4);
        at += 16;
        ProgramInfo p;
        p.cbufTypes.assign(b.begin() + at, b.begin() + at + count);
        at += count;
        p.image.assign(b.begin() + at, b.begin() + at + 80 + code);
        at += 80 + code;
        p.data.assign(b.begin() + at, b.begin() + at + data);
        at += data;
        p.id = hex(fnv(p.data.data(), p.data.size(), fnv(p.image.data(), p.image.size())));
        programs.emplace(fnv(p.image.data(), 80), p);
    }
    return true;
}

// The program a stage runs (pre-Volta: at SET_PIPELINE_PROGRAM bytes into SET_PROGRAM_REGION): a known one when its header,
// code and shader data match exactly, named by the hash of all three; else the hash of its header and first 256 bytes of code,
// marked unknown. The header sits 0x30 bytes into its upload and the data 256-aligned after the code (nvk_shader_upload).
const ProgramInfo *Decoder::program(int stage, std::string &id)
{
    const uint32_t offset = r3(PIPELINE + 0x40 * stage + 4);
    const uint64_t at = addr3(PROGRAM_REGION_A, PROGRAM_REGION_B) + offset;
    uint8_t head[80 + 256];
    if (!read(at, head, sizeof head)) {
        id = "unmapped";
        return nullptr;
    }
    auto match = [&]() -> const ProgramInfo * {
        auto range = programs.equal_range(fnv(head, 80));
        for (auto it = range.first; it != range.second; ++it) {
            const ProgramInfo &p = it->second;
            std::vector<uint8_t> b(p.image.size()), data(p.data.size());
            read(at, b.data(), b.size());
            const uint64_t dataAt = at - 0x30 + ((0x30 + b.size() + 0xff) & ~uint64_t(0xff));
            if (b == p.image && (data.empty() || (read(dataAt, data.data(), data.size()) && data == p.data)))
                return &p;
        }
        return nullptr;
    };
    const ProgramInfo *known = match();
    ProgramInfo learned;
    if (!known && programAt && programAt(offset, learned) && learned.image.size() >= 80) {
        learned.id =
            hex(fnv(learned.data.data(), learned.data.size(), fnv(learned.image.data(), learned.image.size())));
        programs.emplace(fnv(learned.image.data(), 80), learned);
        known = match();
    }
    if (known) {
        id = known->id;
        return known;
    }
    id = "unknown:" + hex(fnv(head, sizeof head));
    if (describe && getenv("NVDEC_DESCRIBE")) {
        fprintf(stderr, "program stage %d at ", stage);
        describe(at);
    }
    if (const char *dir =
            getenv("NVDEC_DUMPPROG")) {  // the unknown program's header and first 8 KiB, for a diff with known ones
        std::vector<uint8_t> b(80 + 8192);
        read(at, b.data(), b.size());
        const std::string path = std::string(dir) + "/" + id.substr(8) + ".bin";
        if (FILE *f = fopen(path.c_str(), "wb")) {
            fwrite(b.data(), 1, b.size(), f);
            fclose(f);
        }
    }
    return nullptr;
}

}  // namespace nvdec
