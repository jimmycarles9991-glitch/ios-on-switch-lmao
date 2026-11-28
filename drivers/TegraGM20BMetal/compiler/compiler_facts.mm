// SPDX-License-Identifier: MIT
// See compiler_facts.h.
#import "compiler_facts.h"

#import <Foundation/Foundation.h>
#include <algorithm>
#include <deque>
#include <vector>
#include "TegraGM20BCompilerABI.h"
#include "../TegraGM20BMetalCompileRequest.h"
#include "../TegraGM20BMetalOffline.h"
#include "../TegraGM20BMetalPipelineKey.h"
#include "../buffer_encoders.h"
#include "../compute_facts_entry.h"
#include "../function_attributes.h"
#include "../reflection_blob.h"
#include "../translate/NativeMetalTranslate.h"

namespace switch_compiler {
namespace {

// The constants as the translator takes them, indexed then named (TegraGM20BMetalOffline.mm).
// `newIndexedConstantArray` and `newNamedConstantArray` allocate wrapper objects that own and free
// `v.data` on dealloc, so `keep` retains both arrays across nmb_offline_function_new_specialized.
bool constantsOf(MTLFunctionConstantValues *values, std::vector<NmbConstant> &out, NSMutableArray *keep,
                 std::string &why)
{
    NSArray *indexed = [values newIndexedConstantArray], *named = [values newNamedConstantArray];
    if (indexed)
        [keep addObject:indexed];
    if (named)
        [keep addObject:named];
    for (unsigned group = 0; group < 2; group++)
        for (id<SwitchMetalConstantValue> v in (group ? named : indexed)) {
            const uint64_t bytes = gm20b::constantBytes(v.dataType);
            if (!bytes || !v.data)
                return why = "a constant of type " + std::to_string(uint64_t(v.dataType)) + " it cannot pass", false;
            out.push_back({v.dataType, group ? 0 : v.index, group ? v.name.UTF8String : nullptr, v.data, bytes});
        }
    return true;
}

NmbOfflineFunction *functionOf(const NmbOfflineLibrary *library, const gm20b::RequestStage &s,
                               std::vector<NmbConstant> &constants, NSMutableArray *keep, std::string &why)
{
    if (!s.specialized) {
        if (!s.constants.empty())
            return why = "constants of a function made without them", nullptr;
        return s.name == s.function
                   ? nmb_offline_function_new(library, s.function.c_str())
                   : nmb_offline_function_new_specialized(library, s.function.c_str(), s.name.c_str(), nullptr, 0);
    }
    MTLFunctionConstantValues *values = gm20b::constantValuesOf(s.constants);
    if (!values)
        return why = "constants it cannot read: " + s.constants, nullptr;
    [keep addObject:values];
    if (!constantsOf(values, constants, keep, why))
        return nullptr;
    return nmb_offline_function_new_specialized(library, s.function.c_str(),
                                                s.name != s.function ? s.name.c_str() : nullptr, constants.data(),
                                                uint32_t(constants.size()));
}

// vertexAttributes (`stageInput` false) or stageInputAttributes as the translator answers them into `list` (nullopt for
// nil); false with `why` where the translator answers none
bool attributesOf(const NmbOfflineFunction *f, bool stageInput, gm20b::AttributeList &list, std::string &why)
{
    const char *which = stageInput ? "stageInputAttributes" : "vertexAttributes";
    const int64_t count = stageInput ? nmb_offline_function_stage_input_attribute_count(f)
                                     : nmb_offline_function_vertex_attribute_count(f);
    if (count < 0 && *nmb_last_error())
        return why = std::string("the translator answers no ") + which + ": " + nmb_last_error(), false;
    if (count < 0)
        return list = std::nullopt, true;
    std::vector<gm20b::FunctionAttributeTag> out;
    for (uint32_t i = 0; i < uint32_t(count); i++) {
        char name[256];
        uint64_t info[5] = {};
        const size_t named = stageInput ? nmb_offline_function_stage_input_attribute(f, i, name, sizeof name, info)
                                        : nmb_offline_function_vertex_attribute(f, i, name, sizeof name, info);
        if (!named)
            return why = std::string("the translator answers no ") + which + " " + std::to_string(i) + ": " +
                         nmb_last_error(),
                   false;
        gm20b::FunctionAttributeTag a;
        a.name = name;
        a.index = uint8_t(info[0]);
        a.type = uint8_t(info[1]);
        a.active = info[2];
        a.patchData = info[3];
        a.controlPointData = info[4];
        out.push_back(std::move(a));
    }
    list = std::move(out);
    return true;
}

// The stage input as the offline descriptor takes it (TegraGM20BMetalOffline.mm computeDescriptor)
void stageInputOf(const gm20b::StageInFields &in, NmbOfflineComputePipelineDescriptor &d)
{
    for (uint32_t i = 0; i < 31; ++i) {
        const gm20b::AttributeFields &a = in.attributes[i];
        if (!a.format)
            continue;
        d.stage_inputs = 1;
        d.stage_attributes[i] = {a.format, a.offset, a.buffer};
        if (a.buffer < 31)
            d.stage_layouts[a.buffer] = {a.stride, a.step, a.rate};
    }
    d.stage_index_type = in.indexType;
    d.stage_index_buffer = in.indexBuffer;
}

// A request's stages as the translator's functions, of the libraries `libraries` gives (the compiler's, each read once
// per process); its functions released with it
class OfflineStages {
public:
    OfflineStages() = default;
    OfflineStages(const OfflineStages &) = delete;
    OfflineStages &operator=(const OfflineStages &) = delete;

