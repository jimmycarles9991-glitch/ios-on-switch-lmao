// SPDX-License-Identifier: MIT
// See compiler_facts.h.
#import "compiler_facts.h"

#import <Foundation/Foundation.h>
#include <algorithm>
#include <deque>
#include <vector>
#include "MetallibSlice.h"
#include "TegraGM20BCompilerABI.h"
#include "../raw/runtime/RawCompileRequest.h"
#include "../raw/runtime/RawPipelineKey.h"
#include "../raw/runtime/raw_buffer_encoders.h"
#include "../raw/runtime/raw_compute_facts_entry.h"
#include "../raw/runtime/raw_function_attributes.h"
#include "../raw/runtime/raw_reflection_blob.h"
#include "../translate/NativeMetalTranslate.h"

namespace switch_compiler {
namespace {

// The AIR slice of the library `l` names: a system library's file under `root` (`held` keeps its mapping), else its bytes
bool airSliceOf(const raw::RequestLibrary &l, const std::string &root, NSData *__strong &held, const uint8_t *&slice,
                size_t &size, std::string &why)
{
    const uint8_t *file = reinterpret_cast<const uint8_t *>(l.bytes.data());
    size_t length = l.bytes.size();
    if (l.system) {
        std::string real;
        if (!SystemLibraries::resolve(root, l.path, real, why))
            return false;
        held = [NSData dataWithContentsOfFile:@(real.c_str()) options:NSDataReadingMappedIfSafe error:nil];
        if (!held)
            return why = "an unreadable library: " + l.path, false;
        file = static_cast<const uint8_t *>(held.bytes);
        length = held.length;
    }
    size_t at = 0;
    if (!switch_metal::airSlice(file, length, at, size))
        return why = "no AIR slice Metal takes in " + (l.path.empty() ? std::string("its library's data") : l.path),
               false;
    slice = file + at;
    return true;
}

// The constants as the translator takes them, indexed then named (raw/runtime/RawOffline.mm)
bool constantsOf(MTLFunctionConstantValues *values, std::vector<NmbConstant> &out, std::string &why)
{
    NSArray *indexed = [values newIndexedConstantArray], *named = [values newNamedConstantArray];
    for (unsigned group = 0; group < 2; group++)
        for (id<RMConstantValue> v in (group ? named : indexed)) {
            const uint64_t bytes = raw::constantBytes(v.dataType);
            if (!bytes || !v.data)
                return why = "a constant of type " + std::to_string(uint64_t(v.dataType)) + " it cannot pass", false;
            out.push_back({v.dataType, group ? 0 : v.index, group ? v.name.UTF8String : nullptr, v.data, bytes});
        }
    return true;
}

NmbOfflineFunction *functionOf(const NmbOfflineLibrary *library, const raw::RequestStage &s,
                               std::vector<NmbConstant> &constants, MTLFunctionConstantValues *__strong &values,
                               std::string &why)
{
    if (!s.specialized) {
        if (!s.constants.empty())
            return why = "constants of a function made without them", nullptr;
        return s.name == s.function
                   ? nmb_offline_function_new(library, s.function.c_str())
                   : nmb_offline_function_new_specialized(library, s.function.c_str(), s.name.c_str(), nullptr, 0);
    }
    if (!(values = raw::constantValuesOf(s.constants)))
        return why = "constants it cannot read: " + s.constants, nullptr;
    if (!constantsOf(values, constants, why))
        return nullptr;
    return nmb_offline_function_new_specialized(library, s.function.c_str(),
                                                s.name != s.function ? s.name.c_str() : nullptr, constants.data(),
                                                uint32_t(constants.size()));
}

// vertexAttributes (`stageInput` false) or stageInputAttributes as the translator answers them; nullopt for nil
raw::AttributeList attributesOf(const NmbOfflineFunction *f, bool stageInput)
{
    const int64_t count = stageInput ? nmb_offline_function_stage_input_attribute_count(f)
                                     : nmb_offline_function_vertex_attribute_count(f);
    if (count < 0)
        return std::nullopt;
    std::vector<raw::FunctionAttributeTag> out;
    for (uint32_t i = 0; i < uint32_t(count); i++) {
        char name[256];
        uint64_t info[5] = {};
        const size_t named = stageInput ? nmb_offline_function_stage_input_attribute(f, i, name, sizeof name, info)
                                        : nmb_offline_function_vertex_attribute(f, i, name, sizeof name, info);
        if (!named)
            return std::nullopt;
        raw::FunctionAttributeTag a;
        a.name = name;
        a.index = uint8_t(info[0]);
        a.type = uint8_t(info[1]);
        a.active = info[2];
        a.patchData = info[3];
        a.controlPointData = info[4];
        out.push_back(std::move(a));
    }
    return out;
}

// The stage input as the offline descriptor takes it (raw/runtime/RawOffline.mm computeDescriptor)
void stageInputOf(const raw::StageInFields &in, NmbOfflineComputePipelineDescriptor &d)
{
    for (uint32_t i = 0; i < 31; ++i) {
        const raw::AttributeFields &a = in.attributes[i];
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

// A request's stages as the translator's functions, each library read once from its AIR slice; all released with it
class OfflineStages {
public:
    OfflineStages() = default;
    OfflineStages(const OfflineStages &) = delete;
    OfflineStages &operator=(const OfflineStages &) = delete;

    ~OfflineStages()
    {
        for (NmbOfflineFunction *f : functions_)
            nmb_offline_function_release(f);
        for (NmbOfflineLibrary *l : libraries_)
            if (l)
                nmb_offline_library_release(l);
    }

    bool make(const raw::CompileRequest &r, const std::string &root, std::string &why)
    {
        libraries_.assign(r.libraries.size(), nullptr);
        held_.resize(r.libraries.size());
        for (const raw::RequestStage &s : r.stages) {
            NmbOfflineLibrary *&library = libraries_[s.library];
            if (!library) {
                const uint8_t *slice = nullptr;
                size_t size = 0;
                if (!airSliceOf(r.libraries[s.library], root, held_[s.library], slice, size, why))
                    return false;
                if (!(library = nmb_offline_library_new(slice, size)))
                    return why = std::string("a library the translator does not read: ") + nmb_last_error(), false;
            }
            MTLFunctionConstantValues *values = nil;
            NmbOfflineFunction *f = functionOf(library, s, constants_.emplace_back(), values, why);
            if (values)
                [values_ addObject:values];
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
    std::vector<NSData *> held_;
    std::vector<NmbOfflineLibrary *> libraries_;
    std::deque<std::vector<NmbConstant>> constants_;  // what each function was made with, held as long as it is
    NSMutableArray *values_ = [NSMutableArray array];
    std::vector<NmbOfflineFunction *> functions_;
};

bool collectEncoder(uint32_t index, bool argumentBuffer, const NmbArgumentLayout *layouts, uint32_t count,
                    const char *refusal, void *ctx)
{
    static_assert(sizeof(raw::ArgumentRun) == sizeof(NmbArgumentRun), "the bridge's runs");
    raw::BufferEncoder e;
    e.index = index;
    e.argumentBuffer = argumentBuffer;
    e.refusal = refusal ? refusal : "";
    for (uint32_t i = 0; i < count; i++) {
        const auto *runs = reinterpret_cast<const raw::ArgumentRun *>(layouts[i].runs);
        e.layouts.push_back({layouts[i].encoded_length, layouts[i].alignment,
                             std::vector<raw::ArgumentRun>(runs, runs + layouts[i].run_count)});
    }
    static_cast<raw::BufferEncoders *>(ctx)->push_back(std::move(e));
    return true;
}

}  // namespace

int32_t makeBufferEncoders(const Prepared &p, const std::string &systemRoot, std::string &entry, std::string &why)
{
    @autoreleasepool {
        OfflineStages stages;
        if (!stages.make(p.request, systemRoot, why))
            return TegraGM20BCompileUnavailable;
        raw::BufferEncoders encoders;
        const bool made = nmb_offline_function_buffer_encoders(stages.function(0), collectEncoder, &encoders);
        if (!made)
            why = "the translator does not decode " + p.request.stages[0].function + ": " + nmb_last_error();
        const std::string blob = raw::encodeBufferEncoders(encoders);
        entry = raw::encodeReflectionEntry(p.key, made ? &blob : nullptr, why);
        return made ? TegraGM20BCompileMade : TegraGM20BCompileRefused;
    }
}

int32_t makeAttributes(const Prepared &p, const std::string &systemRoot, std::string &entry, std::string &why)
{
    @autoreleasepool {
        OfflineStages stages;
        if (!stages.make(p.request, systemRoot, why))
            return TegraGM20BCompileUnavailable;
        const NmbOfflineFunction *function = stages.function(0);
        const std::string blob = raw::encodeAttributeLists(attributesOf(function, false), attributesOf(function, true));
        entry = raw::encodeReflectionEntry(p.key, &blob, why);
        return TegraGM20BCompileMade;
    }
}

int32_t makeFacts(const Prepared &p, const std::string &systemRoot, std::string &entry, std::string &why)
{
    @autoreleasepool {
        OfflineStages stages;
        if (!stages.make(p.request, systemRoot, why))
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
        const size_t visible =
            size_t(std::count_if(p.request.stages.begin() + 1, p.request.stages.end(), [](const raw::RequestStage &s) {
                return s.tag == "kl";
            }));
        const size_t hidden =
            size_t(std::count_if(p.request.stages.begin() + 1, p.request.stages.end(), [](const raw::RequestStage &s) {
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
        entry = raw::encodeFactsEntry(p.key, made ? facts : nullptr, why);
        return made ? TegraGM20BCompileMade : TegraGM20BCompileRefused;
    }
}

}  // namespace switch_compiler
