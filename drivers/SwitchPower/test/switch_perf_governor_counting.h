// SPDX-License-Identifier: BSD-2-Clause-Patent
// Forced into SwitchPerfGovernor.cpp's host build: every atomic read-modify-write and every fence it issues is counted
// (switch_gov_test_rmw, switch_gov_test_fence, defined by the test) and then issued as written. A macro does not expand
// inside its own replacement, so the inner names are the compiler's builtins.
#ifndef SWITCH_PERF_GOVERNOR_COUNTING_H
#define SWITCH_PERF_GOVERNOR_COUNTING_H

#ifdef __cplusplus
extern "C" {
#endif
void switch_gov_test_rmw(void);
void switch_gov_test_fence(void);
#ifdef __cplusplus
}
#endif

// NOLINTBEGIN(bugprone-reserved-identifier,cppcoreguidelines-macro-usage)
#define __atomic_exchange_n(...)         (switch_gov_test_rmw(), __atomic_exchange_n(__VA_ARGS__))
#define __atomic_compare_exchange_n(...) (switch_gov_test_rmw(), __atomic_compare_exchange_n(__VA_ARGS__))
#define __atomic_fetch_add(...)          (switch_gov_test_rmw(), __atomic_fetch_add(__VA_ARGS__))
#define __atomic_fetch_sub(...)          (switch_gov_test_rmw(), __atomic_fetch_sub(__VA_ARGS__))
#define __atomic_fetch_or(...)           (switch_gov_test_rmw(), __atomic_fetch_or(__VA_ARGS__))
#define __atomic_fetch_and(...)          (switch_gov_test_rmw(), __atomic_fetch_and(__VA_ARGS__))
#define __atomic_add_fetch(...)          (switch_gov_test_rmw(), __atomic_add_fetch(__VA_ARGS__))
#define __atomic_sub_fetch(...)          (switch_gov_test_rmw(), __atomic_sub_fetch(__VA_ARGS__))
#define __atomic_thread_fence(order)     (switch_gov_test_fence(), __atomic_thread_fence(order))
// NOLINTEND(bugprone-reserved-identifier,cppcoreguidelines-macro-usage)

#endif
