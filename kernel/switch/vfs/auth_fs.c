// SPDX-License-Identifier: GPL-2.0-only
/*
 * FSIOC_AUTH_FS ("is this file on an authenticated volume") is how AMFI decides on-authorized-authapfs-volume, which
 * its launch constraints accept in place of on-system-volume. On an iPad those files are on the sealed System volume or
 * on a cryptex APFS grafted after checking its manifest. Ours are not, so a trust module the boot package carries
 * authenticates each one instead, and VNOP_IOCTL answers for:
 *  - the two cryptex mounts (SwitchRawMount.h attaches and mounts them instead of grafting): a file signed with a
 *    CDHash from that cryptex's own trust modules, whose UUIDs /chosen/switch-os-cryptex-trust and
 *    switch-app-cryptex-trust list;
 *  - iOS 27's pretranslated tree (switch_pretranslated_authentic).
 * Everything else goes on to its file system.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/mount_internal.h>
#include <sys/vnode_internal.h>
#include <sys/ubc_internal.h>
#include <sys/trust_caches.h>
#include <libkern/amfi/amfi.h>
#include <pexpert/device_tree.h>

#include <switch/auth_fs.h>
#include <switch/pretranslated.h>

#define SWITCH_UUID_SIZE      16
#define SWITCH_SIGNATURES_MAX 4

extern mount_t native_owner_cryptex_mount[2];

/* The trust caches are AMFI's objects, laid out by its own TrustCache library: only its interface reads them. */
bool switch_trust_module_uuid(const uint8_t *cdhash, uint8_t uuid[SWITCH_UUID_SIZE])
{
    TrustCacheQueryToken_t token;
    if (query_trust_cache(kTCQueryTypeAll, cdhash, &token) != KERN_SUCCESS) {
        return false;
    }
#if XNU_12377_121_6
    return amfi->TrustCache.queryGetUUID(&token, uuid).error == kTCReturnSuccess;
#else
    return amfi->TrustCache.version >= 3 && amfi->TrustCache.getUUID(token.trustCache, uuid).error == kTCReturnSuccess;
#endif
}

static bool switch_signed_by(vnode_t vp, const uint8_t *uuids, unsigned int size)
{
    uint8_t cdhashes[SWITCH_SIGNATURES_MAX][CS_CDHASH_LEN];
    unsigned int count = 0;
    vnode_lock_spin(vp);
    for (const struct cs_blob *blob = ubc_get_cs_blobs(vp); blob && count < SWITCH_SIGNATURES_MAX;
         blob = blob->csb_next) {
        memcpy(cdhashes[count++], blob->csb_cdhash, CS_CDHASH_LEN);
    }
    vnode_unlock(vp);
    for (unsigned int i = 0; i < count; ++i) {
        uint8_t uuid[SWITCH_UUID_SIZE];
        if (!switch_trust_module_uuid(cdhashes[i], uuid)) {
            continue;
        }
        for (unsigned int at = 0; at + SWITCH_UUID_SIZE <= size; at += SWITCH_UUID_SIZE) {
            if (memcmp(uuids + at, uuid, SWITCH_UUID_SIZE) == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool switch_signed_by_chosen(vnode_t vp, const char *property)
{
    DTEntry chosen;
    const void *uuids = NULL;
    unsigned int size = 0;
    return SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
           SecureDTGetProperty(chosen, property, &uuids, &size) == kSuccess && size % SWITCH_UUID_SIZE == 0 &&
           switch_signed_by(vp, uuids, size);
}

/* The /chosen property naming a cryptex mount's trust modules, or NULL for any other mount. */
static const char *switch_cryptex_trust(vnode_t vp)
{
    static const char *const properties[2] = {"switch-os-cryptex-trust", "switch-app-cryptex-trust"};
    const mount_t mp = vnode_mount(vp);
    for (unsigned int i = 0; mp != NULL && i < 2; ++i) {
        if (mp == native_owner_cryptex_mount[i]) {
            return properties[i];
        }
    }
    return NULL;
}

static bool switch_authentic(vnode_t vp)
{
    if (vp == NULLVP) {
        return false;
    }
    const char *cryptex = switch_cryptex_trust(vp);
    if (cryptex != NULL) {
        return vnode_isreg(vp) && switch_signed_by_chosen(vp, cryptex);
    }
    return switch_pretranslated_authentic(vp);
}

/* An authvp asks for vp's volume to be its own and authenticated: here, both files authenticated. */
bool switch_auth_fs(vnode_t vp, const void *data)
{
    const fsioc_auth_fs_t *request = data;
    return request && switch_authentic(vp) && (request->authvp == NULLVP || switch_authentic(request->authvp));
}
