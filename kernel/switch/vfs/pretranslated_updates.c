// SPDX-License-Identifier: GPL-2.0-only
/*
 * A deploy's translated System files (kits/SwitchPretranslated/switch_pretranslated_updates.h), applied to the tree's
 * index at capture (pretranslated.c), before its tables are built.
 *
 * The boot package names each file whose translation changed since an install placed the tree. Where the tree's copy
 * already has the named CDHash there is nothing to do. Otherwise the tree's copy is one no trust cache of this boot
 * package names, so it must never be served: an update that stands takes over the file's records, and one that does not
 * takes the file out of the redirect, so the System file runs translated at load time, as with the tree off. Either
 * way one kernel line names the file (tools/devicerun/pretranslated.py judges them). The fallback is Apple's file, not
 * this build's translation of it: every file the list names is there because the build edits it
 * (translate/arm64e/pretranslate/edits_<build>.json), and load-time translation applies no edit, so for locationd the
 * fallback asks for the coprocessor services the edit takes out of its capability word. A list that does not parse
 * names no file, so then the tree is off: nothing here could tell which of its copies the boot package distrusts.
 *
 * An update stands if its CDHash is pinned, it is signed as that CDHash, its file has one link, and its ids are none
 * of the tree's. The kernel then makes it and every folder above it up to the update tree's root root's, as the install
 * makes the tree's, and the tables protect them as they protect the tree.
 */
#if CONFIG_A64X
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/mount_internal.h>
#include <sys/proc.h>
#include <sys/vnode_internal.h>
#include <kern/kalloc.h>
#include <pexpert/device_tree.h>
#include <libkern/crypto/sha2.h>

#include "pretranslated_updates.h"

#define SWITCH_PT_UPDATES_FOLDER "pretranslated-updates"
#define SWITCH_PT_UPDATES_LIST   "switch-pretranslated-updates"

struct switch_pt_file_read {
    vnode_t vp;
    vfs_context_t ctx;
};

static bool switch_pt_read(void *context, void *buffer, uint32_t size, uint64_t offset)
{
    const struct switch_pt_file_read *file = context;
    int resid = 0;
    return !vn_rdwr(UIO_READ, file->vp, (caddr_t)buffer, (int)size, (off_t)offset, UIO_SYSSPACE, IO_NODELOCKED,
                    vfs_context_ucred(file->ctx), &resid, current_proc()) &&
           resid == 0;
}

static void *switch_pt_allocate(size_t size)
{
    return kalloc_data(size, Z_WAITOK);
}

static void switch_pt_release_bytes(void *bytes, size_t size)
{
    kfree_data(bytes, size);
}

static void switch_pt_sha256(const void *bytes, size_t size, uint8_t digest[SWITCH_PT_SHA256])
{
    SHA256_CTX sha;
    SHA256_Init(&sha);
    SHA256_Update(&sha, bytes, size);
    SHA256_Final(digest, &sha);
}

/* Root:wheel, writable by its owner alone: as the install leaves the tree. */
static int switch_pt_adopt(vnode_t vp, vfs_context_t ctx)
{
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_uid);
    VATTR_WANTED(&va, va_gid);
    VATTR_WANTED(&va, va_mode);
    int error = vnode_getattr(vp, &va, ctx);
    if (error) {
        return error;
    }
    const mode_t mode = (mode_t)(va.va_mode & 07777 & ~(S_ISUID | S_ISGID | S_IWGRP | S_IWOTH));
    if (va.va_uid == 0 && va.va_gid == 0 && (va.va_mode & 07777) == mode) {
        return 0;
    }
    VATTR_INIT(&va);
    VATTR_SET(&va, va_uid, 0);
    VATTR_SET(&va, va_gid, 0);
    VATTR_SET(&va, va_mode, mode);
    return vnode_setattr(vp, &va, ctx);
}

