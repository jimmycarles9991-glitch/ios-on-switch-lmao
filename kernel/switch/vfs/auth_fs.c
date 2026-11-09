// SPDX-License-Identifier: GPL-2.0-only
/*
 * FSIOC_AUTH_FS ("is this file on an authenticated volume") is how AMFI decides on-authorized-authapfs-volume, which
 * its launch constraints accept in place of on-system-volume. On an iPad those files are on the sealed System volume or
 * on a cryptex APFS grafted after checking its manifest. Ours are not, so the boot package lists the CDHashes of each
 * one's code in /chosen (tools/package/adt_edit.py), all of them also in the static trust cache it carries, and
 * VNOP_IOCTL answers for:
 *  - the two cryptex mounts (SwitchRawMount.h attaches and mounts them instead of grafting): a regular file signed with
 *    a CDHash /chosen/switch-os-cryptex-cdhashes or switch-app-cryptex-cdhashes lists;
 *  - iOS 27's pretranslated tree (switch_pretranslated_authentic).
 * Everything else goes on to its file system.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mount_internal.h>
#include <sys/vnode_internal.h>
#include <sys/ubc_internal.h>
#include <pexpert/device_tree.h>

#include <switch/auth_fs.h>
#include <switch/pretranslated.h>

extern mount_t native_owner_cryptex_mount[2];

/* Set before launchd and read-only after: the lists point into the device tree, which stays mapped. */
static struct switch_cdhash_list switch_cryptex_cdhashes[2];

struct switch_cdhash_list switch_chosen_cdhashes(const char *property)
{
    DTEntry chosen;
    const void *bytes = NULL;
    unsigned int size = 0;
    if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
        SecureDTGetProperty(chosen, property, &bytes, &size) != kSuccess) {
        size = 0;
    }
    return switch_cdhash_list_of(bytes, size);
}

void switch_auth_fs_init(void)
{
    static const char *const properties[2] = {"switch-os-cryptex-cdhashes", "switch-app-cryptex-cdhashes"};
    for (unsigned int i = 0; i < 2; ++i) {
        switch_cryptex_cdhashes[i] = switch_chosen_cdhashes(properties[i]);
        if (!switch_cryptex_cdhashes[i].count) {
            printf("SwitchAuthFS: no /chosen/%s: that cryptex's files are not authenticated\n", properties[i]);
        }
    }
}

static bool switch_signed_by(vnode_t vp, const struct switch_cdhash_list *list)
{
    bool listed = false;
    vnode_lock_spin(vp);
    for (const struct cs_blob *blob = ubc_get_cs_blobs(vp); blob && !listed; blob = blob->csb_next) {
        listed = switch_cdhash_listed(list, blob->csb_cdhash);
    }
    vnode_unlock(vp);
    return listed;
}

/* A cryptex mount's CDHash list, or NULL for any other mount. */
static const struct switch_cdhash_list *switch_cryptex_cdhashes_of(vnode_t vp)
{
    const mount_t mp = vnode_mount(vp);
    for (unsigned int i = 0; mp != NULL && i < 2; ++i) {
        if (mp == native_owner_cryptex_mount[i]) {
            return &switch_cryptex_cdhashes[i];
        }
    }
    return NULL;
}

static bool switch_authentic(vnode_t vp)
{
    if (vp == NULLVP) {
        return false;
    }
    const struct switch_cdhash_list *cryptex = switch_cryptex_cdhashes_of(vp);
    if (cryptex != NULL) {
        return vnode_isreg(vp) && switch_signed_by(vp, cryptex);
    }
    return switch_pretranslated_authentic(vp);
}

/* An authvp asks for vp's volume to be its own and authenticated: here, both files authenticated. */
bool switch_auth_fs(vnode_t vp, const void *data)
{
    const fsioc_auth_fs_t *request = data;
    return request && switch_authentic(vp) && (request->authvp == NULLVP || switch_authentic(request->authvp));
}
