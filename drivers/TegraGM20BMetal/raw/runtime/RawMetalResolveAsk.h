// SPDX-License-Identifier: MIT
#pragma once
#import "RawMetalInternal.h"
#include "raw_resolve_rule.h"

// `t` (and what it views) read as data from here: a blit copies out of it, or a kernel takes it
void noteReadAsData(id<MTLTexture> t);

// What can read `into`'s texels as data: itself and the texture a view of it views, down to the memory
raw::ResolveTarget resolveTarget(RMTexture *into);

// Rules resolve.twod's and resolve.exact's view of a resolve of `rendered` (the multisampled image the pass drew) into
// `target` (`into` itself, or the level, slice or plane of it the resolve names) after a pass of `width` x `height`
raw::ResolveAsk resolveAsk(RMTexture *into, const raw::Texture &target, const raw::Texture &rendered, uint32_t width,
                           uint32_t height);
