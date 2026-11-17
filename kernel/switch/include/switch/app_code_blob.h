// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_APP_CODE_BLOB_H
#define SWITCH_APP_CODE_BLOB_H

#include <switch/app_code.h>
#include <switch/entitlements_factory.h>

int switch_app_code_blob(struct vnode *vp, int64_t slice)
{
    struct cs_blob *existing = ubc_cs_blob_get(vp, -1, -1, slice);
    if (existing && existing->csb_reconstituted == SWITCH_APP_CODE_METADATA) return 0;
    if (!switch_app_code_image(vp)) return EINVAL;
    int64_t coverage = 0;
    int error = switch_app_code_coverage(vp, slice, &coverage);
    if (error) return error;
    cpu_type_t cpu;
    cpu_subtype_t subtype;
    error = switch_app_code_arch(vp, slice, &cpu, &subtype);
    if (error) return error;
    void *source = NULL;
    size_t source_size = 0;
    struct switch_app_code_capabilities capabilities = {};
    error = switch_app_code_metadata(vp, &source, &source_size);
    if (error) goto out;
    error = switch_app_code_capability_decode(source, source_size, &capabilities);
    if (error) goto out;
    const uint8_t *der = source;
    size_t der_size = source_size;
    const struct cs_hash *hash = cs_find_md(CS_HASHTYPE_SHA256);
    const size_t identity_size = strlen(capabilities.identity) + 1;
    const size_t team_size = capabilities.team[0] ? strlen(capabilities.team) + 1 : 0;
    const size_t cd_size = sizeof(CS_CodeDirectory) + identity_size + team_size +
        CSSLOT_DER_ENTITLEMENTS * hash->cs_size;
    const size_t xml_offset = (cd_size + 7) & ~(size_t)7;
    const size_t der_offset = (xml_offset + sizeof(CS_GenericBlob) + capabilities.xml_size + 7) & ~(size_t)7;
    vm_size_t allocation = der_offset + sizeof(CS_GenericBlob) + der_size;
    vm_offset_t memory = 0;
    if (ubc_cs_blob_allocate(&memory, &allocation) != KERN_SUCCESS) {
        error = ENOMEM;
        goto out;
    }
    CS_CodeDirectory *cd = (CS_CodeDirectory *)memory;
    cd->magic = htonl(CSMAGIC_CODEDIRECTORY);
    cd->length = htonl((uint32_t)cd_size);
    cd->version = htonl(CS_SUPPORTSTEAMID);
    cd->identOffset = htonl(sizeof(*cd));
    cd->hashOffset = htonl((uint32_t)cd_size);
    cd->nSpecialSlots = htonl(CSSLOT_DER_ENTITLEMENTS);
    cd->hashSize = hash->cs_size;
    cd->hashType = CS_HASHTYPE_SHA256;
    cd->pageSize = PAGE_SHIFT;
    memcpy((char *)cd + sizeof(*cd), capabilities.identity, strlen(capabilities.identity) + 1);
    if (team_size) {
        cd->teamOffset = htonl(sizeof(*cd) + identity_size);
        memcpy((char *)cd + sizeof(*cd) + identity_size, capabilities.team, team_size);
    }
    CS_GenericBlob *xml_blob = (CS_GenericBlob *)(memory + xml_offset);
    xml_blob->magic = htonl(CSMAGIC_EMBEDDED_ENTITLEMENTS);
    xml_blob->length = htonl((uint32_t)(sizeof(*xml_blob) + capabilities.xml_size));
    memcpy(xml_blob->data, capabilities.xml, capabilities.xml_size);
    CS_GenericBlob *der_blob = (CS_GenericBlob *)(memory + der_offset);
    der_blob->magic = htonl(CSMAGIC_EMBEDDED_DER_ENTITLEMENTS);
    der_blob->length = htonl((uint32_t)(sizeof(*der_blob) + der_size));
    memcpy(der_blob->data, der, der_size);
    union cs_hash_union digest_context;
    uint8_t digest[CS_HASH_MAX_SIZE];
    hash->cs_init(&digest_context);
    hash->cs_update(&digest_context, xml_blob, ntohl(xml_blob->length));
    hash->cs_final(digest, &digest_context);
    memcpy((uint8_t *)cd + cd_size - CSSLOT_ENTITLEMENTS * hash->cs_size, digest, hash->cs_size);
    hash->cs_init(&digest_context);
    hash->cs_update(&digest_context, der_blob, ntohl(der_blob->length));
    hash->cs_final(digest, &digest_context);
    memcpy((uint8_t *)cd + cd_size - CSSLOT_DER_ENTITLEMENTS * hash->cs_size, digest, hash->cs_size);
    struct cs_blob blob = {};
    blob.csb_mem_kaddr = (void *)memory;
    blob.csb_mem_size = allocation;
    blob.csb_cd = cd;
    blob.csb_teamid = team_size ? (const char *)cd + sizeof(*cd) + identity_size : NULL;
    blob.csb_entitlements_blob = xml_blob;
    blob.csb_der_entitlements_blob = der_blob;
    blob.csb_vnode = vp;
    blob.csb_cpu_type = cpu;
    blob.csb_cpu_subtype = subtype;
    blob.csb_base_offset = slice;
    blob.csb_end_offset = coverage;
    blob.csb_hash_pageshift = PAGE_SHIFT;
    blob.csb_hashtype = hash;
    hash->cs_init(&digest_context);
    hash->cs_update(&digest_context, cd, cd_size);
    hash->cs_final(digest, &digest_context);
    memcpy(blob.csb_cdhash, digest, sizeof(blob.csb_cdhash));
    blob.csb_validation_category = CS_VALIDATION_CATEGORY_INVALID;
    blob.csb_flags = SWITCH_APP_CODE_CS_FLAGS;
    blob.csb_reconstituted = SWITCH_APP_CODE_METADATA;
    blob.csb_entitlements = switch_app_code_native_entitlements(der_blob->data, der_size,
        (const char *)cd + sizeof(*cd), &blob);
    if (!blob.csb_entitlements) {
        cs_blob_cleanup(&blob);
        error = EINVAL;
        goto out;
    }
    struct cs_blob *readonly = zalloc_ro(ZONE_ID_CS_BLOB, Z_WAITOK | Z_ZERO);
    if (!readonly) {
        cs_blob_cleanup(&blob);
        error = ENOMEM;
        goto out;
    }
    vnode_lock(vp);
    if (!UBCINFOEXISTS(vp)) {
        vnode_unlock(vp);
        zfree_ro(ZONE_ID_CS_BLOB, readonly);
        cs_blob_cleanup(&blob);
        error = ENOENT;
        goto out;
    }
    struct ubc_info *uip = vp->v_ubcinfo;
    for (existing = uip->cs_blobs; existing; existing = existing->csb_next) {
        if (existing->csb_reconstituted == SWITCH_APP_CODE_METADATA && existing->csb_base_offset == slice) {
            vnode_unlock(vp);
            zfree_ro(ZONE_ID_CS_BLOB, readonly);
            cs_blob_cleanup(&blob);
            error = 0;
            goto out;
        }
    }
    blob.csb_ro_addr = readonly;
    blob.csb_next = uip->cs_blobs;
    zalloc_ro_update_elem(ZONE_ID_CS_BLOB, readonly, &blob);
    ubc_cs_blob_adjust_statistics(&blob);
    os_atomic_thread_fence(seq_cst);
    uip->cs_blobs = readonly;
    vnode_unlock(vp);
    error = 0;
out:
    if (source) kfree_data(source, source_size);
    if (capabilities.xml) kfree_data(capabilities.xml, capabilities.xml_size);
    return error;
}

#endif
