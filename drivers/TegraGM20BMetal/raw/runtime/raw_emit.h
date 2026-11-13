// SPDX-License-Identifier: MIT
// A state's per-state 3D registers computed by the raw path from its descriptor and its two programs, the registers NVK emits for
// m2v's translation of the state (a record's blob, raw_record.h), with no NVK pipeline or capture. test/emitcheck holds them
// to every record of a pack, test/vertexcheck the vertex input's.
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace raw {

// What a program's compile tells (NAK's shader info): its header, its register count and, for a fragment program, whether
// it asks for early fragment tests
struct EmitProgram {
    const uint8_t *header = nullptr;  // the 80-byte shader program header
    uint32_t gprs = 0;
    bool earlyFragmentTests = false;
    uint32_t interlockControl = 0;
    bool usesSampleShading = false;
};

struct EmitRegister {
    uint32_t method, value;
    // false: the hardware does not read it for this state (a blend of a target it writes nothing to, or one whose blending
    // is off), so any value draws the same
    bool read;
};

// `keyFields`: the state key's fields before " | v" (Pack::string(keyString), RuntimeRecord::keyString). `blended`: the
// color attachments m2v hands a Vulkan blend (bit j: attachment j), where its translation is at hand; null: taken from the
// key, a blend of ONE and ZERO left off. `twoPlane`: its RGB10A8_2P_XR10 colours drawn in their two planes (raw_xr_pass.h),
// each alpha byte a target of its own, whose targets it gives `alphaTargets` (xrAlphaTargets). False: a field it can't read
// (`why`)
bool emitState(const std::string &keyFields, const EmitProgram &vertex, const EmitProgram &fragment,
               std::vector<EmitRegister> &out, std::string &why, const uint32_t *blended = nullptr,
               bool twoPlane = false, uint32_t *alphaTargets = nullptr);

// The vertex input's registers (SET_VERTEX_ATTRIBUTE_A(0..31), SET_VERTEX_STREAM_A_FORMAT of each stream) from the key's
// vertex descriptor fields and `attributes`, the vertex function's [[attribute(n)]] inputs it reads (its reflection's active
// vertexAttributes). m2v numbers their locations densely in attribute order (metal2vulkan link/varyings.rs assign) and the
// buffers they read densely in buffer-index order (link/vertex.rs vertex_input_state). False: an attribute the descriptor
// lacks, or a format or step m2v would fetch in the shader (`why`)
bool emitVertexInput(const std::string &keyFields, const std::vector<uint32_t> &attributes,
                     std::vector<EmitRegister> &out, std::string &why);

// A fixed-function vertex stream emitVertexInput lays out: its Metal buffer index, and whether its stride is its bind's
// (the key's UINT32_MAX)
struct EmitStream {
    uint8_t buffer;
    bool bindStride;
};

// The same, with the streams in stream order
bool emitVertexInput(const std::string &keyFields, const std::vector<uint32_t> &attributes,
                     std::vector<EmitRegister> &out, std::vector<EmitStream> &streams, std::string &why);

// Every method emitState and emitVertexInput may write
std::set<uint32_t> emittableMethods();

}  // namespace raw
