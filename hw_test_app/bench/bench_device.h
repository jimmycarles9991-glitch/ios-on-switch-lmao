// What switch_bench.cpp (draws, dispatches) and bench_twod.cpp (the 2D engine, the copy engine, compare forms) share:
// the bench's raw device, its targets and one timed command buffer.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "drivers/TegraGM20BMetal/raw/runtime/raw_gpu.h"
#include "hw_test_app/bench/bench_records.h"
#include "hw_test_app/bench/switch_bench.h"

struct switch_bench {
    std::unique_ptr<raw::Device> device;
    const raw::PackPipeline *fragment = nullptr;
    const raw::PackPipeline *textured = nullptr;  // the template of a program that samples (bench::textureTemplate)
    const raw::PackKernel *kernel = nullptr;
    raw::Texture image;
    raw::Sampler sampler, linear;
    std::string pack;
    bool imageMade = false, samplerMade = false, linearMade = false;
    uint64_t records = 0;
    std::string error;

    bool open();
    // After a kill: a fresh connection, GPU VM and channel, so no state the killed work left behind reaches the next
    void reopen();
    void close();
};

namespace bench {

struct Target {
    raw::Texture color;
    std::vector<raw::Texture> others;  // the template's other colours, memoryless
    // A colour with no CPU view (compressible memory): its texels copied in from here before each pass and out after it
    raw::Texture *staging = nullptr;
    const raw::Texture *image = nullptr;  // what the template's texture entries sample; null: the bench's picture
    bool linear = false;                  // they sample it filtered (else nearest)
    // The experiment's storage buffer: the addressed job ABI resolves only the addresses a command buffer names
    const raw::Buffer *storage = nullptr;
};

void fail(switch_bench_result &r, uint32_t status, const std::string &why);
// The experiment's fill (none: zeros) into `t`, and `t`'s texels out, each under a CPU lease of `t`'s memory; false: the
// kernel refused the lease
bool fill(raw::Device &d, raw::Texture &t, const switch_bench_experiment &x, uint32_t bpp);
bool readBack(raw::Device &d, const raw::Texture &t, const switch_bench_experiment &x, uint32_t bpp, uint8_t *into,
              size_t bytes);
// A template's other colour formats, from its key string's " c<n>=" fields
std::vector<uint32_t> otherFormats(const char *key);
// The template's inputs as fetchform binds the pack's states over `rect` of a `width` x `height` target
void bindTemplate(raw::RenderEncoder *e, const raw::Pipeline &p, const uint32_t rect[4], uint32_t width,
                  uint32_t height, const raw::Texture &image, const raw::Sampler &sampler);
// The target and the template's other colours (memoryless); false: no memory, `r` says so
bool makeTarget(raw::Device &d, const switch_bench_experiment &x, const std::vector<uint32_t> &others,
                bool compressible, Target &t, switch_bench_result &r);
void destroyTarget(raw::Device &d, Target &t);
// The caller's pixel program in the template's state, made a pipeline of the device; null: `r` says why.
// `firstColourOnly`: the state without the template's other colours, for a pass that binds colour 0 alone
// `storageVa`: written into the shader data at x.storage_va_at (0: none)
raw::Pipeline *pixelPipeline(switch_bench &b, const switch_bench_experiment &x, switch_bench_result &r,
                             bool firstColourOnly, uint64_t storageVa = 0);
// The experiment's command buffers into `t`, fill first where it has one; false: `r` says why
bool timeDraws(switch_bench &b, const switch_bench_experiment &x, Target &t, raw::Pipeline *pipeline,
               switch_bench_result &r);
// A trap in a lone command buffer can land after its fence (the channel is killed while the wait already returned), so
// one more empty pass on the channel: it never completes on a killed one
void checkAlive(switch_bench &b, const switch_bench_experiment &x, Target &t, switch_bench_result &r);

// One command buffer and its times. False: `r` says why
class Submission {
public:
    Submission(switch_bench &b, const switch_bench_experiment &x, switch_bench_result &r) : b_(b), x_(x), r_(r)
    {}

    // `draws` quads of `pipeline` (none: an empty pass) into `t`
    bool render(Target &t, raw::Pipeline *pipeline, uint32_t draws, uint64_t &gpu, uint64_t &wall);
    bool dispatch(const raw::ComputeState *state, uint32_t dispatches, uint64_t &gpu, uint64_t &wall);
    // Whatever `body` records; false from it abandons the command buffer (`r` already says why)
    bool record(const std::function<bool(raw::CommandBuffer &)> &body, uint64_t &gpu, uint64_t &wall);
    bool refused(const std::string &why);

private:
    switch_bench &b_;
    const switch_bench_experiment &x_;
    switch_bench_result &r_;

    bool finish(raw::CommandBuffer *cb, uint64_t &gpu, uint64_t &wall);
    bool error(const std::string &why);
};

// A compare form's run: its own status, why and times, so its failure or its timing never touches the experiment's
class CompareRun {
public:
    CompareRun(const switch_bench_experiment &x, const switch_bench_result &of);
    switch_bench_result r;
    // Its status into the experiment's compare_status; a killed channel kills the experiment too (the bench reopens)
    void report(switch_bench_result &into) const;

private:
    std::vector<uint64_t> gpu_, wall_;
};

void runTwod(switch_bench &b, const switch_bench_experiment &x, switch_bench_result &r);
void runPack(switch_bench &b, const switch_bench_experiment &x, switch_bench_result &r);
void fillSurface(switch_bench &b, const void *surface, uint32_t format, uint32_t color, switch_bench_result &r);

}  // namespace bench
