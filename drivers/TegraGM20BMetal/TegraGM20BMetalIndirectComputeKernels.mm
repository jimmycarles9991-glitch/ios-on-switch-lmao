// SPDX-License-Identifier: MIT
// An indirect dispatch command's kernels at its shape, and its regions and binds written into its block
// (TegraGM20BMetalIndirectCompute.h)
#import "TegraGM20BMetalIndirectCompute.h"
#include "TegraGM20BMetalDirect.h"
#include "compute_key.h"

using SwitchMetalKernelJob = gm20b::ComputeKernelJob<gm20b::ComputeState>;

namespace {

void waitFor(SwitchMetalKernelJob &job)
{
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    if (job.observe([done] {
            dispatch_semaphore_signal(done);
        }) == SwitchMetalKernelJob::State::Pending)
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
}

std::shared_ptr<SwitchMetalKernelJob> requestKernel(SwitchMetalComputeState *state,
                                                    const gm20b::ComputeSpecialization &specialization)
{
    return state->_kernels.request(specialization, [&](std::shared_ptr<SwitchMetalKernelJob> pending) {
        SwitchMetalKernelJob::Run run{pending};
        gm20b::Completion *completion = gRM->completion.get();
        if (!completion)
            return;
        const std::map<uint32_t, uint32_t> memory(specialization.memory.begin(), specialization.memory.end());
        gm20b::KernelDispatch ask(specialization.threads.data(), memory, specialization.regionMode);
        const auto text = specialization.text();
        const auto key = gm20b::computeKey(state->_air, state->_constants, state->_maxThreads, state->_multipleOfWidth,
                                           specialization.threads.data(), text.memory, text.textures, text.samplers,
                                           specialization.regionMode);
        run.result.state = gm20b::computeState(*gRM->raw, gm20b::computeKeyHash(key));
        if (run.result.state) {
            run.result.retry = false;
            return;
        }
        run.result.state = completion->kernel(state->_direct, ask.dispatch(), key, &run.result.retry, state->_function);
    });
}

// A region's launches: its slices within the QMD's grid fields
uint64_t regionLaunches(const gm20b::ComputeRegion &region)
{
    return gm20b::qmdSliceCount({region.groups[0], region.groups[1], region.groups[2]});
}

}  // namespace

std::shared_ptr<SwitchMetalKernelSet> gm20bCommandKernels(SwitchMetalComputeState *pipeline,
                                                          const SwitchMetalICBDispatch &c,
                                                          gm20b::ComputeDispatch &shape, SwitchMetalICBUnmade &unmade)
{
    unmade = {};
    if (c.invalid) {
        unmade = {c.invalidWhy, gm20b::ComputeFailure::Invalid, c.invalid};
        return nullptr;
    }
    if (!c.dispatched)
        return nullptr;
    if (!pipeline) {
        unmade = {"an indirect dispatch command with no compute pipeline state of this device",
                  gm20b::ComputeFailure::Invalid, PredCompute};
        return nullptr;
    }
    shape = gm20b::computeDispatch(c.grid, c.threads, c.perThread, pipeline->_threadLimit);
    if (shape.reason != gm20b::ComputeDispatch::Ready) {
        unmade = {"an indirect dispatch command's threads per threadgroup past its state's limit or none on an axis, "
                  "or a grid past a thread's position",
                  gm20b::ComputeFailure::Invalid};
        return nullptr;
    }
    if (!shape.count)
        return nullptr;
    auto kernels = std::make_shared<SwitchMetalKernelSet>();
    kernels->count = shape.count;
    for (uint8_t region = 0; region < shape.count; ++region) {
        gm20b::ComputeSpecialization specialization;
        specialization.threads = shape.regions[region].local;
        specialization.regionMode = shape.multi;
        specialization.memory = c.memory;
        kernels->jobs[region] = requestKernel(pipeline, specialization);
        waitFor(*kernels->jobs[region]);
    }
    if (kernels->state() != SwitchMetalKernelSet::State::Ready) {
        unmade = {"an indirect dispatch command whose kernel the shader compiler refused, or could not make"};
        return nullptr;
    }
    for (uint8_t region = 0; region < kernels->count; ++region)
        if (!gm20b::ComputeEncoder::icbArgumentTables(*gRM->raw, *kernels->jobs[region]->value())) {
            unmade = {"no memory for the tables an indirect dispatch command's kernel reads argument buffers through",
                      gm20b::ComputeFailure::OutOfMemory};
            return nullptr;
        }
    return kernels;
}

