// SPDX-License-Identifier: GPL-2.0-only
/*
 * The kernel's socket-level NECP policies around a local-network drop, and every add and delete in the Local Network
 * rules' order window (<switch/necp.h>), debug kernels. "SwitchNECP: table <n>" lines are one dump: its head, the
 * policies of switch_necp_table_walk's groups, the flow's recorded skip, and an end line counting each group.
 * "SwitchNECP: policy" lines are the adds and deletes in the window, in the order NECP made them.
 */
#include <switch/necp.h>

#include <sys/systm.h>
#include <sys/proc_internal.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/vm.h>
#include <net/if.h>
#include <netinet/in.h>
#include <net/necp.h>
#include <os/log.h>

#if DEVELOPMENT || DEBUG

static struct switch_necp_timeline switch_necp_timeline;
static uint64_t switch_necp_dumps;

static const char *switch_necp_result_name(necp_kernel_policy_result result)
{
    switch (result) {
    case NECP_KERNEL_POLICY_RESULT_PASS:
        return "pass";
    case NECP_KERNEL_POLICY_RESULT_SKIP:
        return "skip";
    case NECP_KERNEL_POLICY_RESULT_DROP:
        return "drop";
    case NECP_KERNEL_POLICY_RESULT_SOCKET_DIVERT:
        return "divert";
    case NECP_KERNEL_POLICY_RESULT_SOCKET_FILTER:
        return "filter";
    case NECP_KERNEL_POLICY_RESULT_IP_TUNNEL:
        return "tunnel";
    case NECP_KERNEL_POLICY_RESULT_IP_FILTER:
        return "ip-filter";
    case NECP_KERNEL_POLICY_RESULT_SOCKET_SCOPED:
        return "scoped";
    case NECP_KERNEL_POLICY_RESULT_ROUTE_RULES:
        return "route-rules";
    case NECP_KERNEL_POLICY_RESULT_USE_NETAGENT:
        return "netagent";
    case NECP_KERNEL_POLICY_RESULT_NETAGENT_SCOPED:
        return "netagent-scoped";
    case NECP_KERNEL_POLICY_RESULT_SCOPED_DIRECT:
        return "scoped-direct";
    case NECP_KERNEL_POLICY_RESULT_ALLOW_UNENTITLED:
        return "allow-unentitled";
    case NECP_KERNEL_POLICY_RESULT_REMOVE_NETAGENT:
        return "remove-netagent";
    case NECP_KERNEL_POLICY_RESULT_REMOVE_NETAGENT_TYPE:
        return "remove-netagent-type";
    default:
        return "?";
    }
}

/* parameter is the result's 32-bit parameter: the order a skip skips to, a drop's flags. */
static void switch_necp_table_line(uint32_t dump, const char *role, const struct necp_kernel_socket_policy *policy)
{
    os_log(OS_LOG_DEFAULT,
           "SwitchNECP: table %u %s id %u session-order %u order %u conditions 0x%llx negated 0x%llx result %u (%s) "
           "parameter %u session-pid %d",
           dump, role, policy->id, policy->session_order, policy->order, policy->condition_mask,
           policy->condition_negated_mask, policy->result, switch_necp_result_name(policy->result),
           policy->result_parameter.skip_policy_order, policy->session_pid);
}

static const void *switch_necp_next(const void *policy)
{
    return LIST_NEXT((const struct necp_kernel_socket_policy *)policy, chain);
}

static struct switch_necp_row switch_necp_row_of(const void *policy)
{
    const struct necp_kernel_socket_policy *p = policy;
    return (struct switch_necp_row){
        .session_order = p->session_order, .order = p->order, .skip = p->result == NECP_KERNEL_POLICY_RESULT_SKIP};
}

static void switch_necp_print_policy(void *context, const void *policy)
{
    switch_necp_table_line(*(const uint32_t *)context, "policy", policy);
}

/* NECP records a flow's first skip only, from any session; the last skip taken inside the dropping policy's session
 * is what decides whether a skip ahead of the drop jumped over the order-18075 one. */
static void switch_necp_describe_last_skip(const struct switch_necp_drop *drop, char *text, size_t size)
{
    const struct necp_kernel_socket_policy *skip = drop->last_skip;
    if (skip == NULL || skip->session_order != drop->session_order) {
        snprintf(text, size, "none");
        return;
    }
    snprintf(text, size, "id %u order %u to %u", skip->id, skip->order, skip->result_parameter.skip_policy_order);
}

