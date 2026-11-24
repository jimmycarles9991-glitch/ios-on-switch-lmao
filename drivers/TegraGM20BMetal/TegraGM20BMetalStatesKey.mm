// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalCompileRequest.h"
#import "TegraGM20BMetalPipelineKey.h"
#import "TegraGM20BMetalStatesKey.h"
#include "creation_cost.h"

uint64_t gm20bKeepKey(SwitchMetalPipeline *p, MTLRenderPipelineDescriptor *d, SwitchMetalFunction *vertex,
                      SwitchMetalFunction *fragment)
{
    p->_fields = gm20b::keyFieldsOf(d, vertex->_patchType);
    p->_vertexFunction = vertex;
    p->_fragmentFunction = fragment;
    MTLLinkedFunctions *v = d.vertexLinkedFunctions, *f = d.fragmentLinkedFunctions;
    NSArray *lists[4] = {v.functions, v.privateFunctions, f.functions, f.privateFunctions};
    bool linked = false;
    for (int k = 0; k < 4; k++)
        if (lists[k].count) {
            p->_linkedLists[k] = lists[k];
            linked = true;
        }
    if (linked)
        p->_linked = gm20bLinkedText(lists[0], lists[1], lists[2], lists[3]);
    return gm20b::fastKey(p->_fields, vertex->_stageHash, fragment ? fragment->_stageHash : gm20b::kKeyNoStage,
                          gm20b::linkedHash(p->_linked), gm20b::frameworkHash(vertex->_framework));
}

// A variant's ask and a log line may come from two threads at once: the first makes each, once
const std::string &gm20bStateKey(SwitchMetalPipeline *p)
{
    std::call_once(p->_keyed, [p] {
        SwitchMetalFunction *vf = p->_vertexFunction, *ff = p->_fragmentFunction;
        if (!vf)
            return;
        SWITCH_METAL_CREATION_COUNT(textKeys, 1);
        const gm20b::StageKey vertex{libraryName(vf), vf->_constants};
        const gm20b::StageKey fragment{ff ? libraryName(ff) : std::string(), ff ? ff->_constants : std::string()};
        p->_key = gm20b::pipelineKey(p->_fields, &vertex, ff ? &fragment : nullptr, p->_linked, vf->_framework);
    });
    return p->_key;
}

MTLRenderPipelineDescriptor *gm20bMadeFrom(SwitchMetalPipeline *p)
{
    std::call_once(p->_described, [p] {
        if (!p->_vertexFunction)
            return;
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        gm20b::applyFields(p->_fields, d);
        if (NSString *label = p.label)
            d.label = label;
        d.vertexFunction = (id<MTLFunction>)p->_vertexFunction;
        d.fragmentFunction = (id<MTLFunction>)p->_fragmentFunction;
        MTLLinkedFunctions *linked[2] = {[MTLLinkedFunctions linkedFunctions], [MTLLinkedFunctions linkedFunctions]};
        for (int k = 0; k < 2; k++) {
            if (NSArray *functions = p->_linkedLists[2 * k])
                linked[k].functions = functions;
            if (NSArray *functions = p->_linkedLists[2 * k + 1])
                linked[k].privateFunctions = functions;
        }
        d.vertexLinkedFunctions = linked[0];
        d.fragmentLinkedFunctions = linked[1];
        p->_madeFrom = d;
    });
    return p->_madeFrom;
}
