// XNU's struct sysctl_oid and the constants switch_sysctl_oid sets (bsd/sys/sysctl.h, xnu-10063.141.1).
#ifndef SWITCH_TEST_SWITCH_SYSCTL_STUBS_H
#define SWITCH_TEST_SWITCH_SYSCTL_STUBS_H

#include <stdint.h>

#define OID_AUTO           (-1)
#define CTLTYPE_INT        2
#define CTLTYPE_QUAD       4
#define CTLFLAG_RD         0x80000000
#define CTLFLAG_KERN       0x01000000
#define CTLFLAG_LOCKED     0x00800000
#define CTLFLAG_OID2       0x00400000
#define SYSCTL_OID_VERSION 1

struct sysctl_req;
struct sysctl_oid;
#define SYSCTL_HANDLER_ARGS (struct sysctl_oid * oidp, void *arg1, int arg2, struct sysctl_req *req)

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

#endif