static bool switch_pt_data_id_used(const struct switch_pt_index *index, const struct switch_pt_applied *applied,
                                   uint64_t id)
{
    for (uint32_t i = 0; i < index->count; ++i) {
        if (switch_pt_file_at(index, i).data_id == id) {
            return true;
        }
    }
    for (uint32_t i = 0; i < index->dirs; ++i) {
        if (switch_pt_directory_at(index, i) == id) {
            return true;
        }
    }
    for (uint32_t i = 0; i < applied->updates; ++i) {
        if (applied->replaced[i] == id) {
            return true;
        }
    }
    return false;
}

/* The update's folders, its own first, up to the update tree's root (`root_id`) included. Every vnode put in
 * `folders` (*depth of them) holds an iocount for the caller to drop. */
static int switch_pt_update_folders(vnode_t copy, uint64_t root_id, mount_t data, vnode_t *folders, uint64_t *ids,
                                    uint32_t *depth, vfs_context_t ctx)
{
    *depth = 0;
    for (vnode_t at = vnode_getparent(copy); at != NULLVP; at = vnode_getparent(at)) {
        folders[(*depth)++] = at;
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_fileid);
        if (vnode_mount(at) != data || !vnode_isdir(at) || vnode_getattr(at, &va, ctx) ||
            !VATTR_IS_SUPPORTED(&va, va_fileid)) {
            return ENOTDIR;
        }
        ids[*depth - 1] = va.va_fileid;
        if (va.va_fileid == root_id) {
            return 0;
        }
        if (*depth == SWITCH_PT_MAX_UPDATE_DEPTH) {
            return E2BIG;
        }
    }
    return ENOENT;
}

static void switch_pt_note_folder(struct switch_pt_applied *applied, uint64_t id)
{
    for (uint32_t i = 0; i < applied->dir_count; ++i) {
        if (applied->dirs[i] == id) {
            return;
        }
    }
    applied->dirs[applied->dir_count++] = id;
}

/* One update for the tree's file `listed`: its checks, then its folders and file made root's and the file's records
 * pointed at it. Returns why it does not stand, or NULL. */
