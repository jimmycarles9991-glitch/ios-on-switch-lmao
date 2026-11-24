import Darwin
import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Kernel parked BOs (TegraGM20BInfoBoPark, bit 28) on the Metal facade and raw user client:
/// 1. Idle chunks park with VM_PROT_NONE on give-back (phys_footprint drops >= 3 MiB) and unpark cleanly with zero
///    imports and zero host-zeroed bytes.
/// 2. Shared MTLHeaps back on write-combined kernel BOs, keep an identical [buf contents] pointer across Volatile ->
///    give-back -> NonVolatile, and incur zero host-zeroed bytes.
/// 3. Parking cached BOs past the 128 MiB global cap triggers kernel reclaim; BoUnpark reports Reclaimed cleanly and a
///    second connection's fresh BO sees all zeros (no cross-connection stale data).
struct ParkedBoPathTest: GPUPathTest {
    let id = "gpu.path.parked_bo"
    let name = "Idle chunks and shared heaps park without footprint and unpark or re-back cleanly"
    let bit: Int? = 28
    let counters: [String] = []

    static let idleNs: UInt64 = 2_000_000_000
    static let chunkBytes = 4 << 20
    static let heapBytes = 4 << 20
    static let unparkResidentCeilingUs = 200
    static let unparkReclaimedCeilingUs = 1500
    static let minFootprintDropKb = 3072
    static let reclaimCount = 36
    static let boAllocMethod: UInt32 = 2
    static let boFreeMethod: UInt32 = 4
    static let boParkMethod: UInt32 = 35
    static let boUnparkMethod: UInt32 = 36
    static let parkGuardFlag: UInt32 = 1
    static let unparkReclaimed: UInt32 = 1

