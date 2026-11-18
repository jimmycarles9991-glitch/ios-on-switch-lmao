// SPDX-License-Identifier: GPL-2.0-only
#include <sys/errno.h>
#include <sys/codesign.h>
#include <sys/proc_internal.h>
#include <sys/ubc_internal.h>
#include <libkern/libkern.h>
#include <switch/bootstrap.h>
#include <switch/entitlements_factory.h>
#include <kern/cs_blobs.h>
#include <mach/vm_prot.h>

int switch_bootstrap_validate(struct vnode *vp)
{
    for (struct cs_blob *blob = ubc_get_cs_blobs(vp); blob; blob = blob->csb_next)
        if (blob->csb_reconstituted == SWITCH_BOOTSTRAP_SIGNATURE && !switch_bootstrap_image(vp)) return EPERM;
    return 0;
}

bool switch_bootstrap_signed(struct vnode *vp)
{
    if (!vp) return false;
    for (struct cs_blob *blob = ubc_get_cs_blobs(vp); blob; blob = blob->csb_next)
        if (blob->csb_reconstituted == SWITCH_BOOTSTRAP_SIGNATURE) return true;
    return false;
}

bool switch_bootstrap_sandbox_target(struct vnode *vp, int64_t offset)
{
    if (!vp || !switch_bootstrap_image(vp)) return false;
    struct cs_blob *blob = csvnode_get_blob(vp, offset);
    const unsigned int required = CS_VALID | CS_SIGNED | CS_ENTITLEMENTS_VALIDATED;
    return blob && blob->csb_reconstituted == SWITCH_BOOTSTRAP_SIGNATURE &&
        (csblob_get_flags(blob) & required) == required && csblob_get_platform_binary(blob);
}

bool switch_bootstrap_exec_mapping(struct proc *p, struct vnode *vp, uint64_t offset, int prot)
{
    const unsigned int required = CS_VALID | CS_SIGNED;
    return p && (prot & (VM_PROT_WRITE | VM_PROT_EXECUTE)) == VM_PROT_EXECUTE && offset <= INT64_MAX &&
        (proc_getcsflags(p) & required) == required &&
        switch_bootstrap_sandbox_target(p->p_textvp, p->p_textoff) &&
        switch_bootstrap_sandbox_target(vp, (int64_t)offset);
}

int switch_bootstrap_signature(struct vnode *vp, struct cs_blob *blob, unsigned int *flags, bool *admitted)
{
    *admitted = false;
    if (!switch_bootstrap_image(vp)) return blob->csb_reconstituted == SWITCH_BOOTSTRAP_SIGNATURE ? EPERM : 0;
    if (!blob->csb_cd || !blob->csb_hashtype || !ntohl(blob->csb_cd->nCodeSlots)) return EBADEXEC;
    void *xml = NULL;
    size_t xml_size = 0, der_size = 0;
    const CS_GenericBlob *der = NULL;
    int error = csblob_get_entitlements(blob, &xml, &xml_size);
    if (!error) error = csblob_get_der_entitlements(blob, &der, &der_size);
    if (error || (xml_size && !der)) return error ? error : EBADEXEC;
    if (der && !blob->csb_entitlements) {
        if (der_size <= sizeof(*der)) return EBADEXEC;
        blob->csb_entitlements = switch_app_code_native_entitlements(der->data, der_size - sizeof(*der),
            csblob_get_identity(blob), blob);
        if (!blob->csb_entitlements) return EBADEXEC;
    }
    *flags |= CS_VALID | CS_SIGNED | CS_ENTITLEMENTS_VALIDATED | CS_PLATFORM_BINARY;
    blob->csb_flags = *flags;
    blob->csb_validation_category = CS_VALIDATION_CATEGORY_PLATFORM;
    *admitted = true;
    return 0;
}
