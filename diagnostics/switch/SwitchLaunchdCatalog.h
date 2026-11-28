// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_LAUNCHD_CATALOG_H
#define SWITCH_LAUNCHD_CATALOG_H

#include <libkern/crypto/sha2.h>
#include <pexpert/device_tree.h>

static int switch_launchd_catalog_lookup(const char *path, vnode_t parent, vnode_t *file, vfs_context_t context)
{
    struct nameidata *nd = kalloc_type(struct nameidata, Z_WAITOK);
    if (!nd) return ENOMEM;
    NDINIT(nd, LOOKUP, OP_LOOKUP, NOFOLLOW | (parent ? NOCROSSMOUNT : 0), UIO_SYSSPACE, CAST_USER_ADDR_T(path), context);
    nd->ni_flag |= NAMEI_NOFOLLOW_ANY;
    if (parent) {
        nd->ni_dvp = parent;
        nd->ni_cnd.cn_flags |= USEDVP;
    }
    const int error = namei(nd);
    if (!error) {
        nd->ni_cnd.cn_flags &= ~USEDVP;
        *file = nd->ni_vp;
        nameidone(nd);
    }
    kfree_type(struct nameidata, nd);
    return error;
}

static int switch_launchd_catalog_file(proc_t p, vnode_t data, const char *path, size_t limit,
                                     const uint8_t *expected, char *buffer, size_t capacity)
{
    vfs_context_t context = vfs_context_current();
    vnode_t file = NULLVP;
    int error = switch_launchd_catalog_lookup(path, data, &file, context);
    if (error) return error;

    struct vnode_attr attributes;
    VATTR_INIT(&attributes);
    VATTR_WANTED(&attributes, va_data_size);
    if (!vnode_isreg(file) || vnode_mount(file) != vnode_mount(data)) error = EPERM;
    if (!error) error = vnode_getattr(file, &attributes, context);
    if (!error && !VATTR_IS_SUPPORTED(&attributes, va_data_size)) error = ENOTSUP;
    if (!error && (!attributes.va_data_size || attributes.va_data_size > limit)) error = EFBIG;

    if (!error) {
        SHA256_CTX sha;
        SHA256_Init(&sha);
        for (uint64_t at = 0; at < attributes.va_data_size;) {
            const uint64_t remaining = attributes.va_data_size - at;
            const int count = (int)(remaining < capacity ? remaining : capacity);
            int residual = 0;
            error = vn_rdwr(UIO_READ, file, buffer, count, (off_t)at, UIO_SYSSPACE, 0,
                            vfs_context_ucred(context), &residual, p);
            if (!error && residual) error = EIO;
            if (error) break;
            SHA256_Update(&sha, buffer, (size_t)count);
            at += count;
        }
        if (!error) {
            uint8_t actual[SHA256_DIGEST_LENGTH];
            SHA256_Final(actual, &sha);
            if (memcmp(actual, expected, sizeof(actual))) error = EAUTH;
        }
    }
    vnode_put(file);
    return error;
}

static int switch_launchd_catalog_pair(proc_t p, DTEntry chosen)
{
    const void *property = NULL;
    unsigned int size = 0;
    if (SecureDTGetProperty(chosen, "switch-launchd-catalog-sha256", &property, &size) != kSuccess ||
        !property || size != 2 * SHA256_DIGEST_LENGTH) return EINVAL;
    const uint8_t *expected = (const uint8_t *)property;

    vnode_t data = NULLVP;
    int error = switch_launchd_catalog_lookup("/private/var", NULLVP, &data, vfs_context_current());
    if (error) return error;
    if (!vnode_isvroot(data)) {
        vnode_put(data);
        return EPERM;
    }

    const size_t capacity = 64 * 1024;
    char *buffer = kalloc_data(capacity, Z_WAITOK);
    if (!buffer) error = ENOMEM;
    if (!error) error = switch_launchd_catalog_file(p, data, "Switch/overlay/System/Library/xpc/launchd.plist",
                                                  8 * 1024 * 1024, expected, buffer, capacity);
    if (!error) error = switch_launchd_catalog_file(p, data, "Switch/overlay/System/Library/xpc/launchd.plist.sig",
                                                  256 * 1024, expected + SHA256_DIGEST_LENGTH, buffer, capacity);
    if (buffer) kfree_data(buffer, capacity);
    vnode_put(data);
    return error;
}

#endif
