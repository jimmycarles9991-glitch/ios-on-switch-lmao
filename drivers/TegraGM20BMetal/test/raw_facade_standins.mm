// SPDX-License-Identifier: MIT
// See raw_facade_standins.h. The frontend's calls answer none (no device, no twin, no NVK); the bridge's heap words are the
// test's constants.
#include "raw_facade_standins.h"
#import <Metal/Metal.h>
#include <map>
#include <utility>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalDirect.h"
#include "bridge/NativeMetalObjects.h"

namespace standin {
namespace {

std::map<std::pair<void *, uint32_t>, std::vector<Layout>> &layouts()
{
    static std::map<std::pair<void *, uint32_t>, std::vector<Layout>> all;
    return all;
}

}  // namespace

void functionLayouts(id twin, uint32_t index, std::vector<Layout> made)
{
    layouts()[{(__bridge void *)twin, index}] = std::move(made);
}

Asks &asks()
{
    static Asks counted;
    return counted;
}

}  // namespace standin

static_assert(sizeof(raw::ArgumentRun) == sizeof(NmbArgumentRun), "the bridge's runs");

extern "C" bool native_bridge_metal_argument_layouts(id function, NSUInteger index,
                                                     bool (*each)(const struct NmbArgumentLayout *, uint32_t, void *),
                                                     void *ctx, const char **why)
{
    const auto found = standin::layouts().find({(__bridge void *)function, uint32_t(index)});
    if (found == standin::layouts().end()) {
        *why = "no argument buffer at that index";
        return false;
    }
    std::vector<NmbArgumentLayout> out;
    for (const standin::Layout &l : found->second)
        out.push_back({l.encodedLength, l.alignment, reinterpret_cast<const NmbArgumentRun *>(l.runs.data()),
                       uint32_t(l.runs.size()), 0});
    return each(out.data(), uint32_t(out.size()), ctx);
}

extern "C" bool switch_nmb_heap_holds(uint32_t, const NmbHeapTextureFacts *)
{
    return true;
}

extern "C" uint32_t switch_nmb_heap_texture_flags(const NmbHeapTextureFacts *)
{
    standin::asks().textureFlags++;
    return standin::kTextureFlags;
}

extern "C" uint32_t switch_nmb_sampler_word(const struct NmbSamplerDescriptor *)
{
    standin::asks().samplerWords++;
    return standin::kSamplerWord;
}

extern "C" const char *nmb_last_error(void)
{
    return "no bridge on this host";
}

id native_bridge_metal_buffer_over_foreign(const struct switch_foreign_memory *, void *, NSUInteger, NSUInteger)
{
    return nil;
}

bool native_bridge_metal_compute_direct(id, const struct NmbDirectDispatch *,
                                        bool (*)(const struct NmbDirectKernel *, void *), void *, const char **why)
{
    *why = "no frontend on this host";
    return false;
}

id native_bridge_metal_compute_prepare(id, NSUInteger, id *, NSError **)
{
    return nil;
}

NSString *native_bridge_metal_function_library_name(id)
{
    return nil;
}

id native_bridge_metal_function_metallib(id, const void **, size_t *)
{
    return nil;
}

void native_bridge_metal_journal_dispatch(id, MTLSize, MTLSize, bool, const char *, const char *, const char *)
{}

void native_bridge_metal_journal_render(id, NSUInteger)
{}

bool native_bridge_metal_memory(uint64_t out[3])
{
    out[0] = out[1] = out[2] = 0;
    return false;
}

void *native_bridge_metal_nvk_symbol(const char *)
{
    return nullptr;
}

bool native_bridge_metal_render_direct(id, bool, NSUInteger, NSUInteger, const struct NmbDirectBinds *,
                                       bool (*)(const struct NmbDirectState *, void *), void *, const char **why)
{
    *why = "no frontend on this host";
    return false;
}

id native_bridge_metal_render_pipeline_lazy(id, id, NSUInteger)
{
    return nil;
}

int native_bridge_metal_render_pipeline_ready(id, void (^)(void))
{
    return -1;
}

int native_bridge_metal_render_pipeline_urge(id, void (^)(void), dispatch_queue_t)
{
    return -1;
}

void native_bridge_metal_set_surface_importer(switch_surface_importer)
{}

id native_bridge_metal_texture_in_place(id, id, NSUInteger, NSUInteger)
{
    return nil;
}

int native_bridge_metal_texture_memory(id, struct switch_nvk_image_memory *)
{
    return -1;
}

id native_bridge_metal_texture_tiled_in_place(id, id, NSUInteger)
{
    return nil;
}
