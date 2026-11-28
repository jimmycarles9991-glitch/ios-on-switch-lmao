// SPDX-License-Identifier: GPL-2.0-only
/*
 * A local-network drop NECP reports for a process (diag/necp.c, debug kernels): one "SwitchNECP:" line with the
 * process, the process NECP judged its platform conditions on, and the rule that matched. 24B119's Local Network rules
 * let a flow past their drops (orders 18090 to 18104) through an unconditional skip at 18075 to 18120, unless a per-app
 * skip at 18070 sends it to 18080; diag/necp_table.c logs the policies in that window and every add and delete there.
 */
#ifndef SWITCH_NECP_H
#define SWITCH_NECP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

struct proc;
struct necp_kernel_socket_policy;

struct switch_necp_drop {
    uint32_t site;
    int32_t pid;
    uint32_t uid;
    struct proc *process;
    struct proc *caller;
    struct proc *responsible;
    int32_t last_pid;
    int32_t responsible_pid;
    bool delegated;
    bool used_responsible;
    bool necp_platform;
    bool necp_real_platform;
    bool skip_installed;
    uint32_t policy_id;
    uint32_t session_order;
    uint32_t order;
    uint64_t conditions;
    uint64_t negated;
    uint32_t drop_flags;
    uint32_t skip_id;
    const struct necp_kernel_socket_policy *last_skip;
    const struct necp_kernel_socket_policy *policies;
};

#define SWITCH_NECP_PROBE_PIDS 256u

#define SWITCH_NECP_WINDOW_FIRST       18000u
#define SWITCH_NECP_WINDOW_LAST        18200u
#define SWITCH_NECP_TABLE_DUMPS        4u
#define SWITCH_NECP_TABLE_LINES        64u /* a dump's head, its policies, the flow's skip and its end */
#define SWITCH_NECP_TABLE_POLICY_LINES (SWITCH_NECP_TABLE_LINES - 3u)
#define SWITCH_NECP_TABLE_OWN_LINES    128u /* the dropping session's window alone may run past a dump's usual size */
/* Two installs of the networkprivacy session (about 40 policies in the window each, the second deleting the first)
 * and per-app rule changes, with room to spare. */
#define SWITCH_NECP_POLICY_LINES 256u

