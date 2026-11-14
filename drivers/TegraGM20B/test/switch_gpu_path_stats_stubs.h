// SPDX-License-Identifier: MIT
// What SwitchGpuPathStats.cpp calls in a debug kernel, on the host: the backings' counted IOLock, the clock, and the
// sysctl registration it makes once.
#pragma once
#include "gm20b_coherency_stubs.h"

inline uint64_t mach_absolute_time()
{
	static std::atomic<uint64_t> now;
	return now.fetch_add(1, std::memory_order_relaxed) + 1;
}

#define OID_AUTO (-1)
#define CTLTYPE_STRING 3
#define CTLFLAG_RD 0x80000000
#define CTLFLAG_KERN 0x01000000
#define CTLFLAG_LOCKED 0x00800000
#define CTLFLAG_OID2 0x00400000
#define SYSCTL_OID_VERSION 1

struct sysctl_req;
struct sysctl_oid;
#define SYSCTL_HANDLER_ARGS (struct sysctl_oid * oidp, void *arg1, int arg2, struct sysctl_req *req)
#define SYSCTL_OUT(req, pointer, bytes) ((void)(req), (void)(pointer), (void)(bytes), 0)

struct sysctl_oid_list {
	struct sysctl_oid *slh_first;
};

struct sysctl_oid {
	struct sysctl_oid_list *oid_parent;

	struct {
		struct sysctl_oid *sle_next;
	} oid_link;

	int oid_number;
	int oid_kind;
	void *oid_arg1;
	int oid_arg2;
	const char *oid_name;
	int(*oid_handler) SYSCTL_HANDLER_ARGS;
	const char *oid_fmt;
	const char *oid_descr;
	int oid_version;
	int oid_refcnt;
};

inline sysctl_oid_list sysctl__kern_children;

inline void sysctl_register_oid(sysctl_oid *)
{
}
