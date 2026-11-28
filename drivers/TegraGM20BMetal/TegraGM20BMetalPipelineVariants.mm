// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalPipelineVariants.h"
#import "TegraGM20BMetalStatesKey.h"
#include "word_wait.h"

namespace {

using RenderVariant = gm20b::Variant<gm20b::Pipeline>;

// A variant the compiler refused (`failure` empty) or no compiler made: the command buffers that draw it fail, and its
// key is named once
void settleUnmade(RenderVariant &variant, const std::string &key, const std::string &failure)
{
    SWITCH_REFUSE_DETAIL("gm20b", "state.variant", gm20b::Pack::fnv(key.data(), key.size()),
                         "the shader compiler %s the variant %s%s%s: the command buffers that draw it fail",
                         failure.empty() ? "refused" : "made none of", key.c_str(), failure.empty() ? "" : ": ",
                         failure.c_str());
    if (failure.empty())
        variant.publish(nullptr);
    else
        variant.fail();
}

// A compiler's answer about a key holds for the process: refused or not made, the variant stays so. An ask that found no
// compiler to ask is no answer: the next pass that needs the variant asks again (renderVariant's reopen), and that ask
// checks the compiler as a state's creation does (TegraGM20BMetalComplete.mm Completion::request).
void askCompiler(SwitchMetalPipeline *p, const std::shared_ptr<RenderVariant> &variant,
                 const gm20b::RenderVariant &forms, qos_class_t asker)
{
    gm20b::Completion *completion = gRM->completion.get();
    if (!completion) {
        variant->failUnserved();
        return;
    }
    const std::string key = gm20bStateKey(p) + forms.name();
#if SWITCH_METAL_STATS
    gm20bNoteMiss("render variant", gm20b::Pack::fnv(key.data(), key.size()), key, p.label);
#endif
    gm20b::Completion::Ask request;
    request.descriptor = gm20bMadeFrom(p);
    request.facade = request.descriptor;
    request.key = key;
    request.caTransform = p->_caTransform;
    request.own = !p->_packable;
    request.variant = forms;
    request.resolveDescriptor = true;
    request.qos = uint32_t(asker);
    request.bindWait = [variant](std::function<bool()> wait) {
        variant->setWait(std::move(wait));
    };
    completion->askVariant(std::move(request), [variant, key](gm20b::Pipeline *made, const std::string &failure) {
        if (made)
            variant->publish(made);
        else if (failure == gm20b::Completion::kNoCompilerComes)
            variant->failUnserved();
        else
            settleUnmade(*variant, key, failure);
    });
}

// The placer's word holds its thread while it works on one name, so a thread waiting for that variant (a queue head's
// waitUntilScheduled: gm20bSetCompileWaits) sleeps on a turnstile that pushes the placer
std::function<bool()> placerWait(const std::shared_ptr<gm20b::OwnedWord> &placer,
                                 const std::weak_ptr<RenderVariant> &variant)
{
    return [placer, variant] {
        const std::shared_ptr<RenderVariant> pending = variant.lock();
        return pending && placer->waitOnce([&] {
            return pending->state() != RenderVariant::State::Pending;
        });
    };
}

}  // namespace

dispatch_queue_t gm20bVariantPlacer()
{
    static dispatch_queue_t placer = followingSerialQueue("com.brunostjohn.switch.gm20b.variants");
    return placer;
}

// A tier-0 record is placed where the draw selects its name, as a tier-0 hit is: no lock, no page read. Placing any other
// record can take the device lock, grow the program heap (a kernel call) and read a tier-1 record from the card; asking
// the compiler makes the state's key as text. None of that is the drawing thread's, and a cheap name never queues behind
// it. A record that found no room in the heap goes to the compiler, as a name the pack lacks does.
void gm20bPlaceVariant(SwitchMetalPipeline *p, const std::shared_ptr<RenderVariant> &variant,
                       const gm20b::RenderVariant &forms)
{
    if (p->_hasPackVariants)
        if (gm20b::Pipeline *pl = gRM->raw->variantPipelineLockFree(p->_packRun, forms.name(), p->_caTransform)) {
            variant->publish(pl);
            return;
        }
    const gm20b::RenderVariant asked = forms;
    const std::shared_ptr<RenderVariant> placing = variant;
    // the placer's threads take the committers' fixed priority and no QoS class: the compiler is asked at the class of
    // the thread that drew
    const qos_class_t asker = qos_class_self();
    dispatch_async(gm20bVariantPlacer(), ^{
        const auto placer = std::make_shared<gm20b::OwnedWord>();
        placer->setOwner(gm20b::OwnedWord::currentOwner());
        placing->setWait(placerWait(placer, placing));
        gm20b::Pipeline *pl =
            p->_hasPackVariants ? gRM->raw->variantPipeline(p->_packRun, asked.name(), p->_caTransform) : nullptr;
        if (pl)
            placing->publish(pl);
        else
            askCompiler(p, placing, asked, asker);
        placer->clear();
    });
}