uint64_t gm20bCommandBytes(const SwitchMetalKernelSet &kernels, const gm20b::ComputeDispatch &shape)
{
    uint64_t bytes = 0;
    for (uint8_t region = 0; region < kernels.count; ++region)
        bytes += uint64_t(gm20b::ComputeEncoder::icbRegionBytes(*kernels.jobs[region]->value()->rec)) *
                 regionLaunches(shape.regions[region]);
    return bytes;
}

void gm20bCommandBuffers(const SwitchMetalICBDispatch &c, gm20b::ComputeEncoder::IcbBuffer (&out)[31])
{
    for (uint32_t slot = 0; slot < 31; slot++) {
        SwitchMetalBuffer *b = c.buffers[slot];
        out[slot] = {};
        if (b && b->_raw.va && c.offsets[slot] <= b->_raw.length)
            out[slot] = {b->_raw.va + c.offsets[slot],
                         uint32_t(std::min<uint64_t>(b->_raw.length - c.offsets[slot], 0xffffffffu))};
    }
}

bool gm20bCommandBinds(const SwitchMetalKernelSet &kernels, const gm20b::ComputeDispatch &shape,
                       const SwitchMetalICBBlock &block, SwitchMetalICBDispatch &c, const char **why)
{
    c.binds.clear();
    c.strideWords.clear();
    c.roots.clear();
    c.bindSlots = c.strideSlots = 0;
    uint64_t at = 0;
    for (uint8_t region = 0; region < kernels.count; ++region) {
        const gm20b::ComputeState &k = *kernels.jobs[region]->value();
        for (uint64_t s = 0; s < regionLaunches(shape.regions[region]); ++s) {
            if (!gm20b::ComputeEncoder::icbBinds(k, block.va + at, c.binds, c.strideWords, c.roots, why))
                return false;
            at += gm20b::ComputeEncoder::icbRegionBytes(*k.rec);
        }
    }
    for (const gm20b::icb::ComputeBind &b : c.binds)
        c.bindSlots |= 1u << b.slot;
    for (const gm20b::icb::ComputeStride &s : c.strideWords)
        c.strideSlots |= 1u << s.slot;
    for (const gm20b::icb::ComputeRoot &r : c.roots)
        c.bindSlots |= 1u << r.slot;
    return true;
}

bool gm20bCommandRegions(const SwitchMetalKernelSet &kernels, const gm20b::ComputeDispatch &shape,
                         const SwitchMetalICBDispatch &c, const gm20b::ComputeEncoder::IcbBuffer (&buffers)[31],
                         const std::array<uint32_t, 31> &strides, const SwitchMetalICBBlock &block,
                         gm20b::icb::ComputeCommand &words, const char **why)
{
    gm20b::StageIn stageIn;
    stageIn.strides = strides;
    if (c.stageIn)
        stageIn.setRegion({c.stageInRegion.origin.x, c.stageInRegion.origin.y, c.stageInRegion.origin.z,
                           c.stageInRegion.size.width, c.stageInRegion.size.height, c.stageInRegion.size.depth});
    uint64_t at = 0;
    words.qmd.clear();
    for (uint8_t region = 0; region < kernels.count; ++region) {
        const gm20b::ComputeState &k = *kernels.jobs[region]->value();
        const gm20b::ComputeRegion &part = shape.regions[region];
        for (uint64_t s = 0; s < regionLaunches(part); ++s) {
            if (!gm20b::ComputeEncoder::icbRegion(*gRM->raw, k, part, gm20b::qmdSlice({{}, part.groups}, s), shape.grid,
                                                  shape.nominal, buffers, stageIn, block.cpu + at, block.va + at, why))
                return false;
            words.qmd.push_back(block.va + at);
            at += gm20b::ComputeEncoder::icbRegionBytes(*k.rec);
        }
    }
    return true;
}
