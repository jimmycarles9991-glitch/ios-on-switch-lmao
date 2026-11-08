// SPDX-License-Identifier: MIT
// nvdec (r7-raw): a GM20B push-stream executor for equivalence checks. It runs a submission's method stream the way the
// GPU front end would (per-subchannel method registers, Fermi MME macros through Mesa's simulator, LOAD_CONSTANT_BUFFER writes
// into constant-buffer memory, constant-buffer binds) and prints one canonical record per draw, clear and barrier.
//
// Canonical means: addresses never appear. Memory the GPU reads per draw (root table, descriptor sets, uniform payloads, vertex
// data) is printed as a hash of its content, with any GPU pointer inside replaced by the hash of what it points to; persistent
// resources (render targets, texture images) get names in order of first appearance. Two streams that make the GPU draw the same
// thing print the same records, whatever addresses, method order or redundant writes each used.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "nvdec_comp.h"
#include "probe_record.h"

namespace nvdec {

// Resolves [va, va + bytes) of the channel's VM to host memory as the CPU left it at submit time, or null.
using MapFn = std::function<const uint8_t *(uint64_t va, uint64_t bytes)>;

// A known program (from the NVK driver records, tools/mkprograms.py): its 80-byte shader header and code, and what its constant
// buffer slots are (nvk_cbuf_type numbering: 1 root table, 2 shader data, 3 descriptor set, 4 dynamic UBO, 5 UBO descriptor).
struct ProgramInfo {
    std::string id;              // the name a bound one gets: the FNV-1a of its image and data
    std::vector<uint8_t> image;  // header + code, as uploaded to the shader heap
    std::vector<uint8_t> cbufTypes;
    std::vector<uint8_t> data;  // NAK's shader data, uploaded 256-aligned after the code (NVK nvk_shader_upload)
};

class Decoder {
public:
    Decoder(MapFn map, FILE *out);
    void submitBegin();
    void push(const uint32_t *w, uint64_t dwords);
    void submitEnd();

    // Known programs, by the hash of their 80-byte header; a bound program is identified by an exact match of its bytes.
    std::multimap<uint64_t, ProgramInfo> programs;
    bool loadPrograms(const char *path);

    // Set-0 layouts (tools/mkpack.py layouts.txt: "<vs id> <fs id> <set bytes> <offset>:<u|h>..."): only the bytes an entry covers
    // are compared (m2v recycles its descriptor memory and writes only its layout's entries; the rest is the last owner's)
    struct Layout {
        uint32_t bytes;
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        std::set<uint32_t> textureOnly;
    };

    std::map<std::string, Layout> layouts;
    bool loadLayouts(const char *path);
    bool fullStateAtFirstDraw = true;  // one STATE record of every non-address register at the first draw

    // Called at every draw with the decoder in the draw's state (raw/runtime/raw_capture.cpp reads a state off it)
    std::function<void(Decoder &)> onDraw;
    // The capture's own (pack/cap.cpp): NVDEC_SCRUB's zeroing of what a probe draw read, NVDEC_DESCRIBE's naming of an address
    std::function<void(uint64_t va, uint64_t bytes)> scrub;
    std::function<void(uint64_t va)> describe;
    // The PTE kind a VA is bound with (the mock's model of the kext, pack/mock_comp.h): compressed targets' counts (comp)
    std::function<uint32_t(uint64_t va)> vaKind;
    CompCounts comp, compSubmit;
    // The draw's facts, as NVDEC_PROBE prints them (P lines)
    void probeRecord(ProbeRecord &out);
    // A program the known ones lack, by its SET_PIPELINE_PROGRAM offset: the recording driver's own (raw_capture.cpp asks NVK);
    // false: none. It is known from then on.
    std::function<bool(uint32_t offset, ProgramInfo &out)> programAt;
    // A known program by the name a bound one gets (null: none)
    const ProgramInfo *programById(const std::string &id) const;

    uint32_t reg3d(uint32_t mthd) const
    {
        return reg_[0][mthd >> 2];
    }

    // Constant buffer `slot` of shader group `group` (0 vertex .. 4 pixel), as bound now; false: not bound
    bool bound(int group, int slot, uint64_t &addr, uint32_t &bytes) const;
    // The root-table words [0x700, 0x780) this submission wrote (m2v's push ranges as NVK wrote them): mask bit k, word k
    void pushWords(uint32_t &mask, uint32_t words[32]) const;

