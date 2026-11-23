// SPDX-License-Identifier: MIT
// See TegraGM20BMetalReflection.h. The objects a blob is read into are TegraGM20BMetalReflectionObjects.mm's.
#import "TegraGM20BMetalReflection.h"
#import "TegraGM20BMetalComputeState.h"
#import "TegraGM20BMetalInternal.h"

#include "compute_key.h"
#include "kernel_texture.h"
#include "reflection_blob.h"

static bool reflectionAsked(MTLPipelineOption o)
{
    return o & (MTLPipelineOptionArgumentInfo | MTLPipelineOptionBufferTypeInfo);
}

static id fromBlob(const std::string &blob, bool compute)
{
    return gm20bReflectionOf(reinterpret_cast<const uint8_t *>(blob.data()), blob.size(), compute);
}

MTLAutoreleasedRenderPipelineReflection gm20bRenderReflection(SwitchMetalPipeline *p, MTLRenderPipelineDescriptor *d,
                                                              MTLPipelineOption o)
{
    if (!reflectionAsked(o))
        return nil;
    std::string blob, why = "a function of another device, which no request can name";
    if (p->_completable) {
        uint32_t length = 0;
        if (const uint8_t *packed = gRM->raw->pack().reflection(p->_packKey, false, length))
            return gm20bReflectionOf(packed, length, false);
        if (gRM->completion && gRM->completion->reflection(gm20bRecordAsk(p, d), blob, why))
            if (id made = fromBlob(blob, false))
                return made;
    }
    os_log_error(gm20bLog(),
                 "SwitchMetal: %{public}s: no reflection of a render state, none given: %{public}s (%{public}s)",
                 getprogname(), why.c_str(), d.label.UTF8String ?: "-");
    return nil;
}

uint32_t gm20bComputeTexturesWritten(SwitchMetalComputeState *s)
{
    std::call_once(s->_writtenOnce, [s] {
        s->_texturesWritten = ~0u;
        const std::string key = gm20b::computeStateKey(s->_air, s->_constants, s->_maxThreads, s->_multipleOfWidth);
        uint32_t length = 0;
        std::string blob, why;
        const void *bytes = gRM->raw->pack().reflection(gm20b::Pack::fnv(key.data(), key.size()), true, length);
        if (!bytes && gRM->completion && gRM->completion->reflection(s->_function, s->_direct, key, blob, why)) {
            bytes = blob.data();
            length = uint32_t(blob.size());
        }
        gm20b::ReflectionView v;
        if (bytes && gm20b::readReflection(bytes, length, gm20b::kReflectionComputeStages, v))
            s->_texturesWritten = gm20b::kernelTexturesWritten(v);
    });
    return s->_texturesWritten;
}

bool gm20bComputeReflection(SwitchMetalComputeState *s, MTLPipelineOption o,
                            MTLAutoreleasedComputePipelineReflection *reflection, NSError **error)
{
    *reflection = nil;
    if (!reflectionAsked(o))
        return true;
    const std::string key = gm20b::computeStateKey(s->_air, s->_constants, s->_maxThreads, s->_multipleOfWidth);
    uint32_t length = 0;
    std::string blob, why = "no completion";
    if (const uint8_t *packed = gRM->raw->pack().reflection(gm20b::Pack::fnv(key.data(), key.size()), true, length))
        *reflection = gm20bReflectionOf(packed, length, true);
    else if (gRM->completion && gRM->completion->reflection(s->_function, s->_direct, key, blob, why))
        *reflection = fromBlob(blob, true);
    if (*reflection)
        return true;
    os_log_error(gm20bLog(), "SwitchMetal: %{public}s: no reflection of a compute state: %{public}s", getprogname(),
                 why.c_str());
    if (error) {
        NSString *text = [NSString stringWithFormat:@"no reflection: %s", why.c_str()];
        *error = [NSError errorWithDomain:MTLLibraryErrorDomain
                                     code:MTLLibraryErrorInternal
                                 userInfo:@{NSLocalizedDescriptionKey : text}];
    }
    return false;
}
