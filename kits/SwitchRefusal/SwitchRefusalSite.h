// SPDX-License-Identifier: BSD-2-Clause-Patent
// C++ and Objective-C++ refusal sites over SwitchRefusal.h: the (domain, reason) key folded at compile time, so a site
// that refuses on every frame costs one call and one relaxed load once its line is out, and its text is formatted only
// the first time. domain and reason must be string literals.
//
//   SWITCH_REFUSE("raw", "tex.shape", "%ux%u format %u", w, h, format);
//   SWITCH_REFUSE_DETAIL("frontend", "selector.zero", uintptr_t(sel), "-[%s %s]", cls, name);  // a line per detail
#ifndef SWITCH_REFUSAL_SITE_H
#define SWITCH_REFUSAL_SITE_H

#include <cstdint>
#include <type_traits>

#include "SwitchRefusal.h"

#define SWITCH_REFUSAL_KEY(domain, reason) (std::integral_constant<uint64_t, switch_refusal_key(domain, reason)>::value)

#define SWITCH_REFUSE(domain, reason, ...) \
    switch_refusal_k(SWITCH_REFUSAL_KEY(domain, reason), domain, reason, __VA_ARGS__)

#define SWITCH_REFUSE_DETAIL(domain, reason, detail, ...)                                                           \
    switch_refusal_k(switch_refusal_key_with(SWITCH_REFUSAL_KEY(domain, reason), uint64_t(detail)), domain, reason, \
                     __VA_ARGS__)

#endif