static const char *switch_pt_take(vnode_t updates, uint64_t root_id, uint8_t *raw, const struct switch_pt_index *index,
                                  const struct switch_pt_file *listed, const struct switch_pt_update *update,
                                  const struct switch_pt_volumes *volumes, struct switch_pt_applied *applied,
                                  vfs_context_t ctx)
{
    if (!switch_cdhash_listed(volumes->trusted, update->cdhash)) {
        return "CDHash not pinned";
    }
    vnode_t system = NULLVP, copy = NULLVP;
    vnode_t folders[SWITCH_PT_MAX_UPDATE_DEPTH];
    uint64_t folder_ids[SWITCH_PT_MAX_UPDATE_DEPTH];
    uint32_t depth = 0;
    const char *failed = NULL;
    struct vnode_attr va;
    if (vnode_lookup(update->path, VNODE_LOOKUP_NOFOLLOW_ANY, &system, ctx) || vnode_mount(system) != volumes->system ||
        !vnode_isreg(system)) {
        failed = "System file";
    }
    if (!failed) {
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_fileid);
        if (vnode_getattr(system, &va, ctx) || !VATTR_IS_SUPPORTED(&va, va_fileid) ||
            va.va_fileid != listed->system_id) {
            failed = "System file id";
        }
    }
    for (uint32_t i = 0; !failed && i < applied->updates; ++i) {
        if (applied->systems[i] == listed->system_id) {
            failed = "a hard link of an update already taken";
        }
    }
    if (!failed && (vnode_lookupat(switch_pt_update_relative(update), SWITCH_PT_LOOKUP, &copy, ctx, updates) ||
                    vnode_mount(copy) != volumes->data || !vnode_isreg(copy) || vnode_isnamedstream(copy))) {
        failed = "update file";
    }
    uint64_t copy_id = 0, size = 0;
    if (!failed) {
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_fileid);
        VATTR_WANTED(&va, va_nlink);
        VATTR_WANTED(&va, va_data_size);
        if (vnode_getattr(copy, &va, ctx) || !VATTR_IS_SUPPORTED(&va, va_fileid) ||
            !VATTR_IS_SUPPORTED(&va, va_nlink) || !VATTR_IS_SUPPORTED(&va, va_data_size) || va.va_nlink != 1) {
            failed = "update file ids";
        }
        copy_id = va.va_fileid;
        size = va.va_data_size;
    }
    if (!failed && switch_pt_update_folders(copy, root_id, volumes->data, folders, folder_ids, &depth, ctx)) {
        failed = "update folders";
    }
    if (!failed) {
        struct switch_pt_file_read file = {copy, ctx};
        const struct switch_pt_reader reader = {&file, switch_pt_read, switch_pt_allocate, switch_pt_release_bytes,
                                                switch_pt_sha256};
        if (!switch_pt_signed_as_cdhash(&reader, size, update->cdhash)) {
            failed = "not signed as its CDHash";
        }
    }
    if (!failed && switch_pt_data_id_used(index, applied, copy_id)) {
        failed = "update file id";
    }
    for (uint32_t i = 0; !failed && i < depth; ++i) {
        if (switch_pt_adopt(folders[i], ctx)) {
            failed = "update ownership";
        }
    }
    if (!failed && switch_pt_adopt(copy, ctx)) {
        failed = "update ownership";
    }
    uint64_t replaced = 0;
    if (!failed && !switch_pt_index_point(raw, index, listed->system_id, copy_id, update->cdhash, &replaced)) {
        failed = "not in the tree";
    }
    if (!failed) {
        applied->systems[applied->updates] = listed->system_id;
        applied->replaced[applied->updates] = replaced;
        ++applied->updates;
        for (uint32_t i = 0; i < depth; ++i) {
            switch_pt_note_folder(applied, folder_ids[i]);
        }
    }
    for (uint32_t i = 0; i < depth; ++i) {
        vnode_put(folders[i]);
    }
    if (copy) {
        vnode_put(copy);
    }
    if (system) {
        vnode_put(system);
    }
    return failed;
}

static const char *switch_pt_reason(int error)
{
    switch (error) {
    case ENOENT:
        return "no such file";
    case EPERM:
        return "not root:wheel, or writable by group or others";
    case ENOTDIR:
        return "not a folder on Data";
    default:
        return "unreadable";
    }
}

/* The update tree under Data's Switch/ (the tree's parent, which must be root's as the tree is): its vnode, with an
 * iocount, and its id; or an error, with *where naming what failed. */
static int switch_pt_open_updates(vnode_t tree, mount_t data, vnode_t *updates, uint64_t *root_id, const char **where,
                                  vfs_context_t ctx)
{
    vnode_t data_switch = vnode_getparent(tree);
    uint64_t switch_id = 0;
    *where = "/private/var/Switch";
    int error = data_switch ? switch_pt_owned_by_root(data_switch, VDIR, ctx, &switch_id, NULL) : ENOENT;
    if (!error) {
        *where = "/private/var/Switch/" SWITCH_PT_UPDATES_FOLDER;
        error = vnode_lookupat(SWITCH_PT_UPDATES_FOLDER, SWITCH_PT_LOOKUP, updates, ctx, data_switch);
    }
    if (!error && (vnode_mount(*updates) != data || !vnode_isdir(*updates))) {
        error = ENOTDIR;
    }
    if (!error) {
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_fileid);
        error = vnode_getattr(*updates, &va, ctx);
        if (!error && !VATTR_IS_SUPPORTED(&va, va_fileid)) {
            error = ENOTDIR;
        }
        *root_id = va.va_fileid;
    }
    if (error && *updates) {
        vnode_put(*updates);
        *updates = NULLVP;
    }
    if (data_switch) {
        vnode_put(data_switch);
    }
    return error;
}

