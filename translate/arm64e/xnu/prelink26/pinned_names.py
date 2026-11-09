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
    # Five arguments handed to initWithSpecification: the SegmentOptions form.
    0xFFFFFFF00A875FB8: (
        '__ZN12IODMACommand17withSpecificationEPFbPS_NS_9Segment64EPvjEPKNS_14SegmentOptionsEjP8IOMapperS2_'
    ),
    # serviceMatching(name, table) on the raw pointer, then retains the result and releases the table it consumed.
    0xFFFFFFF00A83EA78: '__ZN9IOService15serviceMatchingEPKc11OSSharedPtrI12OSDictionaryE',
    # kern_codesigning.c in source order, which AppleMobileFileIntegrity and the sandbox import. The GOT pass put these
    # names one function off (developer_mode_state's on disable_code_signing_feature, current_proc's on
    # developer_mode_state) or none: the two panics; secure_channel_shared_page's tail call (0xFFFFFFF00A66C0FC);
    # toggle_developer_mode(true), then (false); a load (developer_mode_state, which the GOT pass names); the research
    # pair (the same code, research_mode_enabled first); rem_enable's call; rem_state's load; device state; boot mode;
    # garbage collection's tail call into its body.
    0xFFFFFFF00A66C100: '_enable_developer_mode',
    0xFFFFFFF00A66C150: '_disable_developer_mode',
    0xFFFFFFF00A66C1B8: '_research_mode_state',
    0xFFFFFFF00A66C1F8: '_extended_research_mode_state',
    0xFFFFFFF00A66C238: '_restricted_execution_mode_enable',
    0xFFFFFFF00A66C278: '_restricted_execution_mode_state',
    0xFFFFFFF00A66C2A0: '_update_csm_device_state',
    0xFFFFFFF00A66C2E8: '_complete_security_boot_mode',
    # After set_local_signing_public_key, as in the source.
    0xFFFFFFF00A66CB40: '_get_local_signing_public_key',
    # No calls, followed by fp_getfvp's call to fp_get_ftype, as in kern_descrip.c.
    0xFFFFFFF00A5F68C0: '_fg_get_vnode',
    # current_persona_get_id (voucher, bank, release), then persona_lookup.
    0xFFFFFFF00A62AD80: '_current_persona_get',
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
    0xFFFFFFF00A1FC608: '_vm_map_copyin',
    0xFFFFFFF00A253B20: '_vm_allocate',
    0xFFFFFFF00A253B84: '_vm_deallocate',
    # iokit_rpc.c: after iokit_make_connect_port, the same code for the ident kobject type.
    0xFFFFFFF00A25B9F0: '_iokit_make_ident_port',
    # Before vfs_attr_pack_internal (the "com.apple.FinderInfo" packer), calling it twice.
    0xFFFFFFF00A2BF04C: '_vfs_attr_pack_ext',
    # vfs_cluster.c: advisory_read is the 0x10 wrapper calling the 0x3a0 advisory_read_ext; the GOT pass had put
    # advisory_read's name on advisory_read_ext.
    0xFFFFFFF00A2DFB78: '_advisory_read_ext',
    # (seq << 32) | (aux << 4) | (stat & 0xf).
    0xFFFFFFF00A2F9B38: '_vfs_resolver_result',
    # The mount helper that names "lifs", with vfs_mount_at_path's calls.
    0xFFFFFFF00A2FD974: '_vfs_mount_at_path',
    # mnt_kern_flag (+116) bit 30 read; bit 29 (MNTK_AUTH_OPAQUE) and bit 30 (MNTK_AUTH_OPAQUE_ACCESS) set under the
    # mount lock; the VBLK (v_type 3) check and v_specinfo of vfs_setdevvp; the audit token copy of the context.
    0xFFFFFFF00A320EA4: '_vfs_authopaqueaccess',
    0xFFFFFFF00A320EB0: '_vfs_setauthopaque',
    0xFFFFFFF00A320F54: '_vfs_setauthopaqueaccess',
    0xFFFFFFF00A321930: '_vfs_setdevvp',
    0xFFFFFFF00A32230C: '_vfs_context_copy_audit_token',
    # kpi_vfs.c, which the ubc-like shift had one function off too: MNTK_SYSTEM (0x40) and MNTK_NOSWAP (0x80) set
    # under the mount lock; owner and group stored; snap_mp's mnt_flag (+112) given mp's security bits.
    0xFFFFFFF00A322A58: '_vfs_setsnapshotmntflags',
    # kern_subr.c, adjacent: hashinit, then hashdestroy.
    0xFFFFFFF00A6468D8: '_hashdestroy',
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
    0xFFFFFFF00A6911C8: '_ubc_upl_abort',
    0xFFFFFFF00A690114: '_ubc_upl_abort_range',
    # refcnt.c: the count store; ldadd +1 with the overflow check; ldaddl then ldapr on the last reference (barrier).
    0xFFFFFFF00A7EE0B4: '_os_ref_init_count_external',
    0xFFFFFFF00A7EE0D0: '_os_ref_retain_external',
    0xFFFFFFF00A7EE1FC: '_os_ref_release_barrier_external',
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
    # corecrypto. subrs.c's fortify checks after strnstr, each panicking with its own name ("__memcpy_chk object size
    # check failed", ...) before tail-calling memmove or memset.
    0xFFFFFFF00A264588: '___memcpy_chk',
    # The int behind the desired_vnodes oid, next to rootvp, between ___stack_chk_guard and _gIODTPlane in apfs's GOT.
    0xFFFFFFF00AC58DA4: '_desiredvnodes',
    # The isa of the kernel's global block literals (invoke, then descriptor after it), which kexts' blocks use too.
    0xFFFFFFF00AC5CA78: '__NSConcreteGlobalBlock',
    # Each 16 bytes before its class's vtable, after the class's gMetaClass in the kexts' GOTs: Class::metaClass.
    0xFFFFFFF007C19348: '__ZN13IOCommandPool9metaClassE',
    # kern_codesigning.c after the image4 dispatcher ("image4_ dispatch: overflow on input"): the wrappers that only
    # branch to xnu_image4_get_monitor_exports and xnu_image4_set_release_type.
    0xFFFFFFF00A66D54C: '_kernel_image4_get_monitor_exports',
    0xFFFFFFF00A66D568: '_kernel_image4_set_release_type',
    # IOKitBSDInit.cpp after IOBSDGetPlatformUUID: IOBSDMountChange and IOBSDLowSpaceUnlinkKernelCore (empty without
    # IOPOLLED_COREFILE: a lone RET each), then the string-entitlement checks (get_bsdtask_info).
    0xFFFFFFF00A8ECC10: '_IOBSDMountChange',
    0xFFFFFFF00A8ECC14: '_IOBSDLowSpaceUnlinkKernelCore',
    0xFFFFFFF00A8ECC94: '_IOTaskHasStringEntitlement',
    # IODMACommand.cpp's OutputBig32 (rev), which no kext's iOS 17 GOT names in its gap.
    0xFFFFFFF00A873134: '__ZN12IODMACommand11OutputBig32EPS_NS_9Segment64EPvj',
    # apfs. vfs_support.c's nop_*/err_* in source order between named ones (nop returns 0, err ENOTSUP: the
    # same words as ours).
    0xFFFFFFF00A3149F0: '_err_readdirattr',
    0xFFFFFFF00A314A68: '_nop_allocate',
    0xFFFFFFF00A314A98: '_nop_pagein',
    0xFFFFFFF00A314B58: '_nop_pageout',
    0xFFFFFFF00A314C28: '_err_searchfs',
    # apfs. vfs_cprotect.c in source order between cpx_key and doc_tombstone_get, the same count as ours (the
    # accessors' words match).
    0xFFFFFFF00A32D0C8: '_cpx_set_is_sep_wrapped_key',
    0xFFFFFFF00A32D0DC: '_cpx_is_composite_key',
    0xFFFFFFF00A32D114: '_cpx_set_use_offset_for_iv',
    0xFFFFFFF00A32D140: '_cpx_set_synthetic_offset_for_iv',
    0xFFFFFFF00A32D160: '_cpx_max_key_len',
    0xFFFFFFF00A32D170: '_cpx_set_key_len',
    0xFFFFFFF00A32D1A4: '_cpx_set_aes_iv_key',
    0xFFFFFFF00A32D338: '_cpx_copy',
    0xFFFFFFF00A32D6F8: '_cp_is_valid_class',
    0xFFFFFFF00A32D71C: '_cp_os_version',
    # Paired in order inside runs of named neighbours that hold as many functions in 24B119 as in ours (unload_trust_cache's
    # entitlement string, vnode_addfsref's panic, the proc_ accessors' loads).
    0xFFFFFFF00A317B5C: '_utf8_normalizeOptCaseFoldToUTF8',
    0xFFFFFFF00A5EF578: '_current_cached_proc_cred',
    0xFFFFFFF00A612960: '_ctl_name_by_id',
    0xFFFFFFF00A629FD4: '_proc_persona_get',
    0xFFFFFFF00A62A400: '_persona_lookup',
    0xFFFFFFF00A62E624: '_proc_original_ppid',
    0xFFFFFFF00A62E7A8: '_proc_orig_ppidversion',
    0xFFFFFFF00A62EB70: '_proc_is_simulated',
    0xFFFFFFF00A62F26C: '_proc_signal',
    0xFFFFFFF00A635AB4: '_proc_pgrpid',
    # The sandbox's (AMFI shares some), each placed by what it does and checked against the alphabetical order of every
    # GOT holding it: kern_cs.c's csblob/csproc accessors (cs_blob grew 0x30 after csb_hashtype, so platform_binary reads
    # 0xdc), kern_proc.c's syscall-filter and sandbox-info setters (nsysent 558, mach_trap_count 128, mach_kobj_count),
    # proc_is_translated (P_TRANSLATED), proc_is_rsr, proc_ucred (p_proc_ro checked in the RO zone), file_socket
    # (DTYPE_SOCKET, ENOTSOCK) and file_drop, vnode_authorize (kauth_authorize_action), psignal's SIGKILL try-thread form,
    # PE_parse_boot_arg_str (force-string parse of the boot args),
    # ptrauth_utils_sign_blob_generic (what ptrauth_utils_auth_blob_generic compares against).
    0xFFFFFFF00A1F7998: '_vm_map_page_mask',
    0xFFFFFFF00A27CB20: '_read_frandom',
    0xFFFFFFF00A2F0524: '_vn_revoke',
    0xFFFFFFF00A2F177C: '_vn_getpath_ext_with_mntlen',
    0xFFFFFFF00A2F1860: '_vn_getpath_ext',
    0xFFFFFFF00A2F7554: '_vnode_authorize',
    0xFFFFFFF00A5F100C: '_kauth_proc_label_update',
    0xFFFFFFF00A5F1A74: '_csblob_get_flags',
    0xFFFFFFF00A5F1A94: '_csproc_get_blob',
    0xFFFFFFF00A5F1B5C: '_csvnode_get_blob',
    0xFFFFFFF00A5F1B74: '_csblob_get_identity',
    0xFFFFFFF00A5F1BD4: '_csblob_get_cdhash',
    0xFFFFFFF00A5F21E4: '_csproc_get_platform_binary',
    0xFFFFFFF00A5F2EA4: '_cs_restricted',
    0xFFFFFFF00A5F70D4: '_file_socket',
    0xFFFFFFF00A5F75C0: '_file_drop',
    0xFFFFFFF00A62D3CC: '_proc_find_ident_validated',
    0xFFFFFFF00A62F5D0: '_proc_ucred',
    0xFFFFFFF00A630104: '_proc_getexecutablevnode_noblock',
    0xFFFFFFF00A632FB8: '_proc_is_translated',
    0xFFFFFFF00A6331DC: '_proc_is_rsr',
    0xFFFFFFF00A636034: '_proc_get_syscall_filter_mask_size',
    0xFFFFFFF00A636064: '_proc_get_syscall_filter_mask',
    0xFFFFFFF00A636164: '_proc_set_syscall_filter_mask',
    0xFFFFFFF00A636294: '_proc_set_syscall_filter_callbacks',
    0xFFFFFFF00A6362F4: '_proc_set_syscall_filter_index',
    0xFFFFFFF00A636390: '_proc_set_sandbox_info_callbacks',
    0xFFFFFFF00A6363D4: '_proc_set_filter_message_flag',
    0xFFFFFFF00A643868: '_psignal_sigkill_try_thread_with_reason',
    0xFFFFFFF00A64E408: '_sysctl_handle_string',
    0xFFFFFFF00A7DFE48: '__ZN8OSSymbol24existingSymbolForCStringEPKc',
    0xFFFFFFF00A922530: '_PE_parse_boot_arg_str',
    0xFFFFFFF00A0DD2F8: '_ptrauth_utils_sign_blob_generic',
    # AMFI's: kern_cs.c's csblob accessors in source order (cs_blob grew 0x30 after csb_hashtype: base_offset 0x28, size
    # 0x40, addr 0x50 as ours; teamid 0xb8, validation_category 0xe0, code_directory 0xb0 = ours + 0x30), the csproc/csfg
    # getters and cs_require_lv/cs_system_require_lv (release constants in 24B119; ours keep their policy globals),
    # ubc_subr.c's blob parsers ("CS Blob length"), the trust-cache queries' _with_length forms (query type or cdhash
    # first), csblob_register_profile (a cs_profile_register_t: a no-op without a code-signing monitor, as ours),
    # vm_map_copyout (NULL copy), vm_map_wire (vm_tag_bt) and unwire, the printable vnode names, task_set_cs_auxiliary_info,
    # launch_constraint_data_get_launch_type, ptrauth_utils_auth_blob_generic and pmap's CoreEntitlements acceleration
    # buffers. Every one sorts between its named neighbours in the GOTs that hold it.
    0xFFFFFFF00A171B78: '_task_set_cs_auxiliary_info',
    0xFFFFFFF00A1FAF4C: '_vm_map_unwire',
    0xFFFFFFF00A1FAF58: '_vm_map_wire',
    0xFFFFFFF00A1FC5EC: '_vm_map_copyout',
    0xFFFFFFF00A2D1FFC: '_vnode_putname_printable',
    0xFFFFFFF00A5F17E0: '_cs_require_lv',
    0xFFFFFFF00A5F17E8: '_cs_system_require_lv',
    0xFFFFFFF00A5F17F0: '_csblob_get_base_offset',
    0xFFFFFFF00A5F17F8: '_csblob_get_size',
    0xFFFFFFF00A5F1800: '_csblob_get_addr',
    0xFFFFFFF00A5F1808: '_csblob_get_platform_binary',
    0xFFFFFFF00A5F18D0: '_csvnode_invalidate_flags',
    0xFFFFFFF00A5F1A7C: '_csblob_get_hashtype',
    0xFFFFFFF00A5F1B6C: '_csblob_get_teamid',
    0xFFFFFFF00A5F1BDC: '_csblob_get_cdhash_with_length',
    0xFFFFFFF00A5F1BF0: '_csblob_get_signer_type',
    0xFFFFFFF00A5F1BF8: '_csblob_set_validation_category',
    0xFFFFFFF00A5F1C8C: '_csblob_get_validation_category',
    0xFFFFFFF00A5F1C94: '_csblob_set_auxiliary_info',
    0xFFFFFFF00A5F1CE8: '_csblob_get_auxiliary_info',
    0xFFFFFFF00A5F1CF0: '_csblob_get_trust_level',
    0xFFFFFFF00A5F1D2C: '_csblob_get_code_directory',
    0xFFFFFFF00A5F1DBC: '_csblob_os_entitlements_set',
    0xFFFFFFF00A5F1E28: '_csblob_os_entitlements_copy',
    0xFFFFFFF00A5F1EC8: '_csblob_os_entitlements_get',
    0xFFFFFFF00A5F1F30: '_csproc_get_teamid',
    0xFFFFFFF00A5F28EC: '_csfg_get_teamid',
    0xFFFFFFF00A5F2A4C: '_csfg_get_csblob',
    0xFFFFFFF00A5F2DF8: '_csproc_get_platform_identifier',
    0xFFFFFFF00A5F32E0: '_launch_constraint_data_get_launch_type',
    0xFFFFFFF00A66B764: '_query_trust_cache_with_length',
    0xFFFFFFF00A66B8D0: '_query_trust_cache_for_rem_with_length',
    0xFFFFFFF00A66CC7C: '_csm_resolve_os_entitlements_from_proc',
    0xFFFFFFF00A68F180: '_csblob_get_entitlements',
    0xFFFFFFF00A68F344: '_csblob_get_der_entitlements',
    0xFFFFFFF00A68F4EC: '_csblob_register_profile',
    # One function of ours each under two names, so the GOT count cannot place them. kalloc.c's frees in lifs's GOT
    # order between kernel_thread_start and lck_grp_alloc_init, four signatures: kfree_data (KHEAP_DATA, ptr and size
    # into kfree_ext), kfree_data_addr (KHEAP_DATA and ptr into kfree_addr_ext), kfree_type_impl (the zone view check,
    # then kfree_ext) and kfree_type_var_impl (the view with bit 0 set into kfree_ext). Our kfree_type_impl handed a
    # var view reads its version and sizes as a zone and panics in zone_invalid_panic. IOGPUFamily's entry between
    # mach_vm_region and memmove is mach_vm_wire_level_monitor, not memcpy: KERN_INVALID_ARGUMENT for no pages,
    # KERN_RESOURCE_SHORTAGE past the user wire limit's pages less the wired count, as ours; memmove is the 432-byte
    # body. IOLib.cpp's IOLockSleep and IOLockSleepDeadline, each only a branch to lck_mtx_sleep /
    # lck_mtx_sleep_deadline.
    0xFFFFFFF00A13041C: '_kfree_data',
    0xFFFFFFF00A130430: '_kfree_data_addr',
    0xFFFFFFF00A1303D4: '_kfree_type_impl',
    0xFFFFFFF00A130414: '_kfree_type_var_impl',
    0xFFFFFFF00A227074: '_mach_vm_wire_level_monitor',
    0xFFFFFFF00A972EB0: '_memmove',
    0xFFFFFFF00A80E4D0: '_IOLockSleep',
    0xFFFFFFF00A80E4E0: '_IOLockSleepDeadline',
    # Alone in their GOT gaps among our names, with the body: lock_rw_old.c's "Invalid lock sleep action" and
    # assert_wait_deadline; the tail call into vm_map_region; the arm no-op; port_name_to_task_grp(name, flavor 0,
    # options 0, group 3), which is port_name_to_task_external's TASK_FLAVOR_CONTROL and TASK_GRP_EXTERNAL (Apple's
    # inlined port_name_to_task_name passes flavor 3, options 12, group 0: the same argument order as ours);
    # wakeup_with_inheritor_and_turnstile with no woken-thread out pointer (all, not one);
    # the "Expected to have usecount or iocount" check of vnode_getwithref_noblock; the PAC-signed v_parent load.
    0xFFFFFFF00A138158: '_lck_rw_sleep_deadline',
    0xFFFFFFF00A25442C: '_mach_vm_region',
    0xFFFFFFF00A29C650: '_ml_thread_policy',
    0xFFFFFFF00A12C90C: '_port_name_to_task',
    0xFFFFFFF00A13D114: '_wakeup_all_with_inheritor',
    0xFFFFFFF00A2F3A78: '_vnode_getwithref_noblock',
    0xFFFFFFF00A323B44: '_vnode_parent',
    # apfs's SMR imports between read_random and snprintf, in GOT order: smr_call (disables and re-enables
    # preemption around the queue), smr_enter, smr_hash_destroy (frees through kfree_ext), smr_hash_init,
    # smr_hash_init_empty, smr_hash_is_empty_initialized (the same instructions as ours), smr_leave (re-enables
    # preemption).
    0xFFFFFFF00A164064: '_smr_call',
    0xFFFFFFF00A163E18: '_smr_enter',
    0xFFFFFFF00A16482C: '_smr_hash_destroy',
    0xFFFFFFF00A164784: '_smr_hash_init',
    0xFFFFFFF00A164744: '_smr_hash_init_empty',
    0xFFFFFFF00A164760: '_smr_hash_is_empty_initialized',
    0xFFFFFFF00A163E44: '_smr_leave',
    # apfs between file_drop and kauth_cred_get, in GOT order and as 21H461's apfs imported them: the withvid
    # wrapper (fourth argument 0), vfs_bio.c's gc register/unregister pair (one global lock), the EXC_GUARD of the
    # current thread, kern_shutdown.c's two flags (OSKextIsInUserspaceReboot inlined as a byte load).
    0xFFFFFFF00A5F6D44: '_file_vnode_withvid',
    0xFFFFFFF00A2CF700: '_fs_buffer_cache_gc_register',
    0xFFFFFFF00A2CF8AC: '_fs_buffer_cache_gc_unregister',
    0xFFFFFFF00A600EA4: '_generate_file_permissions_guard_exception',
    0xFFFFFFF00A63E7B0: '_get_system_inshutdown',
    0xFFFFFFF00A63E7BC: '_get_system_inuserspacereboot',
    # kpi_vfs.c's context queries in apfs's GOT order between vfs_context_current and vfs_context_kernel: the
    # branch into the original dataless check, p_vfs_iopolicy bit 0x20 (IGNORE_CONTENT_PROTECTION), is64bit, the
    # dataless-manipulation entitlement strings, proc_pendingsignals with the mask; then the original dataless check
    # itself, alone between vfs_context_kernel and vfs_context_proc.
    0xFFFFFFF00A3109A0: '_vfs_context_dataless_materialization_is_prevented',
    0xFFFFFFF00A322570: '_vfs_context_ignores_content_protection',
    0xFFFFFFF00A32079C: '_vfs_context_is64bit',
    0xFFFFFFF00A310A84: '_vfs_context_is_dataless_manipulator',
    0xFFFFFFF00A3223B8: '_vfs_context_issignal',
    0xFFFFFFF00A3109A4: '_vfs_context_orig_dataless_materialization_is_prevented',
    # apfs's pairs, each gap holding one name its 21H461 apfs imported and one Darwin 26 KPI of ours: in GOT order.
    0xFFFFFFF00A2DFA44: '_cluster_unlock_direct_read',
    0xFFFFFFF00A2DC544: '_cluster_update_range',
    0xFFFFFFF00A32CEA8: '_cpx_alloc',
    0xFFFFFFF00A32CF34: '_cpx_alloc_ctx',
    0xFFFFFFF00A2F8594: '_vnode_attr_authorize_init',
    0xFFFFFFF00A2F8620: '_vnode_authattr',
    # libkern's os_pcpu_ref: init and kill (the two left between destroy and release), retain_try (the one left
    # between release and wait_for_death).
    0xFFFFFFF00A7EE2C4: '_os_pcpu_ref_init',
    0xFFFFFFF00A7EE468: '_os_pcpu_ref_kill',
    0xFFFFFFF00A7EE590: '_os_pcpu_ref_retain_try',
    # Alone or ordered in their GOT gaps, with the body: the null check of ubc_was_mapped_writable (ubc_was_mapped
    # has none); the shared-to-exclusive upgrade of cache_lookup_ext; each thread_call_enter*_delayed's own name in
    # its panic; IOSimpleLockInit's inlined lck_spin_init (Free only branches, GetMachLock returns its argument);
    # setMapperOptions's store; IORegistryIterator::iterateOver from the registry root, the other overload's caller.
    0xFFFFFFF00A6912F4: '_ubc_was_mapped_writable',
    0xFFFFFFF00A2D3404: '_cache_lookup_ext',
    0xFFFFFFF00A189AC4: '_thread_call_enter_delayed',
    0xFFFFFFF00A189B28: '_thread_call_enter1_delayed',
    0xFFFFFFF00A80E8E0: '_IOSimpleLockInit',
    0xFFFFFFF00A888980: '__ZN18IOMemoryDescriptor16setMapperOptionsEt',
    0xFFFFFFF00A81E850: '__ZN18IORegistryIterator11iterateOverEPK15IORegistryPlanej',
    # IOCircularDataQueue.cpp in IOHIDFamily's GOT order: CopyLatest, CopyMemoryDescriptor before
    # CreateWithEntries; Destroy, Enqueue (the 420-byte writer) after it.
    0xFFFFFFF00A8C1E10: '_IOCircularDataQueueCopyLatest',
    0xFFFFFFF00A8C19B0: '_IOCircularDataQueueCopyMemoryDescriptor',
    0xFFFFFFF00A8C18FC: '_IOCircularDataQueueDestroy',
    0xFFFFFFF00A8C1A28: '_IOCircularDataQueueEnqueue',
    # Data, by the functions that use it: eight named functions each (spec_ioctl, bdevsw_add, ...; decmpfs_init,
    # decmpfs_read_compressed, ...) reference it where ours reference bdevsw / decmpfs_ctx, in apfs's GOT order.
    # IOSerialFamily's data GOT after tk_nin: the tty counter tk_rawcc, then the line discipline's input routine
    # (3.5 KiB, taken as a pointer for linesw).
    0xFFFFFFF00ABE0160: '_bdevsw',
    0xFFFFFFF00AC407D8: '_decmpfs_ctx',
    0xFFFFFFF00AC57D18: '_tk_rawcc',
    0xFFFFFFF00A682FFC: '_ttyinput',
    # apfs, read from the code: vn_pathconf (21H461's apfs import there); build_path_with_parent with flags 0 and
    # the current context, EINVAL for a negative length; the mount flags cleared but MNT_VISFLAGMASK's two bits;
    # UT_FS_ENTITLED_RESERVE_ACCESS (uthread 0x20000) or P_VFS_IOPOLICY_ENTITLED_RESERVE_ACCESS (0x2000).
    0xFFFFFFF00A31BE2C: '_vn_pathconf',
    0xFFFFFFF00A2F19C4: '_vn_getpath_fsenter_with_parent',
    0xFFFFFFF00A320BEC: '_vfs_clearflags',
    0xFFFFFFF00A32251C: '_vfs_context_allow_entitled_reserve_access',
    # MAC framework entries in apfs's GOT between lck_spin_unlock and mach_absolute_time, by the mac_policy_ops
    # slot each calls: 0x440 is mpo_mount_check_snapshot_mount (six arguments), 0x8a8 mpo_vnode_check_setflags.
    0xFFFFFFF00A939C98: '_mac_mount_check_snapshot_mount',
    0xFFFFFFF00A937794: '_mac_vnode_check_setflags',
    # task.c's exported references, one function of ours under two names each, in the GOT order of the IOKit
    # families that imported them in 21H461 (between sysctlbyname and thread_block): task_deallocate (os_ref release,
    # then task_deallocate_internal), task_id_token_port_name_to_task (flavor 3 into the port lookup),
    # task_is_app_suspended (one bit, as ours), task_reference (zone_id_require, then os_ref retain).
    0xFFFFFFF00A17E60C: '_task_deallocate',
    0xFFFFFFF00A1AD270: '_task_id_token_port_name_to_task',
    0xFFFFFFF00A1AA9A8: '_task_is_app_suspended',
    0xFFFFFFF00A17E580: '_task_reference',
    # IOMemoryDescriptor.cpp after setVMTags, in the GOT order of IOSurface and IOGPUFamily, whose 21H461 builds
    # imported these names there: setContext with the default key (a branch into the keyed form),
    # IOMemoryDescriptor::setOwnership (after IOGeneralMemoryDescriptor's), the three-argument getPageCounts,
    # getDMAMapLength, getDescriptorID (setDescriptorID, then the reserved field).
    0xFFFFFFF00A887B2C: '__ZN18IOMemoryDescriptor10setContextEP8OSObject',
    0xFFFFFFF00A887BB8: '__ZN18IOMemoryDescriptor12setOwnershipEP4taskij',
    0xFFFFFFF00A888034: '__ZN18IOMemoryDescriptor13getPageCountsEPyS0_S0_',
    0xFFFFFFF00A887E18: '__ZN18IOMemoryDescriptor15getDMAMapLengthEPy',
    0xFFFFFFF00A887B3C: '__ZN18IOMemoryDescriptor15getDescriptorIDEv',
    # IOGPUFamily's IOPerfControlClient::querySubmitterRole, read from the code: kIOReturnNotFound unless the client's
    # device type (+0xc) is GPU, else the task's GPU role byte into role_out. copyClient is the 8-byte tail call into
    # copyClientForDeviceType that the other kexts import. The serial driver's IORecursiveLockSleepDeadline, the only
    # 21H461 import of its kext in its GOT gap.
    0xFFFFFFF00A8F3050: '__ZN19IOPerfControlClient18querySubmitterRoleEP9IOServiceP4taskPj',
    0xFFFFFFF00A80E7D8: '_IORecursiveLockSleepDeadline',
    # apfs between lck_mtx_lock and lck_mtx_unlock (IOLockLock and IOLockUnlock's own names): six arguments, the
    # sleep action's flags tested first, the deadline in x5.
    0xFFFFFFF00A13C1F0: '_lck_mtx_sleep_with_inheritor',
    # Read from the code, each the one name of ours in its GOT gap that does it: OSSerializer::withBlock (_Block_copy
    # and OSSerializer's metaclass); IOUserClient::removeMappingForDescriptor (the mappings set walked with an
    # OSCollectionIterator, each IOMemoryMap's descriptor compared); IOWorkLoop::setMaximumLockTime (under the gate,
    # the interval stored and option bit 1 into the flags, as ours).
    0xFFFFFFF00A7DA930: '__ZN12OSSerializer9withBlockEU13block_pointerFbP11OSSerializeE',
    0xFFFFFFF00A8BB858: '__ZN12IOUserClient26removeMappingForDescriptorEP18IOMemoryDescriptor',
    0xFFFFFFF00A868ACC: '__ZN10IOWorkLoop18setMaximumLockTimeEyj',
    # Identified for 24A446 in UniXNU's XnuPrelink fixtures (fileset-24A446-symbols.json) and found here at the
    # same place: task_is_privileged right after init_task_ledgers (a cached bool byte in 13432); isLPWMode after
    # isAOTMode, reading the same +0x35c byte, before IOPMIsLPWMode; the {2, 2} SMR_SEQ_INIT clock apfs reaches as
    # _apfs_smr, Private.exports' name for _vfs_smr (tsleep, mount_common and vflush use it here).
    0xFFFFFFF00A16FB0C: '_task_is_privileged',
    0xFFFFFFF00A91D6E8: '__ZN14IOPMrootDomain9isLPWModeEv',
    0xFFFFFFF00ABD4640: '_apfs_smr',
    # subr_log.c's logwakeup, read from the code (the log-open flag, selwakeup, then SIGIO to the pgid or pid):
    # DiskImages' one entry between kauth_cred_unref and mach_absolute_time, where its 21H461 import was logwakeup.
    0xFFFFFFF00A6737F4: '_logwakeup',
    # OSMetaClass::getMetaClassWithName: NULL for no name, then sAllClassesDict->getObject(name) under
    # sAllClassesLock (the virtual call's ptrauth discriminator 0x5837 is OSDictionary::getObject(const OSSymbol *)'s).
    0xFFFFFFF00A7A17B8: '__ZN11OSMetaClass20getMetaClassWithNameEPK8OSSymbol',
    # vfs_vnodecovered_noblock: mp->mnt_vnodecovered (+0x38, its signed pointer's discriminator 0x7e60) through
    # vnode_getwithref_noblock, else NULL, as ours; vfs_vnodecovered is the same code before it, through
    # vnode_getwithref. The sandbox's only entry between vfs_statfs and vm_deallocate, where its 21H461 import was
    # vm_allocate_external: the GOT pass named it that, and the sandbox's mount_notify_mount hook took a kern_return_t
    # for the covered vnode at devfs's mount in bsd_init (a data abort in vnode_vtype).
    0xFFFFFFF00A3218C8: '_vfs_vnodecovered_noblock',
}


def names_for(kernelcache: str) -> dict[int, str]:
    with Path(kernelcache).open('rb') as data:
        digest = hashlib.file_digest(data, 'sha256').hexdigest()
    return NAMES if digest == KERNELCACHE_SHA256 else {}
