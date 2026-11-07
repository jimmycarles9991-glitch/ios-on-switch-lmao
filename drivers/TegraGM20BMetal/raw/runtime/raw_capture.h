// SPDX-License-Identifier: MIT
// A render state's raw record read off NVK's own emission, in process: the pack generator's technique (pack/rawpack.py) run on
// one command buffer NVK recorded with one draw of the state, decoded on the CPU by pack/nvdec (Mesa's MME simulator for NVK's
// macros) and never submitted. The decoder starts from NVK's context as the pack holds it (PackInit: NVK's queue init of this
// build, its MME macros among it), with the addresses of the live NVK set over it (set()).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace nvdec {
class Decoder;
}

namespace raw {

class Pack;

struct CapturedProgram {
    std::string id;  // nvdec's name: the FNV-1a of its bytes where known, "unknown:..." else
    // header + code, NAK's shader data, each constant buffer's nvk_cbuf_type
    std::vector<uint8_t> image, data, cbufTypes;
};

struct CapturedState {
    // The 3D methods the draw leaves other than the context did after its init, the raw path's own (raw_owned.h) left out
    std::vector<std::pair<uint32_t, uint32_t>> registers;
    CapturedProgram vertex, pixel;
    uint32_t pushMask = 0, push[32] = {};  // the root-table words [0x700, 0x780) the command buffer wrote
};

class Capture {
public:
    // `map`: [va, va + bytes) of the recording driver's GPU VM as its CPU left it, or null
    explicit Capture(std::function<const uint8_t *(uint64_t va, uint64_t bytes)> map);
    ~Capture();

    bool prime(const Pack &pack, std::string &err);
    // A method of the live driver's context the pack's init holds another value for (its own addresses)
    void set(uint32_t subc, uint32_t mthd, uint32_t value);

    // The programs a capture can name (their bytes, data and constant buffer types: NVK's records)
    nvdec::Decoder &decoder()
    {
        return *decoder_;
    }

    // One command buffer's push segments, in order: a record per draw
    std::vector<CapturedState> decode(const std::vector<std::pair<const uint32_t *, uint64_t>> &segments);

private:
    std::unique_ptr<nvdec::Decoder> decoder_;
    std::vector<uint32_t> context_;  // the 3D registers after prime()
    std::vector<CapturedState> drawn_;
    void record(nvdec::Decoder &d);
};

}  // namespace raw
