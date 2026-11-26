"""24B119's kernel functions and data that IOSkywalkFamily and mDNSOffloadUserClient-Embedded import, named by hand.

24B119's kernel is xnu-13432.42.3, our tree xnu-12377.121.6. Every name below sits in its entry's GOT gap (the GOT is
sorted by imported name) and at its place in Apple's run of the source file's functions between named neighbours,
which follows the source order but for the swaps the comments name. Most bodies match what our kernel compiles for
the name, with offsets shifted where 13432's structs differ (struct ifnet +0x100, struct ifaddr -0x10 against our
larger lock, __kern_packet +8 after pkt_trace_tag); where 13432 rewrote a function, its place, its gap and its
arguments decide, as the comments say.
"""

SKYWALK = {
    # kpi_interface.c. The two wake setters differ only in their IFXF flag (0x4000000 inband wake packet tagging,
    # 0x8000000 low power wake); set_offload_enabled is set_offload_common with enabled 0 (set_offload passes 1);
    # set_tso_mtu checks AF_INET (2) and AF_INET6 (0x1e) and the 0x2b limit; set_rx_flow_steering sets IFXF 0x200000;
    # get_local_ports is get_local_ports_extended(ifp, 0, 1, ports); get_rx_steering_rules takes an ioref, asks the
    # flowswitch and returns ENXIO (6) or ENODEV (0x13) as ours. enable_synchronous_input is 13432's, after
    # enable_cellular_thread_group with the same three VERIFYs (kpi_interface.c lines 3957, 3962 and 3963) and IFXF
    # 0x20000000 in place of 0x10000000; macOS 27.2's kernel (xnu-13432.41.4) exports it under this name with the
    # same body and line numbers.
    0xFFFFFFF00A36D08C: '_ifnet_set_inband_wake_packet_tagging',
    0xFFFFFFF00A36D13C: '_ifnet_set_low_power_wake',
    0xFFFFFFF00A3C4860: '_ifnet_set_offload_enabled',
    0xFFFFFFF00A3C4880: '_ifnet_set_tso_mtu',
    0xFFFFFFF00A3C7F68: '_ifnet_get_local_ports',
    0xFFFFFFF00A3C8CA4: '_ifnet_set_rx_flow_steering',
    0xFFFFFFF00A3C8D7C: '_ifnet_enable_synchronous_input',
    0xFFFFFFF00A3C8DF8: '_ifnet_get_rx_steering_rules',
    # ifaddr_ifnet loads ifa_ifp (+0x30 here, +0x40 in ours, as ifaddr_netmask's ifa_netmask is +0x28 against +0x38);
    # ifaddr_get_ia6_flags is the one function between ifaddr_findbestforaddr and ifmaddr_address, an AF_INET6 check
    # under the address lock.
    0xFFFFFFF00A3C755C: '_ifaddr_ifnet',
    0xFFFFFFF00A3C760C: '_ifaddr_get_ia6_flags',
    # channel_kern.c from its top: notify (ENXIO on KR_DROP, else the ring's ckr_na_notify with kernproc), reclaim
    # (a tail call into kr_reclaim), then after get_next_slot and advance_slot: ring_get_context (+0x110),
    # increment_ring_stats (a tail call into kr_update_stats), slot_get_packet, slot_attach_packet and
    # slot_detach_packet (13432's bodies, without our bounds checks; the same arguments), and get_service_class
    # after tx_refill.
    0xFFFFFFF00A702720: '_kern_channel_notify',
    0xFFFFFFF00A70274C: '_kern_channel_reclaim',
    0xFFFFFFF00A702A78: '_kern_channel_ring_get_context',
    0xFFFFFFF00A702B7C: '_kern_channel_increment_ring_stats',
    0xFFFFFFF00A702C0C: '_kern_channel_slot_get_packet',
    0xFFFFFFF00A702C2C: '_kern_channel_slot_attach_packet',
    0xFFFFFFF00A702C88: '_kern_channel_slot_detach_packet',
    0xFFFFFFF00A7030B4: '_kern_channel_get_service_class',
    # nexus_ioctl.c, nexus.c and nx_netif.c: nxioctl_kernel loads ncd_nxctl and tail-calls _nxioctl (placed after
    # nxioctl here); register_domain_provider names the provider with
    # snprintf and adds it with nxdom_prov_add (nxdom_prov_free on failure); attr_create allocates and returns EINVAL
    # (0x16) for a NULL out pointer; between increment_queue_stats and llink_add come rx_enqueue (frees the rejected
    # packets, pops the thread marks) and tx_dequeue (the netif dequeue, then increment_queue_stats); llink_remove
    # checks the nexus is a netif one and tail-calls nx_netif_llink_remove.
    0xFFFFFFF00A72C734: '_nxioctl_kernel',
    0xFFFFFFF00A72D164: '_kern_nexus_register_domain_provider',
    0xFFFFFFF00A72D6B0: '_kern_nexus_attr_create',
    0xFFFFFFF00A76F640: '_kern_netif_queue_rx_enqueue',
    0xFFFFFFF00A76F7CC: '_kern_netif_queue_tx_dequeue',
    0xFFFFFFF00A76F9A0: '_kern_nexus_netif_llink_remove',
    # packet_kern.c, in source order. The link flag pairs differ by their bit (broadcast 0x1, multicast 0x2, ethfcs
    # 0x4); get_service_class and get_traffic_class are the same switch, told apart by place; clone passes flags 0
    # and is placed before clone_internal (clone_nosleep passes 1, after it); set_next stores and get_next loads
    # pkt_nextpkt (+0xa8), in that order here; trace_event is an empty function in this RELEASE kernel. The three
    # set_fpd_* store into the packet here: the sequence number at +0xd0, and in the halfword at +0xd4 the context id
    # (bits 0-5), the command (bits 6-8, EINVAL above 7) and bit 15 to mark either set; pp_alloc_packet zeroes both.
    # Ours are stubs that return 0: the setters' own read-modify-writes are the kernel's only loads of that
    # halfword, so nothing sees the bit. Their only callers are IOSkywalkFamily's IOSkywalkNetworkPacket::setFPD*
    # methods. No kext here imports those methods, and NetworkingDriverKit's packet setters include none for FPD.
    0xFFFFFFF00A792AE4: '_kern_packet_set_headroom',
    0xFFFFFFF00A792B08: '_kern_packet_get_headroom',
    0xFFFFFFF00A792B3C: '_kern_packet_set_link_header_length',
    0xFFFFFFF00A792B60: '_kern_packet_get_link_header_length',
    0xFFFFFFF00A792B68: '_kern_packet_set_link_broadcast',
    0xFFFFFFF00A792B7C: '_kern_packet_get_link_broadcast',
    0xFFFFFFF00A792B88: '_kern_packet_set_link_multicast',
    0xFFFFFFF00A792B9C: '_kern_packet_set_link_ethfcs',
    0xFFFFFFF00A792BB0: '_kern_packet_get_link_multicast',
    0xFFFFFFF00A792BBC: '_kern_packet_get_link_ethfcs',
    0xFFFFFFF00A792BF0: '_kern_packet_get_transport_traffic_background',
    0xFFFFFFF00A792BFC: '_kern_packet_get_transport_traffic_realtime',
    0xFFFFFFF00A792C14: '_kern_packet_get_transport_new_flow',
    0xFFFFFFF00A792C20: '_kern_packet_get_transport_last_packet',
    0xFFFFFFF00A792D1C: '_kern_packet_get_service_class',
    0xFFFFFFF00A792E00: '_kern_packet_get_compression_generation_count',
    0xFFFFFFF00A792E28: '_kern_packet_get_service_class_index',
    0xFFFFFFF00A792F10: '_kern_packet_is_high_priority',
    0xFFFFFFF00A792F78: '_kern_packet_set_traffic_class',
    0xFFFFFFF00A792FA8: '_kern_packet_get_traffic_class',
    0xFFFFFFF00A793080: '_kern_packet_set_inet_checksum',
    0xFFFFFFF00A7930A0: '_kern_packet_get_inet_checksum',
    0xFFFFFFF00A7931C4: '_kern_packet_get_data_length',
    0xFFFFFFF00A79320C: '_kern_packet_get_next_buflet',
    0xFFFFFFF00A793244: '_kern_packet_finalize',
    0xFFFFFFF00A7933C4: '_kern_packet_get_object_index',
    0xFFFFFFF00A7933CC: '_kern_packet_get_timestamp',
    0xFFFFFFF00A793400: '_kern_packet_set_timestamp',
    0xFFFFFFF00A79342C: '_kern_packet_get_timestamp_requested',
    0xFFFFFFF00A793440: '_kern_packet_tx_completion',
    0xFFFFFFF00A793568: '_kern_packet_set_tx_completion_status',
    0xFFFFFFF00A7935B4: '_kern_packet_get_group_start',
    0xFFFFFFF00A7935D0: '_kern_packet_get_group_end',
    0xFFFFFFF00A7935DC: '_kern_packet_get_expire_time',
    0xFFFFFFF00A79363C: '_kern_packet_get_expiry_action',
    0xFFFFFFF00A79369C: '_kern_packet_get_token',
    0xFFFFFFF00A793798: '_kern_packet_get_packetid',
    0xFFFFFFF00A793800: '_kern_packet_set_vlan_tag',
    0xFFFFFFF00A79381C: '_kern_packet_get_vlan_tag',
    0xFFFFFFF00A793854: '_kern_packet_get_app_metadata',
    0xFFFFFFF00A793894: '_kern_packet_set_wake_flag',
    0xFFFFFFF00A7938EC: '_kern_packet_clone',
    0xFFFFFFF00A793E54: '_kern_packet_set_next',
    0xFFFFFFF00A793E5C: '_kern_packet_get_next',
    0xFFFFFFF00A793FD4: '_kern_packet_get_trace_id',
    0xFFFFFFF00A793FDC: '_kern_packet_set_trace_id',
    0xFFFFFFF00A793FE4: '_kern_packet_trace_event',
    0xFFFFFFF00A79407C: '_kern_packet_set_fpd_sequence_number',
    0xFFFFFFF00A794094: '_kern_packet_set_fpd_context_id',
    0xFFFFFFF00A7940B0: '_kern_packet_set_fpd_command',
    0xFFFFFFF00A7940DC: '_kern_packet_get_flowid',
    0xFFFFFFF00A794100: '_kern_packet_set_trace_tag',
    0xFFFFFFF00A794108: '_kern_packet_get_trace_tag',
    0xFFFFFFF00A794110: '_kern_packet_get_tx_nexus_port_id',
    0xFFFFFFF00A794134: '_kern_packet_get_protocol_segment_size',
    0xFFFFFFF00A79413C: '_kern_packet_set_segment_count',
    0xFFFFFFF00A794144: '_kern_packet_get_priv',
    0xFFFFFFF00A79414C: '_kern_packet_set_priv',
    0xFFFFFFF00A794154: '_kern_packet_get_tso_flags',
    # pbufpool_kern.c: get_context (PPF_EXTERNAL, then pp_ctx_retain), segment_get_index (+0x48), the alloc pairs told
    # apart by the nosleep flag 1, free (the handle's low tag bits cleared, then pp_free_packet), free_batch and
    # free_buffer (tail calls), free_chain and destroy with the VERIFYs of our lines 396 and 424 at 13432's 403 and
    # 431 (the same +7 shift).
    0xFFFFFFF00A79B1F4: '_kern_pbufpool_get_context',
    0xFFFFFFF00A79B248: '_kern_segment_get_index',
    0xFFFFFFF00A79B250: '_kern_pbufpool_alloc',
    0xFFFFFFF00A79B2B4: '_kern_pbufpool_alloc_nosleep',
    0xFFFFFFF00A79B318: '_kern_pbufpool_alloc_batch',
    0xFFFFFFF00A79B39C: '_kern_pbufpool_alloc_batch_nosleep',
    0xFFFFFFF00A79B420: '_kern_pbufpool_free',
    0xFFFFFFF00A79B428: '_kern_pbufpool_free_batch',
    0xFFFFFFF00A79B438: '_kern_pbufpool_free_chain',
    0xFFFFFFF00A79B480: '_kern_pbufpool_alloc_buffer',
    0xFFFFFFF00A79B488: '_kern_pbufpool_alloc_buffer_nosleep',
    0xFFFFFFF00A79B490: '_kern_pbufpool_free_buffer',
    0xFFFFFFF00A79B494: '_kern_pbufpool_destroy',
    # IOService::applyToClients(block) calls the vtable's applyToClients (slot +0x448) with its block trampoline;
    # IOInterruptDispatchSourceInterrupt (IOUserServer.cpp, between IODispatchSource::free and
    # IOInterruptDispatchSource::init) counts (+0x28) and stamps (+0x30) the interrupt under the ivars' lock and
    # wakes the waiter (+0x20).
    0xFFFFFFF00A83C13C: '__ZN9IOService14applyToClientsEU13block_pointerFvPS_E',
    0xFFFFFFF00A8D85D4: '__Z34IOInterruptDispatchSourceInterruptP8OSObjectPvP9IOServicei',
    # Each class's metaClass pointer: its value is the class's gMetaClass.
    0xFFFFFFF007C25458: '__ZN25IODataQueueDispatchSource9metaClassE',
    0xFFFFFFF007C25E50: '__ZN25IOInterruptDispatchSource9metaClassE',
}
