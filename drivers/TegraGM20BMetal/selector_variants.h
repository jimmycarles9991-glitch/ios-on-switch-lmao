// SPDX-License-Identifier: MIT
// Fold only meaningful config bytes. The pack's alignment assumption also proves the folded buffer's bind residual is 0.
#pragma once
#include <cstdint>
#include <functional>
#include "emit.h"
#include "pack_selectors.h"
#include "record.h"

namespace gm20b {

struct UboFold {
    uint32_t desc = 0;  // the buffer's set-0 entry's byte
    uint64_t mask = 0;  // bit i: byte i is folded
    const uint8_t *bytes = nullptr;
};

// The fragment stage compiled with `fold` and the root push words of `zeroPush` as 0; false: not made
using FoldedCompile =
    std::function<bool(const UboFold &fold, uint32_t zeroPush, CapturedProgram &program, EmitProgram &emit)>;

// The record's set-0 entry of the configs' buffer (a fragment uniform buffer at `slot`), -1: none
inline int32_t selectorEntry(const RuntimeRecord &record, uint8_t slot)
{
    const SetEntries entries = entriesOf(record);
    for (uint32_t i = 0; i < entries.size(); i++)
        if (entries[i].kind == EntryUbo && entries[i].stage == 2 && entries[i].slot == slot && uniformEntry(entries[i]))
            return int32_t(i);
    return -1;
}

// The constant buffer slot `program` reads the uniform buffer of set-0 byte `desc` at, -1: none
inline int32_t selectorBank(const CapturedProgram &program, uint32_t desc)
{
    for (uint32_t c = 0; c < program.cbufTypes.size(); c++)
        if (cbufKind(program.cbufTypes[c]) == CbufUbo &&
            cbufUboOffset(program.cbufTypes.data(), farUbosOf(program), c) == desc)
            return int32_t(c);
    return -1;
}

inline uint64_t selectorCompared(const SelectorConfigs &configs)
{
    return (configs.length >= 64 ? ~0ull : (1ull << configs.length) - 1) & ~configs.padding;
}

inline bool selectorVariants(const SelectorConfigs &configs, const EmitProgram &generic, const FoldedCompile &compile,
                             RuntimeRecord &out, std::string &why)
{
    const PackPipeline &p = out.pipeline;
    const int32_t entry = selectorEntry(out, configs.slot);
    if (entry < 0 || !configs.length || configs.length > kSelectorBytes || !pixelStage(p)) {
        why = "selector config has no fragment uniform entry of its layout";
        return false;
    }
    const SetEntries entries = entriesOf(out);
    uint32_t assume = 0, zeroPush = 0;
    const int word = bufferResidualWord(entries[entry]);
    bool residualAsserted = word < 0;
    for (uint32_t k = 0; k < kUniformRanges; k++) {
        if (p.uniform[k].entry != uint32_t(entry) || word < 0 || word >= 32)
            continue;
        zeroPush |= 1u << word;
        assume |= AssumeUniformAligned << k;
        residualAsserted = true;
    }
    if (!residualAsserted) {
        why = "selector config has no alignment contract for its buffer residual";
        return false;
    }
    const uint64_t compared = selectorCompared(configs);
    for (const SelectorConfig &config : configs.values) {
        RuntimeRecord::Selector made;
        made.id = config.id;
        for (uint32_t i = 0; i < configs.length; i++)
            made.value[i] = compared >> i & 1 ? config.value[i] : 0;
        EmitProgram emit;
        if (!compile({entries[entry].offset, compared, made.value}, zeroPush, made.program, emit)) {
            why = "selector config " + std::to_string(config.id) + " fragment compilation failed";
            return false;
        }
        if (made.program.cbufTypes != out.programs[1].cbufTypes || made.program.farUbos != out.programs[1].farUbos ||
            emit.earlyFragmentTests != generic.earlyFragmentTests ||
            emit.interlockControl != generic.interlockControl || emit.usesSampleShading != generic.usesSampleShading) {
            why = "selector config " + std::to_string(config.id) + " changed fragment bindings or ordering";
            return false;
        }
        const CapturedProgram &base = out.programs[1];
        if (made.program.image == base.image && made.program.data == base.data && made.program.gprs == base.gprs &&
            made.program.boundTextures == base.boundTextures) {
            out.selectorGeneric.push_back(config.id);
            continue;
        }
        out.selectors.push_back(std::move(made));
    }
    out.selectorAssume = assume;
    return true;
}

}  // namespace gm20b
