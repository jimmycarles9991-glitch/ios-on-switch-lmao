import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// 22 Nov, boot B: an idle second gave the Metal driver's pooled memory back and the first frame after every pause
/// made it again on the drawing thread, an 8 MiB heap's pages zero-filled there (4.4 ms); launching apps zero-filled
/// their chunks the same way (4.3-9.1 ms). Memory made again now comes from the kernel's zeroed spares: a private
/// heap's arena and the device's chunks are cached BoAllocs (drivers/TegraGM20BMetal/arena.cpp, memory.cpp). A burst
/// of clears into a volatile private heap's texture and a target of the device's own, and a shared buffer's fill; two
/// idle seconds, past this app's idle give-back (which purges the heap and frees the chunks); the same burst again.
/// The second burst must import nothing, take no untouched host pages, and finish every object call (the heap's
/// restore at NonVolatile included) under the ceiling, which a kernel zero-fill of a chunk of this size on the calling
/// thread cannot meet. A miss of a smaller BoAlloc is named in the debug kernel's spare-misses line.
struct IdleRedrawPathTest: GPUPathTest {
    let id = "gpu.path.idle_redraw"
    let name = "Drawing after an idle pause takes its memory from the kernel's spares, nothing zeroed on the caller"
    let counters: [String] = []
    static let ceilingUs = 1500, heapBytes = 8 << 20, width = 1280, height = 720, idleNs: UInt64 = 2_000_000_000

    struct Calls {
        var slowestUs = 0, slowest = "-"

        mutating func timed<T>(_ what: String, _ call: () throws -> T) rethrows -> T {
            let start = DispatchTime.now().uptimeNanoseconds
            let made = try call()
            let us = Int((DispatchTime.now().uptimeNanoseconds - start) / 1000)
            if us > slowestUs { (slowestUs, slowest) = (us, what) }
            return made
        }
    }

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        guard GM20BFacade.memory(of: gpu.device)["imports_made"] != nil else {
            throw TestFailure(GM20BFacade.missing("imports_made", on: gpu.device))
        }
        let descriptor = MTLHeapDescriptor()
        descriptor.size = Self.heapBytes
        descriptor.storageMode = .private
        guard let heap = gpu.device.makeHeap(descriptor: descriptor) else { throw TestFailure("makeHeap returned nil") }
        var first = Calls()
        try await burst(gpu, heap, &first)
        try await Task.sleep(nanoseconds: Self.idleNs)
        let before = GM20BFacade.memory(of: gpu.device)
        var second = Calls()
        let purged = second.timed("heap_restore") { heap.setPurgeableState(.nonVolatile) } == .empty
        try await burst(gpu, heap, &second)
        let after = GM20BFacade.memory(of: gpu.device)
        let moved = { (key: String) in (after[key] ?? 0) - (before[key] ?? 0) }
        metrics.set("first_slowest_us", first.slowestUs)
        metrics.set("slowest_us", second.slowestUs)
        metrics.set("slowest", second.slowest)
        metrics.set("heap_purged", purged ? 1 : 0)
        metrics.set("arena_restores", moved("arena_restores"))
        metrics.set("imports", moved("imports_made"))
        metrics.set("host_zeroed_kb", moved("host_zeroed") >> 10)
        if moved("arena_restores") == 0 {
            PathParts.current?.skip("the idle seconds purged no heap: its restore was not judged", counters: [])
        }
        var failures: [String] = []
        if moved("imports_made") > 0 { failures.append("the second burst made \(moved("imports_made")) imports") }
        if moved("host_zeroed") > 0 {
            failures.append("the second burst imported \(moved("host_zeroed") >> 10) KiB of untouched host pages")
        }
        if second.slowestUs > Self.ceilingUs {
            failures.append(
                "\(second.slowest) took \(second.slowestUs) us, over \(Self.ceilingUs): memory made on the calling "
                    + "thread (the debug kernel's spare-misses line names a BoAlloc no spare served)"
            )
        }
        return failures
    }

    /// Each object it makes timed, its work waited for, and the heap made volatile again
    func burst(_ gpu: GPU, _ heap: MTLHeap, _ calls: inout Calls) async throws {
        let placed = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: Self.width / 2, height: Self.height / 2, mipmapped: false
        )
        placed.storageMode = .private
        placed.usage = [.renderTarget, .shaderRead]
        guard let inHeap = calls.timed("heap_texture", { heap.makeTexture(descriptor: placed) }) else {
            throw TestFailure("the heap made no texture")
        }
        let own = try calls.timed("texture") { try gpu.texture(.bgra8Unorm, Self.width, Self.height) }
        let shared = try calls.timed("buffer") { try gpu.buffer(1 << 20) }
        try await gpu.run { commands in
            for target in [inHeap, own] {
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
            }
            let blit = commands.makeBlitCommandEncoder()!
            blit.fill(buffer: shared, range: 0 ..< shared.length, value: 0x5A)
            blit.endEncoding()
        }
        heap.setPurgeableState(.volatile)
    }
}