    // The bound program of stage `stage` (1 vertex, 5 pixel) among the known ones, or null; `id` names it either way
    const ProgramInfo *boundProgram(int stage, std::string &id)
    {
        return program(stage, id);
    }

    uint64_t draws = 0, dispatches = 0, clears = 0, submits = 0, macroCalls = 0, unknownMacros = 0, badHeaders = 0;
    // Draws that read a constant buffer over bytes the copy engine wrote after the channel's last constant-cache invalidate
    // (their barriers name CBSTALE): the SMs' constant caches may still hold what those bytes were
    uint64_t staleConstantReads = 0;

    static uint64_t fnv(const void *p, size_t n, uint64_t h = 0xcbf29ce484222325ull);

private:
    struct Bind {
        uint64_t addr = 0;
        uint32_t size = 0;
        bool valid = false;
    };

    MapFn map_;
    FILE *out_;
    // the host tests' 2D engine (raw/fx/test/twod_sim: the mip chains' blits), executed into the BOs' host memory where the
    // decoder is built with NVDEC_TWOD (raw/test/build.sh); declared either way, so every object sees one layout
    [[maybe_unused]] void *twod_ = nullptr;
    void twod(uint32_t subc, uint32_t mthd, uint32_t data);
    uint32_t reg_[5][0x4000] = {};
    std::vector<uint32_t> mmeRam_ = std::vector<uint32_t>(0x2000), mmeStart_ = std::vector<uint32_t>(0x80);
    uint32_t mmeRamPtr_ = 0, mmeStartPtr_ = 0;
    // method parse state, carried across the pushes of one submission
    uint32_t op_ = 0, subc_ = 0, mthd_ = 0, left_ = 0;
    // a macro call collecting its parameters
    int pendingMacro_ = -1;
    std::vector<uint32_t> params_;
    // constant buffer bytes written by LOAD_CONSTANT_BUFFER (the GPU's view, over the CPU's)
    std::map<uint64_t, uint32_t> cbufWrites_;  // persists: the GPU's writes stay in memory across submissions
    std::set<uint64_t> writtenThisSubmit_;
    const Layout *layout_ = nullptr;  // of the draw being printed
    Bind binds_[5][18];
    // persistent resource names, by the host memory behind the address where the map knows it (r8-rawca: so the same pages
    // mapped in two channels' address spaces, the raw path's and m2v's twins, are one resource), shared by every decoder
    static std::map<uint64_t, int> &names();
    std::set<std::string> pendingBarriers_;
    // [from, to) the copy engine wrote since the last constant-cache invalidate (INVALIDATE_SHADER_CACHES(_NO_WFI) CONSTANT)
    std::vector<std::pair<uint64_t, uint64_t>> constantStale_;
    bool firstDraw_ = true;
    bool indexed_ = false;  // the draw being printed is DRAW_INDEX_BUFFER's

    void method(uint32_t subc, uint32_t mthd, uint32_t data, bool fromMme);
    void runMacro();
    void copyEngine(uint32_t launch);
    void draw(const char *kind, uint32_t a, uint32_t b);
    void probeDump(uint32_t first, uint32_t count);
    void clear(uint32_t data);
    void countComp(uint32_t clearData);
    void barrier(const std::string &what);
    bool readsStaleConstants();

    uint32_t r3(uint32_t mthd) const
    {
        return reg_[0][mthd >> 2];
    }

    uint64_t addr3(uint32_t hiMthd, uint32_t loMthd) const
    {
        return (uint64_t(r3(hiMthd) & 0xff) << 32) | r3(loMthd);
    }

    bool read(uint64_t va, void *dst, uint64_t bytes);  // CPU memory with the LOAD_CONSTANT_BUFFER writes over it
    std::string name(uint64_t va);
    std::string contentHash(uint64_t va, uint64_t bytes, int depth, bool handles, bool childHandles);
    std::string ticEntry(uint32_t index);
    std::string tscEntry(uint32_t index);
    const ProgramInfo *program(int stage, std::string &id);
};

}  // namespace nvdec
