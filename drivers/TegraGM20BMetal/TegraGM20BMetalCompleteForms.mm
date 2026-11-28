// SPDX-License-Identifier: MIT
// Completion's background forms (TegraGM20BMetalComplete.h askForms): what a pack gives a state of its own beside its
// record, asked of the shader compiler for a state it made once that state is placed.
#import "TegraGM20BMetalComplete.h"

#include <pthread/qos.h>
#include <array>
#include "TegraGM20BMetalInternal.h"
#include "gpu.h"
#include "record.h"
#include "remote_ask.h"

namespace gm20b {
namespace {

// Whether `r`'s full form (RequestForm::Full) adds what directRecord (direct.cpp) adds a pack build's record, as far as
// `r` shows it: a program variant, which programVariants makes only of a uniform range read through a residual push
// word, or its fragment program's split, which it tries only where `r` is of no storage form, set 1 arrays or argument
// tables and was not tried. Each only of a state of a vertex and a fragment stage. False where `r` is a full form
// already: split, or holding a variant beside its turned one.
bool fullFormAdds(const RuntimeRecord &r)
{
    if (r.split)
        return false;
    for (const RuntimeRecord::Variant &v : r.variants)
        if (!(v.assume & AssumeTurned))
            return false;
    const PackPipeline &p = r.pipeline;
    if (!pixelStage(p) || tessellates(p))
        return false;
    if (!r.splitTried && !p.storageMask && !p.heapArrays && !p.spillWords[0] && !p.spillWords[1])
        return true;
    const SetEntries entries = entriesOf(r);
    for (const PackUniformRange &u : p.uniform)
        if (u.entry < entries.size()) {
            const int word = bufferResidualWord(entries[u.entry]);
            if (word >= 0 && word < 32)
                return true;
        }
    return false;
}

Pipeline **twinSlot(Pipeline &made)
{
    return made.storageBlendBits ? &made.blendTwin : &made.barrierTwin;
}

}  // namespace

bool Completion::markForm(const Pipeline *made, RequestForm form)
{
    std::lock_guard<gm20b::UnfairMutex> l(formLock_);
    const auto [it, inserted] = formsAsked_.emplace(std::make_pair(made, form), FormState::Asked);
    if (inserted)
        return true;
    if (it->second != FormState::Retry)
        return false;
    it->second = FormState::Asked;
    formsRetrying_.fetch_sub(1, std::memory_order_release);
    return true;
}

bool Completion::claimRetryForm(const Pipeline *made, RequestForm form)
{
    std::lock_guard<gm20b::UnfairMutex> l(formLock_);
    const auto it = formsAsked_.find({made, form});
    if (it == formsAsked_.end() || it->second != FormState::Retry)
        return false;
    it->second = FormState::Asked;
    formsRetrying_.fetch_sub(1, std::memory_order_release);
    return true;
}

void Completion::unmarkForm(const Pipeline *made, RequestForm form)
{
    std::lock_guard<gm20b::UnfairMutex> l(formLock_);
    const auto it = formsAsked_.find({made, form});
    if (it == formsAsked_.end() || it->second == FormState::Retry)
        return;
    it->second = FormState::Retry;
    formsRetrying_.fetch_add(1, std::memory_order_release);
}

void Completion::enqueueForms(const Ask &a, const CacheKey &k, std::array<FormAsk, 2> forms, uint32_t count)
{
    // off the creating thread and the answer queue, whose blocks a form's request would hold up. Each form gets its
    // own descriptor copy on the creating thread so concurrent background asks never share an Apple descriptor object.
    std::array<Ask, 2> asked{};
    for (uint32_t i = 0; i < count; i++) {
        asked[i] = a;
        asked[i].facade = [a.facade copy];
        asked[i].descriptor = nil;
        asked[i].bindWait = nullptr;
    }
    const CacheKey key = k;
    dispatch_group_enter(forms_);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), ^{
        for (uint32_t i = 0; i < count; i++)
            if (!stopping_.load(std::memory_order_acquire))
                formOf(asked[i], key, forms[i].target, forms[i].form, false);
        dispatch_group_leave(forms_);
    });
}

void Completion::askForms(const Ask &a, const CacheKey &k, Pipeline *made, const RuntimeRecord &record)
{
    if (!made || !a.facade)
        return;
    std::array<FormAsk, 2> forms{};
    uint32_t count = 0;
    if (made->storageBits) {
        if (Pipeline *twin = __atomic_load_n(twinSlot(*made), __ATOMIC_ACQUIRE)) {
            if (hasFormsToRetry() && claimRetryForm(twin, RequestForm::BarrierFull))
                forms[count++] = {twin, RequestForm::BarrierFull};
        } else if (markForm(made, RequestForm::Barrier)) {
            forms[count++] = {made, RequestForm::Barrier};
        }
    }
    if ((fullFormAdds(record) || (record.strideTwin && fullFormAdds(*record.strideTwin))) &&
        markForm(made, RequestForm::Full))
        forms[count++] = {made, RequestForm::Full};
    if (count)
        enqueueForms(a, k, forms, count);
}