/* True the first time `pid` is seen while one of `slots` (zero when free) is still free. */
static inline bool switch_necp_probe_claim(int32_t *slots, unsigned count, int32_t pid)
{
    if (pid <= 0) {
        return false;
    }
    for (unsigned i = 0; i < count; i++) {
        int32_t seen = __atomic_load_n(&slots[i], __ATOMIC_RELAXED);
        if (seen == 0 &&
            __atomic_compare_exchange_n(&slots[i], &seen, pid, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            return true;
        }
        if (seen == pid) {
            return false;
        }
    }
    return false;
}

static inline bool switch_necp_in_window(uint32_t order)
{
    return order >= SWITCH_NECP_WINDOW_FIRST && order <= SWITCH_NECP_WINDOW_LAST;
}

static inline bool switch_necp_take(uint32_t *used, uint32_t cap)
{
    uint32_t seen = __atomic_load_n(used, __ATOMIC_RELAXED);
    while (seen < cap) {
        if (__atomic_compare_exchange_n(used, &seen, seen + 1u, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            return true;
        }
    }
    return false;
}

/* What a dump prints, in this order: the dropping session's policies in the window (up to SWITCH_NECP_TABLE_OWN_LINES),
 * the other sessions' policies in the window, then the dropping session's skips outside it, while
 * SWITCH_NECP_TABLE_POLICY_LINES last. */
enum switch_necp_table_group {
    SWITCH_NECP_TABLE_OWN_WINDOW,
    SWITCH_NECP_TABLE_OTHER_WINDOW,
    SWITCH_NECP_TABLE_OWN_SKIP,
    SWITCH_NECP_TABLE_GROUPS,
    SWITCH_NECP_TABLE_UNPRINTED = SWITCH_NECP_TABLE_GROUPS,
};

static inline enum switch_necp_table_group switch_necp_table_group(uint32_t session_order, uint32_t order, bool skip,
                                                                   uint32_t drop_session_order)
{
    const bool own = session_order == drop_session_order;
    if (switch_necp_in_window(order)) {
        return own ? SWITCH_NECP_TABLE_OWN_WINDOW : SWITCH_NECP_TABLE_OTHER_WINDOW;
    }
    return own && skip ? SWITCH_NECP_TABLE_OWN_SKIP : SWITCH_NECP_TABLE_UNPRINTED;
}

struct switch_necp_row {
    uint32_t session_order;
    uint32_t order;
    bool skip;
};

struct switch_necp_table_walker {
    const void *(*next)(const void *policy);
    struct switch_necp_row (*row)(const void *policy);
    void (*print)(void *context, const void *policy);
    void *context;
};

struct switch_necp_table_counts {
    uint32_t walked;
    uint32_t wanted[SWITCH_NECP_TABLE_GROUPS];
    uint32_t printed[SWITCH_NECP_TABLE_GROUPS];
};

/* One pass over the list from `first` per group, each keeping the list's order. */
static inline void switch_necp_table_walk(const void *first, uint32_t drop_session_order,
                                          const struct switch_necp_table_walker *walker,
                                          struct switch_necp_table_counts *counts)
{
    uint32_t printed = 0;
    for (unsigned group = 0; group < SWITCH_NECP_TABLE_GROUPS; group++) {
        for (const void *policy = first; policy != NULL; policy = walker->next(policy)) {
            const struct switch_necp_row row = walker->row(policy);
            if (group == 0) {
                counts->walked++;
            }
            const unsigned of =
                (unsigned)switch_necp_table_group(row.session_order, row.order, row.skip, drop_session_order);
            if (of != group) {
                continue;
            }
            counts->wanted[group]++;
            const uint32_t cap =
                group == SWITCH_NECP_TABLE_OWN_WINDOW ? SWITCH_NECP_TABLE_OWN_LINES : SWITCH_NECP_TABLE_POLICY_LINES;
            if (printed < cap) {
                walker->print(walker->context, policy);
                counts->printed[group]++;
                printed++;
            }
        }
    }
}

/* The number (from 1) of the dump a drop takes at policy-table `generation`, or 0: the first drop of a boot dumps, then
 * the first after the generation moved, SWITCH_NECP_TABLE_DUMPS in all. `state` holds the generation of the last dump
 * in its high half and the dumps taken in its low half. */
static inline uint32_t switch_necp_dump_claim(uint64_t *state, uint32_t generation)
{
    uint64_t seen = __atomic_load_n(state, __ATOMIC_RELAXED);
    for (;;) {
        const uint32_t dumps = (uint32_t)seen;
        if (dumps >= SWITCH_NECP_TABLE_DUMPS || (dumps != 0 && (uint32_t)(seen >> 32) == generation)) {
            return 0;
        }
        const uint64_t next = (uint64_t)generation << 32 | (dumps + 1u);
        if (__atomic_compare_exchange_n(state, &seen, next, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            return dumps + 1u;
        }
    }
}

struct switch_necp_timeline {
    uint32_t generation;
    uint32_t lines;
    uint32_t suppressed;
};

/* An add or delete at `order`: only one in the window moves the table's generation (what a new dump waits for) and is
 * logged, as `*generation`, until SWITCH_NECP_POLICY_LINES; past that it is counted as suppressed. */
static inline bool switch_necp_policy_account(struct switch_necp_timeline *timeline, uint32_t order,
                                              uint32_t *generation)
{
    if (!switch_necp_in_window(order)) {
        return false;
    }
    *generation = __atomic_add_fetch(&timeline->generation, 1u, __ATOMIC_RELAXED);
    if (switch_necp_take(&timeline->lines, SWITCH_NECP_POLICY_LINES)) {
        return true;
    }
    __atomic_fetch_add(&timeline->suppressed, 1u, __ATOMIC_RELAXED);
    return false;
}

#if (DEVELOPMENT || DEBUG) && defined(SWITCH_DIAG_NECP) && SWITCH_DIAG_NECP
/* Runs under NECP's policy lock (shared) and the caller's socket or client locks, where NECP itself logs and reads the
 * same process fields; it walks `policies` (necp_kernel_socket_policies) under that lock. */
void switch_necp_local_drop(const struct switch_necp_drop *drop);
void switch_necp_table_dump(const struct switch_necp_drop *drop);

/* Runs under NECP's policy lock held exclusive (necp_policy_apply_all, necp_kernel_socket_policy_delete), so the
 * table's generation never moves while a drop site walks the table. */
void switch_necp_policy_changed(const struct necp_kernel_socket_policy *policy, bool added);

/* struct necp_socket_info's last field: the last skip necp_socket_find_policy_match_with_info_locked took for it. NECP
 * zeroes the info before each evaluation and hashes it (flowhash) before the skip is set. */
#define SWITCH_NECP_INFO_FIELDS              const struct necp_kernel_socket_policy *switch_last_skip;
#define SWITCH_NECP_SKIP_TAKEN(info, policy) ((info)->switch_last_skip = (policy))

#define SWITCH_NECP_DROP_FIELDS(info, policy, skip, mask)                                                           \
    .pid = (info).pid, .delegated = (info).is_delegated, .used_responsible = (info).used_responsible_pid,           \
    .necp_platform = (info).is_platform_binary, .necp_real_platform = (info).real_is_platform_binary,               \
    .skip_installed = ((mask) & NECP_KERNEL_CONDITION_DELEGATE_IS_PLATFORM_BINARY) != 0, .policy_id = (policy)->id, \
    .session_order = (policy)->session_order, .order = (policy)->order, .conditions = (policy)->condition_mask,     \
    .negated = (policy)->condition_negated_mask, .drop_flags = (policy)->result_parameter.drop_flags,               \
    .skip_id = (skip), .last_skip = (info).switch_last_skip, .policies = LIST_FIRST(&necp_kernel_socket_policies)

#define SWITCH_NECP_CLIENT_LOCAL_DROP(info, policy, skip, caller_proc, effective, responsible_proc, mask) \
    switch_necp_local_drop(&(const struct switch_necp_drop){.site = 1,                                    \
                                                            .uid = (info).uid,                            \
                                                            .process = (effective),                       \
                                                            .caller = (caller_proc),                      \
                                                            .responsible = (responsible_proc),            \
                                                            SWITCH_NECP_DROP_FIELDS(info, policy, skip, mask)})

/* info.uid is filled only when some policy has a uid condition; the socket's credential is what NECP reads then. */
#define SWITCH_NECP_SOCKET_LOCAL_DROP(site_number, info, policy, skip, so, effective, mask)          \
    switch_necp_local_drop(&(const struct switch_necp_drop){.site = (site_number),                   \
                                                            .uid = kauth_cred_getuid((so)->so_cred), \
                                                            .process = (effective),                  \
                                                            .last_pid = (so)->last_pid,              \
                                                            .responsible_pid = (so)->so_rpid,        \
                                                            SWITCH_NECP_DROP_FIELDS(info, policy, skip, mask)})

#define SWITCH_NECP_POLICY_ADDED(policy)   switch_necp_policy_changed((policy), true)
#define SWITCH_NECP_POLICY_DELETED(policy) switch_necp_policy_changed((policy), false)
#else
#define SWITCH_NECP_INFO_FIELDS
#define SWITCH_NECP_SKIP_TAKEN(info, policy) \
    do {                                     \
    } while (0)
#define SWITCH_NECP_CLIENT_LOCAL_DROP(info, policy, skip, caller_proc, effective, responsible_proc, mask) \
    do {                                                                                                  \
    } while (0)
#define SWITCH_NECP_SOCKET_LOCAL_DROP(site_number, info, policy, skip, so, effective, mask) \
    do {                                                                                    \
    } while (0)
#define SWITCH_NECP_POLICY_ADDED(policy) \
    do {                                 \
    } while (0)
#define SWITCH_NECP_POLICY_DELETED(policy) \
    do {                                   \
    } while (0)
#endif

__END_DECLS

#endif /* SWITCH_NECP_H */