static void switch_necp_flow_skip_line(uint32_t dump, const struct switch_necp_drop *drop)
{
    if (drop->skip_id == NECP_KERNEL_POLICY_ID_NONE || drop->skip_id == UINT32_MAX) {
        os_log(OS_LOG_DEFAULT, "SwitchNECP: table %u skip none", dump);
        return;
    }
    for (const struct necp_kernel_socket_policy *policy = drop->policies; policy != NULL;
         policy = LIST_NEXT(policy, chain)) {
        if (policy->id == drop->skip_id) {
            switch_necp_table_line(dump, "skip", policy);
            return;
        }
    }
    os_log(OS_LOG_DEFAULT, "SwitchNECP: table %u skip id %u is in no policy", dump, drop->skip_id);
}

void switch_necp_table_dump(const struct switch_necp_drop *drop)
{
    const uint32_t generation = __atomic_load_n(&switch_necp_timeline.generation, __ATOMIC_RELAXED);
    uint32_t dump = switch_necp_dump_claim(&switch_necp_dumps, generation);
    if (dump == 0) {
        return;
    }
    char last_skip[64];
    switch_necp_describe_last_skip(drop, last_skip, sizeof(last_skip));
    os_log(OS_LOG_DEFAULT,
           "SwitchNECP: table %u head of %u: drop site %u pid %d policy id %u session-order %u order %u "
           "session-skip %s flow-skip id %u generation %u suppressed %u",
           dump, SWITCH_NECP_TABLE_DUMPS, drop->site, drop->pid, drop->policy_id, drop->session_order, drop->order,
           last_skip, drop->skip_id, generation, __atomic_load_n(&switch_necp_timeline.suppressed, __ATOMIC_RELAXED));

    const struct switch_necp_table_walker walker = {
        .next = switch_necp_next, .row = switch_necp_row_of, .print = switch_necp_print_policy, .context = &dump};
    struct switch_necp_table_counts counts = {};
    switch_necp_table_walk(drop->policies, drop->session_order, &walker, &counts);
    switch_necp_flow_skip_line(dump, drop);
    os_log(OS_LOG_DEFAULT,
           "SwitchNECP: table %u end: walked %u policies; printed %u of %u in the session's window, %u of %u in other "
           "sessions' window, %u of %u session skips outside it",
           dump, counts.walked, counts.printed[SWITCH_NECP_TABLE_OWN_WINDOW],
           counts.wanted[SWITCH_NECP_TABLE_OWN_WINDOW], counts.printed[SWITCH_NECP_TABLE_OTHER_WINDOW],
           counts.wanted[SWITCH_NECP_TABLE_OTHER_WINDOW], counts.printed[SWITCH_NECP_TABLE_OWN_SKIP],
           counts.wanted[SWITCH_NECP_TABLE_OWN_SKIP]);
}

void switch_necp_policy_changed(const struct necp_kernel_socket_policy *policy, bool added)
{
    uint32_t generation = 0;
    if (!switch_necp_policy_account(&switch_necp_timeline, policy->order, &generation)) {
        if (switch_necp_in_window(policy->order) &&
            __atomic_load_n(&switch_necp_timeline.suppressed, __ATOMIC_RELAXED) == 1u) {
            os_log(OS_LOG_DEFAULT, "SwitchNECP: policy lines past %u are counted, not printed",
                   SWITCH_NECP_POLICY_LINES);
        }
        return;
    }
    const proc_t self = current_proc();
    os_log(OS_LOG_DEFAULT,
           "SwitchNECP: policy %s generation %u id %u session-order %u order %u conditions 0x%llx negated 0x%llx "
           "result %u (%s) parameter %u by pid %d (%s) session-pid %d",
           added ? "add" : "delete", generation, policy->id, policy->session_order, policy->order,
           policy->condition_mask, policy->condition_negated_mask, policy->result,
           switch_necp_result_name(policy->result), policy->result_parameter.skip_policy_order, proc_getpid(self),
           proc_best_name(self), policy->session_pid);
}

#endif /* DEVELOPMENT || DEBUG */