void Completion::retryForms(Ask a, Pipeline *made)
{
    if (!made || !a.facade || !hasFormsToRetry())
        return;
    std::array<FormAsk, 2> forms{};
    uint32_t count = 0;
    if (made->storageBits) {
        if (Pipeline *twin = __atomic_load_n(twinSlot(*made), __ATOMIC_ACQUIRE)) {
            if (claimRetryForm(twin, RequestForm::BarrierFull))
                forms[count++] = {twin, RequestForm::BarrierFull};
        } else if (claimRetryForm(made, RequestForm::Barrier)) {
            forms[count++] = {made, RequestForm::Barrier};
        }
    }
    if (claimRetryForm(made, RequestForm::Full))
        forms[count++] = {made, RequestForm::Full};
    if (!count)
        return;
    CacheKey k;
    if (!keyOf(a, k)) {
        for (uint32_t i = 0; i < count; i++)
            unmarkForm(forms[i].target, forms[i].form);
        return;
    }
    enqueueForms(a, k, forms, count);
}

void Completion::formOf(const Ask &a, const CacheKey &k, Pipeline *made, RequestForm form, bool again)
{
    CompileRequest r;
    StoreKey store;
    if (!again) {
        if (!renderRequest(a, k, false, r, store, false, RequestKind::Render, form))
            return;
        RuntimeRecord stored;
        std::string why;
        if (const RecordCache::Found found = fromStore(store, packKeyOf(r), stored, why);
            found != RecordCache::Found::None) {
            if (found == RecordCache::Found::Record)
                attachForm(a, k, made, stored, form);
            return;
        }
    }
    if (!remote_->serving() || !remote_->taking())
        return unmarkForm(made, form);
    if (!renderRequest(a, k, true, r, store, again, RequestKind::Render, form))
        return;
    const std::string packKey = packKeyOf(r);
    dispatch_group_enter(forms_);
    const bool sent = remote_->compileAsync(
        encodeRequest(r), false, uint32_t(QOS_CLASS_BACKGROUND),
        [this, a, k, made, store, packKey, form, again](Remote::Answer answer, const Remote::Answered &) {
            auto record = std::make_shared<RuntimeRecord>();
            std::string refusal;
            const bool stopping = stopping_.load(std::memory_order_acquire);
            const RecordCache::Found found =
                !stopping && (answer == Remote::Answer::Made || answer == Remote::Answer::Refused)
                    ? fromStore(store, packKey, *record, refusal)
                    : RecordCache::Found::None;
            const bool checkDeclined =
                !stopping && found == RecordCache::Found::None && answer != Remote::Answer::Failed && !again;
            const AskNext next = stopping ? AskNext::End
                                          : askNext(answer, found != RecordCache::Found::None, again,
                                                    checkDeclined && remote_->declined());
            if (!stopping && next == AskNext::End &&
                (answer == Remote::Answer::Unavailable || answer == Remote::Answer::Failed))
                unmarkForm(made, form);
            if (next == AskNext::End || found == RecordCache::Found::Refusal) {
                dispatch_group_leave(forms_);
                return;
            }
            const Ask asked = a;
            const CacheKey key = k;
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_BACKGROUND, 0), ^{
                if (!stopping_.load(std::memory_order_acquire)) {
                    if (next == AskNext::Use)
                        attachForm(asked, key, made, *record, form);
                    else
                        formOf(asked, key, made, form, true);
                }
                dispatch_group_leave(forms_);
            });
        });
    if (!sent) {
        unmarkForm(made, form);
        dispatch_group_leave(forms_);
    }
}

void Completion::attachForm(const Ask &a, const CacheKey &k, Pipeline *made, const RuntimeRecord &record,
                            RequestForm form)
{
    if (fullForm(form)) {
        std::string why;
        if (!device_.attachFullForm(record, why))
            once([NSString stringWithFormat:@"%s of state %016llx is not attached: %s",
                                            form == RequestForm::Full ? "a full form" : "a barrier twin's full form",
                                            (unsigned long long)k.name(), why.c_str()]);
        return;
    }
    std::string why;
    bool retry = false;
    Pipeline *twin = device_.addRecord(record, a.caTransform, why, &retry);
    if (!twin) {
        if (retry)
            unmarkForm(made, form);
        return;
    }
    if (twin == made || twin->storageBits)
        return;
    Pipeline *none = nullptr;
    __atomic_compare_exchange_n(twinSlot(*made), &none, twin, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    if ((fullFormAdds(record) || (record.strideTwin && fullFormAdds(*record.strideTwin))) &&
        markForm(twin, RequestForm::BarrierFull))
        formOf(a, k, twin, RequestForm::BarrierFull, false);
}

}  // namespace gm20b
