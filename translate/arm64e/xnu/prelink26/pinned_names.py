"""Kernel functions of one kernelcache, named by hand where names.py's other sources cannot or get them wrong.

Each is identified against the same-source function in our 12377 kernel. A wrong name links a kext to the wrong
function, so they apply only to the kernelcache whose decompressed sha256 is KERNELCACHE_SHA256 (ipsw/pins.bzl).
"""

import hashlib
from pathlib import Path

KERNELCACHE_SHA256 = '250337d45598da0e5cc578413704981368afe92f84b3450dc956e21da1b21bd4'

NAMES = {
    # retainCount 1 and IOProviderPropertyMerger's vtable (its C1 twin follows at +0x28); D2 poisons the vtable
    # pointer. 24B119's AppleUSBHostMergeProperties subclasses IOProviderPropertyMerger, 21H461's subclassed IOService,
    # so iOS 17 has no import in that GOT interval to name them.
    0xFFFFFFF00A8EA254: '__ZN24IOProviderPropertyMergerC2EPK11OSMetaClass',
    0xFFFFFFF00A8EA2A4: '__ZN24IOProviderPropertyMergerD2Ev',
    # The same pair for IOMultiMemoryDescriptor (D2 with ~IOMemoryDescriptor inlined), which IOGPUFamily subclasses.
    0xFFFFFFF00A8899F4: '__ZN23IOMultiMemoryDescriptorC2EPK11OSMetaClass',
    0xFFFFFFF00A889A4C: '__ZN23IOMultiMemoryDescriptorD2Ev',
    # IOBufferMemoryDescriptor's factories, told apart by what they pass initWithPhysicalMask: alignment and mask 0
    # with the tags stored at 0x30/0x32; alignment 1 and the caller's mask; initWithGuardPages inlined (internal flag
    # 0x20, round_page, page_size, mask 0). The GOT pass put these names on the wrong functions, the pair above among
    # them.
    0xFFFFFFF00A872760: '__ZN24IOBufferMemoryDescriptor17inTaskWithOptionsEP4taskjmmjj',
    0xFFFFFFF00A872840: '__ZN24IOBufferMemoryDescriptor22inTaskWithPhysicalMaskEP4taskjyy',
    0xFFFFFFF00A872910: '__ZN24IOBufferMemoryDescriptor20inTaskWithGuardPagesEP4taskjy',
    # Five arguments handed to initWithSpecification: the SegmentOptions form.
    0xFFFFFFF00A875FB8: (
        '__ZN12IODMACommand17withSpecificationEPFbPS_NS_9Segment64EPvjEPKNS_14SegmentOptionsEjP8IOMapperS2_'
    ),
    # segments[index] = segment, return true: the segment function kexts pass to IODMACommand as data (__got).
    0xFFFFFFF00A8762CC: '__ZN12IODMACommand12OutputHost64EPS_NS_9Segment64EPvj',
    # serviceMatching(name, table) on the raw pointer, then retains the result and releases the table it consumed.
    0xFFFFFFF00A83EA78: '__ZN9IOService15serviceMatchingEPKc11OSSharedPtrI12OSDictionaryE',
    # kern_codesigning.c in source order, which AppleMobileFileIntegrity and the sandbox import. The GOT pass put these
    # names one function off (developer_mode_state's on disable_code_signing_feature, current_proc's on
    # developer_mode_state) or none: the two panics; secure_channel_shared_page's tail call (0xFFFFFFF00A66C0FC);
    # toggle_developer_mode(true), then (false); a load (developer_mode_state, which the GOT pass names); the research
    # pair (the same code, research_mode_enabled first); rem_enable's call; rem_state's load; device state; boot mode;
    # garbage collection's tail call into its body.
    0xFFFFFFF00A66C038: '_disable_code_signing_feature',
    0xFFFFFFF00A66C100: '_enable_developer_mode',
    0xFFFFFFF00A66C150: '_disable_developer_mode',
    0xFFFFFFF00A66C1B8: '_research_mode_state',
    0xFFFFFFF00A66C1F8: '_extended_research_mode_state',
    0xFFFFFFF00A66C238: '_restricted_execution_mode_enable',
    0xFFFFFFF00A66C278: '_restricted_execution_mode_state',
    0xFFFFFFF00A66C2A0: '_update_csm_device_state',
    0xFFFFFFF00A66C2E8: '_complete_security_boot_mode',
    0xFFFFFFF00A66C34C: '_garbage_collect_provisioning_profiles',
    # After set_local_signing_public_key, as in the source.
    0xFFFFFFF00A66CB40: '_get_local_signing_public_key',
    # No calls, followed by fp_getfvp's call to fp_get_ftype, as in kern_descrip.c.
    0xFFFFFFF00A5F68C0: '_fg_get_vnode',
    # current_persona_get_id (voucher, bank, release), then persona_lookup.
    0xFFFFFFF00A62AD80: '_current_persona_get',
    # panic() after OSCollection's and OSIterator's deleted destructors, between OSMetaClass::instanceConstructed and
    # __stack_chk_fail in the GOT of every kext with an abstract class (thirty of the prelinked ones). Vtables cannot
    # name it: every pure virtual slot points here.
    0xFFFFFFF00A96EE90: '___cxa_pure_virtual',
    # The OSSharedPtr wrapper after setObject(unsigned, OSSharedPtr): the same tail call through the vtable, two slots
    # further (0x100, replaceObject after merge), which IOGameControllerFamily imports.
    0xFFFFFFF00A7A4FC0: '__ZN7OSArray13replaceObjectEjRK11OSSharedPtrIK15OSMetaClassBaseE',
    # lifs. kalloc.c after kalloc_ext and kalloc_large: the data wrapper, its shared twin, kalloc_data_require, then the
    # typed entry points; all of them call kalloc_ext (0xFFFFFFF00A12F8E4).
    0xFFFFFFF00A12FCE4: '_kalloc_data',
    0xFFFFFFF00A12FE9C: '_kalloc_type_impl',
    0xFFFFFFF00A12FF3C: '_kalloc_type_var_impl',
    # "Invalid vm_map_copy_t sz:%lld, ofst:%lld"; the copyin wrapper that makes src_destroy a boolean and tail-calls
    # vm_map_copyin_internal, as ours does; the user-flags check, ktriage and VM_FLAGS_ANYWHERE toggle of vm_allocate's
    # external entry; vm_deallocate's tail call into mach_vm_deallocate.
    0xFFFFFFF00A1F1574: '_vm_map_copy_discard',
    0xFFFFFFF00A1FC608: '_vm_map_copyin',
    0xFFFFFFF00A253B20: '_vm_allocate',
    0xFFFFFFF00A253B84: '_vm_deallocate',
    # iokit_rpc.c: after iokit_make_connect_port, the same code for the ident kobject type.
    0xFFFFFFF00A25B9F0: '_iokit_make_ident_port',
    # Before vfs_attr_pack_internal (the "com.apple.FinderInfo" packer), calling it twice.
    0xFFFFFFF00A2BF04C: '_vfs_attr_pack_ext',
    # vfs_bio.c after buf_meta_breadn: buf_bwrite(ap->a_bp), the function lifs puts in its vnop_bwrite slot.
    0xFFFFFFF00A2CEEC0: '_vn_bwrite',
    # vfs_cluster.c: advisory_read is the 0x10 wrapper calling the 0x3a0 advisory_read_ext; the GOT pass had put
    # advisory_read's name on advisory_read_ext.
    0xFFFFFFF00A2DFB78: '_advisory_read_ext',
    0xFFFFFFF00A2E0190: '_advisory_read',
    # (seq << 32) | (aux << 4) | (stat & 0xf).
    0xFFFFFFF00A2F9B38: '_vfs_resolver_result',
    # The mount helper that names "lifs", with vfs_mount_at_path's calls.
    0xFFFFFFF00A2FD974: '_vfs_mount_at_path',
    # lifs's vnode operation vector: vnop_revoke's nop (a_vp, then vn_revoke), vnop_advlock's and vnop_copyfile's
    # ENOTSUP, vfs_support.c's source order.
    0xFFFFFFF00A314920: '_nop_revoke',
    0xFFFFFFF00A314A60: '_err_advlock',
    0xFFFFFFF00A314C40: '_err_copyfile',
    # mnt_kern_flag (+116) bit 30 read; bit 29 (MNTK_AUTH_OPAQUE) and bit 30 (MNTK_AUTH_OPAQUE_ACCESS) set under the
    # mount lock; the VBLK (v_type 3) check and v_specinfo of vfs_setdevvp; the audit token copy of the context.
    0xFFFFFFF00A320EA4: '_vfs_authopaqueaccess',
    0xFFFFFFF00A320EB0: '_vfs_setauthopaque',
    0xFFFFFFF00A320F54: '_vfs_setauthopaqueaccess',
    0xFFFFFFF00A321930: '_vfs_setdevvp',
    0xFFFFFFF00A32230C: '_vfs_context_copy_audit_token',
    # kpi_vfs.c, which the ubc-like shift had one function off too: MNTK_SYSTEM (0x40) and MNTK_NOSWAP (0x80) set
    # under the mount lock; owner and group stored; snap_mp's mnt_flag (+112) given mp's security bits.
    0xFFFFFFF00A3211E4: '_vfs_setmntsystem',
    0xFFFFFFF00A321478: '_vfs_setnoswap',
    0xFFFFFFF00A3221F8: '_vfs_setowner',
    0xFFFFFFF00A322A58: '_vfs_setsnapshotmntflags',
    # kern_subr.c, adjacent: hashinit, then hashdestroy.
    0xFFFFFFF00A6467BC: '_hashinit',
    0xFFFFFFF00A6468D8: '_hashdestroy',
    # The handler of the kernel's CTLTYPE_INT "L"/"LU" oids (num_vnodes, cs_blob_size, ...): SYSCTL_LONG's.
    0xFFFFFFF00A64E00C: '_sysctl_handle_long',
    # ubc_subr.c, read from the code (v_type VREG, v_ubcinfo at +120, ui_size at +32, ui_flags at +40). The GOT pass
    # had this block shifted (ubc_setsize's name on upl_abort, getsize's on msync, ...): create_upl's vm_tag_bt and
    # ubc_create_upl_kernel calls (the GOT holds its _external alias too); ui_size; UI_ISMAPPED and
    # UI_MAPPEDWRITE out; both bits set; the "overflow detected" msync; range_op's 32-bit range check and tail call;
    # ubc_setsize's tail call into ubc_setsize_ex; upl abort and abort_range.
    0xFFFFFFF00A690F8C: '_ubc_create_upl',
    0xFFFFFFF00A690188: '_ubc_getsize',
    0xFFFFFFF00A69123C: '_ubc_is_mapped',
    0xFFFFFFF00A69129C: '_ubc_is_mapped_writable',
    0xFFFFFFF00A6906E4: '_ubc_msync',
    0xFFFFFFF00A690F2C: '_ubc_range_op',
    0xFFFFFFF00A690164: '_ubc_setsize',
    0xFFFFFFF00A68FCE4: '_ubc_setsize_ex',
    0xFFFFFFF00A6911C8: '_ubc_upl_abort',
    0xFFFFFFF00A690114: '_ubc_upl_abort_range',
    # refcnt.c: the count store; ldadd +1 with the overflow check; ldaddl then ldapr on the last reference (barrier).
    0xFFFFFFF00A7EE0B4: '_os_ref_init_count_external',
    0xFFFFFFF00A7EE0D0: '_os_ref_retain_external',
    0xFFFFFFF00A7EE1FC: '_os_ref_release_barrier_external',
    # The parent list of the vfs.lifs and vfs.apfs sysctl nodes (vfs.generic's too).
    0xFFFFFFF007BDE9A8: '_sysctl__vfs_children',
    # IOLib.cpp's data allocators in source order: IOMallocData, IOMallocZeroData, IOMallocDataShareable,
    # IOMallocZeroDataShareable, IOFreeData, IOFreeDataShareable. The first, second and fifth load one heap
    # (KHEAP_DATA_BUFFERS), these three the other (KHEAP_DATA_SHARED). The GOT pass had the private heap's names on the
    # shareable allocators: our IOMallocData and IOMallocData_external are one function, so both names fit (r34-net).
    0xFFFFFFF00A80DA8C: '_IOMallocDataShareable',
    0xFFFFFFF00A80DAD8: '_IOMallocZeroDataShareable',
    0xFFFFFFF00A80DB70: '_IOFreeDataShareable',
    # IOSerialFamily. subr_xxx.c's stubs in source order: enodev's ENODEV, enodev_strat's bare return, then enxio, enoioctl,
    # enosys, eopnotsupp, nullop, and nulldev's 0 last; the cdevsw IOSerialBSDClient registers points at them.
    0xFFFFFFF00A676794: '_enodev',
    0xFFFFFFF00A67679C: '_enodev_strat',
    0xFFFFFFF00A6767C8: '_nulldev',
    # tty.c, in IOSerialFamily's GOT between tty_unlock and wakeup in this order: ttyclose (ttyflush, then ttyfree inlined:
    # three clfree, lck_mtx_destroy, kfree), ttyfree (the same deallocation), ttymalloc (kalloc_type and the clallocs)
    # and ttyselect (selrecord).
    0xFFFFFFF00A685940: '_ttyclose',
    0xFFFFFFF00A685870: '_ttyfree',
    0xFFFFFFF00A688E50: '_ttymalloc',
    0xFFFFFFF00A687AD0: '_ttyselect',
    # IOGameControllerFamily. vm_kern.c: after vm_kernel_addrperm_external, the same code with the unslide.
    0xFFFFFFF00A1EE914: '_vm_kernel_unslide_or_perm_external',
    # The callback-pointer iterateObjects, before the block one (0xFFFFFFF00A7A64A4).
    0xFFFFFFF00A7A63B0: '__ZN12OSCollection14iterateObjectsEPvPFbS0_P8OSObjectE',
    # OSNumber.cpp after the two withNumber factories: d8 stored as is (withFloat's fcvt follows); doubleValue's ucvtf.
    0xFFFFFFF00A7D66C4: '__ZN8OSNumber10withDoubleEd',
    0xFFFFFFF00A7D67B8: '__ZNK8OSNumber11doubleValueEv',
    # OSSerializeBinary.cpp: binaryWithCapacity, then addBinary, then setIndexed (0xFFFFFFF00A7E43B0).
    0xFFFFFFF00A7E40D4: '__ZN11OSSerialize18binaryWithCapacityEjPvS0_',
    # IODataQueue's structors (its metaclass "IODataQueue" just before): retainCount 1 and the vtable; D2 poisons it.
    0xFFFFFFF00A8C01E4: '__ZN11IODataQueueC2EPK11OSMetaClass',
    0xFFFFFFF00A8C0234: '__ZN11IODataQueueD2Ev',
}


def names_for(kernelcache: str) -> dict[int, str]:
    with Path(kernelcache).open('rb') as data:
        digest = hashlib.file_digest(data, 'sha256').hexdigest()
    return NAMES if digest == KERNELCACHE_SHA256 else {}