/* The tree's record of the System file at `update`'s path: by the path the index spells, else by the file's id, so a
 * hard link the index spells otherwise is found as well. */
static int64_t switch_pt_listed(const struct switch_pt_index *index, const struct switch_pt_update *update,
                                const struct switch_pt_volumes *volumes, vfs_context_t ctx)
{
    const int64_t record = switch_pt_index_find(index, update->path, update->length);
    if (record >= 0) {
        return record;
    }
    vnode_t system = NULLVP;
    int64_t found = -1;
    if (!vnode_lookup(update->path, VNODE_LOOKUP_NOFOLLOW_ANY, &system, ctx) &&
        vnode_mount(system) == volumes->system) {
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_fileid);
        if (!vnode_getattr(system, &va, ctx) && VATTR_IS_SUPPORTED(&va, va_fileid)) {
            found = switch_pt_index_find_id(index, va.va_fileid);
        }
    }
    if (system) {
        vnode_put(system);
    }
    return found;
}

int switch_pt_apply_updates(vnode_t tree, uint8_t *raw, const struct switch_pt_index *index,
                            const struct switch_pt_volumes *volumes, struct switch_pt_applied *applied,
                            vfs_context_t ctx)
{
    DTEntry chosen;
    const void *bytes = NULL;
    unsigned int size = 0;
    if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
        SecureDTGetProperty(chosen, SWITCH_PT_UPDATES_LIST, &bytes, &size) != kSuccess) {
        return 0;
    }
    struct switch_pt_updates list;
    const enum switch_pt_error parsed = switch_pt_updates_parse((const uint8_t *)bytes, size, &list);
    if (parsed != SWITCH_PT_OK) {
        return (int)parsed;
    }
    int64_t records[SWITCH_PT_MAX_UPDATES];
    uint32_t needed = 0;
    for (uint32_t i = 0; i < list.count; ++i) {
        const struct switch_pt_update update = switch_pt_update_at(&list, i);
        records[i] = switch_pt_listed(index, &update, volumes, ctx);
        if (records[i] < 0) {
            printf("SwitchPretranslated: %s not updated (not in the tree)\n", update.path);
        } else if (memcmp(switch_pt_file_at(index, (uint32_t)records[i]).cdhash, update.cdhash, SWITCH_PT_CDHASH) ==
                   0) {
            records[i] = -1;
        } else {
            ++needed;
        }
    }
    if (!needed) {
        return 0;
    }
    vnode_t updates = NULLVP;
    uint64_t root_id = 0;
    const char *where = NULL;
    const int error = switch_pt_open_updates(tree, volumes->data, &updates, &root_id, &where, ctx);
    if (error) {
        printf("SwitchPretranslated: %u of %u updates needed, none applied (%s: %s)\n", needed, list.count, where,
               switch_pt_reason(error));
    }
    for (uint32_t i = 0; i < list.count; ++i) {
        if (records[i] < 0) {
            continue;
        }
        const struct switch_pt_update update = switch_pt_update_at(&list, i);
        const struct switch_pt_file listed = switch_pt_file_at(index, (uint32_t)records[i]);
        const char *failed = NULL;
        if (memcmp(listed.cdhash, update.cdhash, SWITCH_PT_CDHASH) != 0) {
            failed = error ? "no update folder"
                           : switch_pt_take(updates, root_id, raw, index, &listed, &update, volumes, applied, ctx);
        }
        if (!failed) {
            printf("SwitchPretranslated: %s from the deploy's update\n", update.path);
            continue;
        }
        switch_pt_index_unlist(raw, index, listed.system_id, i);
        printf("SwitchPretranslated: %s not updated (%s); Apple's file runs translated at load time, without this "
               "build's edit of it\n",
               update.path, failed);
    }
    if (updates) {
        vnode_put(updates);
    }
    return 0;
}
#endif