    ~OfflineStages()
    {
        for (NmbOfflineFunction *f : functions_)
            nmb_offline_function_release(f);
    }

    bool make(const gm20b::CompileRequest &r, const Libraries &libraries, std::string &why)
    {
        held_.resize(r.libraries.size());
        for (const gm20b::RequestStage &s : r.stages) {
            __strong id<MTLLibrary> &held = held_[s.library];
            if (!held && !(held = libraries(r.libraries[s.library], why)))
                return false;
            const NmbOfflineLibrary *library = offline::libraryHandle(held);
            if (!library)
                return why = "a library the translator does not hold", false;
            NmbOfflineFunction *f = functionOf(library, s, constants_.emplace_back(), values_, why);
            if (!f) {
                if (why.empty())
                    why = "no function " + s.function + ": " + nmb_last_error();
                return false;
            }
            functions_.push_back(f);
        }
        return true;
    }

    NmbOfflineFunction *function(size_t stage) const
    {
        return functions_[stage];
    }

private:
    std::vector<id<MTLLibrary>> held_;
    std::deque<std::vector<NmbConstant>> constants_;  // what each function was made with, held as long as it is
    NSMutableArray *values_ = [NSMutableArray array];
    std::vector<NmbOfflineFunction *> functions_;
};

bool collectEncoder(uint32_t index, bool argumentBuffer, const NmbArgumentLayout *layouts, uint32_t count,
                    const char *refusal, void *ctx)
{
    static_assert(sizeof(gm20b::ArgumentRun) == sizeof(NmbArgumentRun), "the bridge's runs");
    gm20b::BufferEncoder e;
    e.index = index;
    e.argumentBuffer = argumentBuffer;
    e.refusal = refusal ? refusal : "";
    for (uint32_t i = 0; i < count; i++) {
        const auto *runs = reinterpret_cast<const gm20b::ArgumentRun *>(layouts[i].runs);
        e.layouts.push_back({layouts[i].encoded_length, layouts[i].alignment,
                             std::vector<gm20b::ArgumentRun>(runs, runs + layouts[i].run_count)});
    }
    static_cast<gm20b::BufferEncoders *>(ctx)->push_back(std::move(e));
    return true;
}

}  // namespace

int32_t makeBufferEncoders(const Prepared &p, const Libraries &libraries, std::string &entry, std::string &why)
{
    @autoreleasepool {
        OfflineStages stages;
        if (!stages.make(p.request, libraries, why))
            return TegraGM20BCompileUnavailable;
        gm20b::BufferEncoders encoders;
        const bool made = nmb_offline_function_buffer_encoders(stages.function(0), collectEncoder, &encoders);
        if (!made)
            why = "the translator does not decode " + p.request.stages[0].function + ": " + nmb_last_error();
        const std::string blob = gm20b::encodeBufferEncoders(encoders);
        entry = gm20b::encodeReflectionEntry(p.content, made ? &blob : nullptr, why);
        return made ? TegraGM20BCompileMade : TegraGM20BCompileRefused;
    }
}

int32_t makeAttributes(const Prepared &p, const Libraries &libraries, std::string &entry, std::string &why)
{
    @autoreleasepool {
        OfflineStages stages;
        if (!stages.make(p.request, libraries, why))
            return TegraGM20BCompileUnavailable;
        const NmbOfflineFunction *function = stages.function(0);
        gm20b::AttributeList vertex, stageInput;
        const bool made = attributesOf(function, false, vertex, why) && attributesOf(function, true, stageInput, why);
        const std::string blob = gm20b::encodeAttributeLists(vertex, stageInput);
        entry = gm20b::encodeReflectionEntry(p.content, made ? &blob : nullptr, why);
        return made ? TegraGM20BCompileMade : TegraGM20BCompileRefused;
    }
}

int32_t makeFacts(const Prepared &p, const Libraries &libraries, std::string &entry, std::string &why)
{
    @autoreleasepool {
        OfflineStages stages;
        if (!stages.make(p.request, libraries, why))
            return TegraGM20BCompileUnavailable;
        std::vector<NmbOfflineFunction *> linked;
        std::vector<switch_linked_group_name> groupNames;
        for (const char *tag : {"kl", "kp", "kb", "kg"})
            for (size_t i = 1; i < p.request.stages.size(); ++i)
                if (p.request.stages[i].tag == tag) {
                    linked.push_back(stages.function(i));
                    if (p.request.stages[i].tag == "kg")
                        groupNames.push_back({p.request.stages[i].group.data(), p.request.stages[i].group.size()});
                }
        const size_t visible = size_t(
            std::count_if(p.request.stages.begin() + 1, p.request.stages.end(), [](const gm20b::RequestStage &s) {
                return s.tag == "kl";
            }));
        const size_t hidden = size_t(
            std::count_if(p.request.stages.begin() + 1, p.request.stages.end(), [](const gm20b::RequestStage &s) {
                return s.tag == "kp";
            }));
        NmbOfflineComputePipelineDescriptor d = {};
        d.function = stages.function(0);
        d.max_threads = p.request.kernel.maxThreads;
        d.multiple_of_execution_width = p.request.kernel.multipleOfWidth;
        d.linked = linked.data();
        d.linked_count = visible;
        d.private_linked = linked.data() + visible;
        d.private_count = hidden;
        d.binary_linked = linked.data() + visible + hidden;
        d.binary_count = linked.size() - visible - hidden - groupNames.size();
        d.group_linked = d.binary_linked + d.binary_count;
        d.group_names = groupNames.data();
        d.group_count = groupNames.size();
        stageInputOf(p.request.kernel.stageIn, d);
        uint32_t facts[3] = {};
        const bool made = nmb_offline_compute_pipeline_facts(&d, facts);
        if (!made)
            why = std::string("no compute prepare: ") + nmb_last_error();
        entry = gm20b::encodeFactsEntry(p.content, made ? facts : nullptr, why);
        return made ? TegraGM20BCompileMade : TegraGM20BCompileRefused;
    }
}

}  // namespace switch_compiler