    static func footprint() -> UInt64 {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<natural_t>.size)
        let status = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        return status == KERN_SUCCESS ? info.phys_footprint : 0
    }

    static func allocCached(_ conn: GPUConnection, bytes: UInt64) -> (handle: UInt64, token: UInt32)? {
        let input = GPUConnection.words([
            UInt32(bytes & 0xffff_ffff), UInt32(bytes >> 32), 12, 0,
        ])
        let (kr, reply) = conn.call(Self.boAllocMethod, input, outputSize: 16)
        guard kr == KERN_SUCCESS else { return nil }
        let handle = GPUConnection.uint64(reply, 0)
        let token = GPUConnection.uint32(reply, 8)
        return handle != 0 && token != 0 ? (handle, token) : nil
    }

    static func freeBo(_ conn: GPUConnection, handle: UInt64) {
        let input = GPUConnection.words([UInt32(handle & 0xffff_ffff), UInt32(handle >> 32)])
        _ = conn.call(Self.boFreeMethod, input, outputSize: 0)
    }

    static func parkBo(_ conn: GPUConnection, handle: UInt64, guardNone: Bool) -> kern_return_t {
        let input = GPUConnection.words([
            UInt32(handle & 0xffff_ffff), UInt32(handle >> 32), guardNone ? Self.parkGuardFlag : 0, 0,
        ])
        return conn.call(Self.boParkMethod, input, outputSize: 0).0
    }

    static func unparkBo(
        _ conn: GPUConnection, handle: UInt64
    ) -> (kr: kern_return_t, status: UInt32, address: UInt64) {
        let input = GPUConnection.words([UInt32(handle & 0xffff_ffff), UInt32(handle >> 32), 0, 0])
        let (kr, reply) = conn.call(Self.boUnparkMethod, input, outputSize: 24)
        guard kr == KERN_SUCCESS else { return (kr, 0, 0) }
        return (kr, GPUConnection.uint32(reply, 16), GPUConnection.uint64(reply, 0))
    }

    static func touchPrivateChunk(_ gpu: GPU) async throws -> UInt64 {
        let buf = autoreleasepool {
            gpu.device.makeBuffer(length: Self.chunkBytes, options: .storageModePrivate)
        }
        guard let buf else { throw TestFailure("failed to allocate 4 MiB private buffer") }
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.fill(buffer: buf, range: 0 ..< Self.chunkBytes, value: 0x3A)
            blit.endEncoding()
        }
        return Self.footprint()
    }

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let initialMem = GM20BFacade.memory(of: gpu.device)
        guard initialMem["chunk_parks"] != nil else {
            PathParts.current?.skip(GM20BFacade.missing("chunk_parks", on: gpu.device), counters: [])
            return []
        }
        var failures: [String] = []

        // Arm 1: idle chunk park/unpark and footprint drop
        let footprintPeak = try await Self.touchPrivateChunk(gpu)
        try await Task.sleep(nanoseconds: Self.idleNs)
        let memParked = GM20BFacade.memory(of: gpu.device)
        let footprintParked = Self.footprint()
        let chunkParks = (memParked["chunk_parks"] ?? 0) - (initialMem["chunk_parks"] ?? 0)
        let dropKb = Int(Int64(footprintPeak) - Int64(footprintParked)) >> 10
        metrics.set("chunk_parks", chunkParks)
        metrics.set("parked_chunks_kb", (memParked["parked_chunks"] ?? 0) >> 10)
        metrics.set("footprint_drop_kb", dropKb)
        if chunkParks <= 0 {
            failures.append("idle give-back parked no chunks")
        }
        if dropKb < Self.minFootprintDropKb {
            failures.append("footprint dropped \(dropKb) KiB while parked, expected >= \(Self.minFootprintDropKb)")
        }

        let startNs = DispatchTime.now().uptimeNanoseconds
        guard let reused = gpu.device.makeBuffer(length: Self.chunkBytes, options: .storageModePrivate) else {
            throw TestFailure("failed to re-allocate 4 MiB private buffer after park")
        }
        let unparkUs = Int((DispatchTime.now().uptimeNanoseconds - startNs) / 1000)
        let readback = try gpu.buffer(4096)
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.fill(buffer: reused, range: 0 ..< 4096, value: 0x6C)
            blit.copy(from: reused, sourceOffset: 0, to: readback, destinationOffset: 0, size: 4096)
            blit.endEncoding()
        }
        let memAfterUnpark = GM20BFacade.memory(of: gpu.device)
        let unparkRes = (memAfterUnpark["chunk_unparks_resident"] ?? 0) - (memParked["chunk_unparks_resident"] ?? 0)
        let unparkRec = (memAfterUnpark["chunk_unparks_reclaimed"] ?? 0) - (memParked["chunk_unparks_reclaimed"] ?? 0)
        let chunkImports = (memAfterUnpark["imports_made"] ?? 0) - (memParked["imports_made"] ?? 0)
        let chunkHzKb = ((memAfterUnpark["host_zeroed"] ?? 0) - (memParked["host_zeroed"] ?? 0)) >> 10
        let wrongChunk = UnsafeRawBufferPointer(start: readback.contents(), count: 4096).filter { $0 != 0x6C }.count
        metrics.set("unpark_us", unparkUs)
        metrics.set("chunk_unparks_res", unparkRes)
        metrics.set("chunk_unparks_rec", unparkRec)
        metrics.set("chunk_imports", chunkImports)
        metrics.set("chunk_hz_kb", chunkHzKb)
        if unparkRes + unparkRec <= 0 {
            failures.append("re-allocating after idle did not unpark a parked chunk")
        }
        let ceiling = unparkRec > 0 ? Self.unparkReclaimedCeilingUs : Self.unparkResidentCeilingUs
        if unparkUs > ceiling {
            failures.append("chunk unpark took \(unparkUs) us, over \(ceiling)")
        }
        if chunkImports > 0 { failures.append("chunk unpark made \(chunkImports) imports") }
        if chunkHzKb > 0 { failures.append("chunk unpark host-zeroed \(chunkHzKb) KiB") }
        if wrongChunk != 0 { failures.append("\(wrongChunk) bytes wrong on unparked chunk") }

        // Arm 2: shared MTLHeap stable [buf contents] pointer and zero host-zeroed bytes
        let memBeforeHeap = GM20BFacade.memory(of: gpu.device)
        let heapDesc = MTLHeapDescriptor()
        heapDesc.size = Self.heapBytes
        heapDesc.storageMode = .shared
        guard let heap = gpu.device.makeHeap(descriptor: heapDesc),
              let heapBuf = heap.makeBuffer(length: 4096, options: .storageModeShared)
        else {
            throw TestFailure("failed to make 4 MiB shared heap and buffer")
        }
        let ptrBefore = UInt(bitPattern: heapBuf.contents())
        memset(heapBuf.contents(), 0x4B, 4096)
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(from: heapBuf, sourceOffset: 0, to: readback, destinationOffset: 0, size: 4096)
            blit.endEncoding()
        }
        let wrongPrePurge = UnsafeRawBufferPointer(start: readback.contents(), count: 4096).filter { $0 != 0x4B }.count
        if wrongPrePurge != 0 {
            failures.append("\(wrongPrePurge) bytes wrong on shared heap before purge")
        }
        heap.setPurgeableState(.volatile)
        try await Task.sleep(nanoseconds: Self.idleNs)
        let purgedState = heap.setPurgeableState(.keepCurrent)
        let restoredFrom = heap.setPurgeableState(.nonVolatile)
        let ptrAfter = UInt(bitPattern: heapBuf.contents())
        memset(heapBuf.contents(), 0x7D, 4096)
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(from: heapBuf, sourceOffset: 0, to: readback, destinationOffset: 0, size: 4096)
            blit.endEncoding()
        }
        let memAfterHeap = GM20BFacade.memory(of: gpu.device)
        let heapHzKb = ((memAfterHeap["host_zeroed"] ?? 0) - (memBeforeHeap["host_zeroed"] ?? 0)) >> 10
        let heapParks = (memAfterHeap["heap_parks"] ?? 0) - (memBeforeHeap["heap_parks"] ?? 0)
        let heapRes = (memAfterHeap["heap_unparks_resident"] ?? 0) - (memBeforeHeap["heap_unparks_resident"] ?? 0)
        let heapRec = (memAfterHeap["heap_unparks_reclaimed"] ?? 0) - (memBeforeHeap["heap_unparks_reclaimed"] ?? 0)
        let wrongHeap = UnsafeRawBufferPointer(start: readback.contents(), count: 4096).filter { $0 != 0x7D }.count
        metrics.set("heap_purged", purgedState == .empty ? 1 : 0)
        metrics.set("heap_va_same", ptrBefore == ptrAfter ? 1 : 0)
        metrics.set("heap_hz_kb", heapHzKb)
        metrics.set("heap_parks", heapParks)
        metrics.set("heap_unparks_res", heapRes)
        metrics.set("heap_unparks_rec", heapRec)
        if purgedState != .empty && restoredFrom != .empty {
            failures.append("volatile shared heap did not purge on idle give-back")
        }
        if heapParks <= 0 {
            failures.append("idle give-back parked no shared heap")
        }
        if heapRes + heapRec <= 0 {
            failures.append("restoring purged shared heap did not unpark a parked heap")
        }
        if ptrBefore != ptrAfter {
            failures.append("shared heap [buf contents] moved across purge/restore")
        }
        if heapHzKb > 0 { failures.append("shared heap host-zeroed \(heapHzKb) KiB") }
        if wrongHeap != 0 { failures.append("\(wrongHeap) bytes wrong on restored shared heap") }

        // Arm 3: reclaim past 128 MiB parked cap + cross-connection zero-fill safety
        guard let connA = GPUConnection(), let connB = GPUConnection() else {
            throw TestFailure("failed to open raw GPU connections for reclaim check")
        }
        var parkedHandles: [(handle: UInt64, token: UInt32, unparkedLive: Bool, addr: UInt64)] = []
        defer {
            for item in parkedHandles {
                if item.unparkedLive && item.addr != 0 {
                    connA.unmap(GPUConnection.Mapping(token: item.token, address: item.addr))
                }
                Self.freeBo(connA, handle: item.handle)
            }
        }
        for i in 0 ..< Self.reclaimCount {
            guard let bo = Self.allocCached(connA, bytes: UInt64(Self.chunkBytes)),
                  let map = connA.map(bo.token, bytes: Self.chunkBytes, readOnly: false)
            else {
                throw TestFailure("raw cached BoAlloc/Map failed at index \(i)")
            }
            memset(UnsafeMutableRawPointer(bitPattern: UInt(map.address))!, 0xA5, 4096)
            let parkKr = Self.parkBo(connA, handle: bo.handle, guardNone: true)
            if parkKr != KERN_SUCCESS {
                connA.unmap(map)
                Self.freeBo(connA, handle: bo.handle)
                failures.append("raw BoPark failed at index \(i) with \(GPUConnection.hex(parkKr))")
                break
            }
            parkedHandles.append((bo.handle, bo.token, false, 0))
        }
        try await Task.sleep(nanoseconds: 300_000_000)
        var sawReclaimed = false
        for i in 0 ..< min(parkedHandles.count, 8) {
            let res = Self.unparkBo(connA, handle: parkedHandles[i].handle)
            if res.kr != KERN_SUCCESS {
                failures.append("raw BoUnpark failed at index \(i) with \(GPUConnection.hex(res.kr))")
                break
            }
            if res.status == Self.unparkReclaimed {
                sawReclaimed = true
                break
            }
            parkedHandles[i].unparkedLive = true
            parkedHandles[i].addr = res.address
            try await Task.sleep(nanoseconds: 200_000_000)
        }
        metrics.set("reclaim_seen", sawReclaimed ? 1 : 0)
        if !sawReclaimed {
            failures.append("parking \(Self.reclaimCount * (Self.chunkBytes >> 20)) MiB saw no kernel reclaim")
        }
        for item in parkedHandles {
            if item.unparkedLive && item.addr != 0 {
                connA.unmap(GPUConnection.Mapping(token: item.token, address: item.addr))
            }
            Self.freeBo(connA, handle: item.handle)
        }
        parkedHandles.removeAll()

        guard let fresh = Self.allocCached(connB, bytes: UInt64(Self.chunkBytes)),
              let freshMap = connB.map(fresh.token, bytes: Self.chunkBytes, readOnly: false)
        else {
            throw TestFailure("connB fresh BoAlloc/Map failed")
        }
        defer {
            connB.unmap(freshMap)
            Self.freeBo(connB, handle: fresh.handle)
        }
        let head = UnsafeRawBufferPointer(start: freshMap.base, count: 4096)
        let tail = UnsafeRawBufferPointer(start: freshMap.base.advanced(by: Self.chunkBytes - 4096), count: 4096)
        let staleBytes = head.filter { $0 != 0 }.count + tail.filter { $0 != 0 }.count
        metrics.set("cross_conn_stale", staleBytes)
        if staleBytes != 0 {
            failures.append("fresh BO on second connection saw \(staleBytes) non-zero bytes")
        }
        return failures
    }
}
