// SPDX-License-Identifier: MIT
// What the full-form facade tests make their states of: the stand-in compiler (facade_variant_compiler.h) serving a pack
// that lacks the test's BGRA8 state, and the record shapes it answers with
#pragma once
#include "facade_variant_compiler.h"

#include <string>
#include <vector>

namespace harness {

constexpr uint32_t kFullFormResidualWord = 16;

inline id<MTLLibrary> servedFull(RenderCompiler &compiler, const std::string &name, id<MTLDevice> __strong &device)
{
    const std::string directory = dir() + "/" + name + "-" + std::to_string(getpid());
    mkdir(directory.c_str(), 0700);
    if (!compiler.serve(directory))
        return nil;
    NSString *why = nil;
    device = metal_device_open(writePack(packKey(MTLPixelFormatRGBA16Float)).c_str(), 0, 0, &why);
    if (!device)
        return nil;
    const std::string cache = directory + "/cache";
    mkdir(cache.c_str(), 0700);
    metal_complete_states(cache.c_str(), nullptr);
    return [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:nil];
}

// A fragment uniform buffer read through residual push word kFullFormResidualWord, after the record's entries: what a
// program variant assumes is 0
inline void residualUniform(gm20b::RuntimeRecord &r)
{
    gm20b::PackPipeline &pp = r.pipeline;
    const uint32_t k = gm20b::entryCount(pp);
    gm20b::setEntryCount(pp, k + 1);
    pp.entry[k] = {uint16_t(16 * k), gm20b::EntryUbo, 2, 1, uint8_t((kFullFormResidualWord + 1) << 2), 0, 0};
    pp.setBytes = uint16_t(16 * (k + 1));
    pp.memoryEntries |= 1u << k;
    pp.pushMask |= 1u << kFullFormResidualWord;
    pp.uniform[0] = {uint8_t(k), 1, 0, 0};
}

// What the full form adds: the fragment program's aligned variant and its split (split_record_test.cpp's batch split)
inline void addFullForm(gm20b::RuntimeRecord &r)
{
    gm20b::CapturedProgram aligned = r.programs[1];
    aligned.image[84] = 1;
    aligned.id = "fs-aligned";
    r.variants.push_back({gm20b::AssumeUniformAligned, aligned});
    r.pipeline.variantCount[1]++;
    r.splitTried = r.split = true;
    gm20b::PackSplit &s = r.splitRecord;
    s.frontierBytes = 64;
    s.frontierCbuf = 2;
    s.prologueCbufs = 1;
    s.batchVersion = 1;
    s.batchThreads = 32;
    s.qmd[18] = 32u << 16;
    s.qmd[19] = 0x10001;
    r.splitPrograms[0] = r.programs[1];
    r.splitPrograms[0].image[85] = 1;
    r.splitPrograms[0].id = "fs-split";
    r.splitPrograms[0].cbufTypes = {gm20b::CbufRoot, gm20b::CbufSet, gm20b::CbufFrontier};
    r.splitPrograms[1].id = "prologue";
    r.splitPrograms[1].image.assign(80 + 64, 0);
}

// A plain record as compilerd makes one: its split not tried
inline void primaryForm(gm20b::RuntimeRecord &r)
{
    r.splitTried = false;
}

// The facade_made_twin_test.cpp storage form, which a depth attachment's state needs
inline void storageForm(gm20b::RuntimeRecord &record)
{
    gm20b::PackPipeline &pp = record.pipeline;
    pp.flags |= gm20b::PipelineStorageMetadata;
    pp.storageMask = pp.storageWriteMask = 1;
    pp.storageSamples = 1;
    pp.storageTic[0] = gm20b::formatInfo(MTLPixelFormatBGRA8Unorm)->tic0;
    record.blob = {0x1224, 1, 0x1a00, 0};
}

inline id<MTLRenderPipelineState> createdState(id<MTLDevice> device, id<MTLLibrary> library, bool depth,
                                               std::string &why)
{
    MTLRenderPipelineDescriptor *d = descriptor(library, MTLPixelFormatBGRA8Unorm);
    if (depth)
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    NSError *error = nil;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:&error];
    if (!state)
        why = error.localizedDescription.UTF8String ?: "no state";
    return state;
}

inline gm20b::Pipeline *rawOf(id<MTLRenderPipelineState> state)
{
    __unsafe_unretained SwitchMetalPipeline *pipeline = (SwitchMetalPipeline *)state;
    return state ? pipeline->_raw : nullptr;
}

// The test's state made by `compiler`; `device` and `state` keep it alive, `why` says why none
inline gm20b::Pipeline *madeFull(RenderCompiler &compiler, const char *name, id<MTLDevice> __strong &device,
                                 id<MTLRenderPipelineState> __strong &state, std::string &why, bool depth = false)
{
    id<MTLLibrary> library = servedFull(compiler, name, device);
    if (!library)
        return why = "no device or library", nullptr;
    state = createdState(device, library, depth, why);
    return rawOf(state);
}

inline std::vector<gm20b::RequestForm> forms(RenderCompiler &compiler)
{
    std::vector<gm20b::RequestForm> out;
    for (const gm20b::CompileRequest &r : compiler.captured())
        out.push_back(r.form);
    return out;
}

inline const gm20b::VariantTable *tableOf(const gm20b::Pipeline *pl)
{
    return __atomic_load_n(&pl->variantTable, __ATOMIC_ACQUIRE);
}

inline const gm20b::SplitSource *splitOf(const gm20b::Pipeline *pl)
{
    return __atomic_load_n(&pl->splitSource, __ATOMIC_ACQUIRE);
}

}  // namespace harness
