import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// A connection's writer promise words and its clock anchor table lie in separate regions of its fence page
/// (GPUConnection.FencePage). Part A, on a TegraGM20B connection of the test's own: ClockAnchors answers (anchors_kr 0)
/// with the table at the mirror's offset in the GPU's mapping of the page (anchors_offset) holding at least the boot's
/// anchor (anchors_published); a copy-only channel, 32 IOSurfaces imported and a promise of each one's writes kept by
/// it (promises_made: ids 0 to 31), then three rounds of cancelling all 32 and making them again. No byte of the anchor
/// table moves (anchor_bytes_changed, the most over the rounds, 0; anchors_published_after equal to anchors_published:
/// the display is on, so the GPU is not rail-gated meanwhile) and every promise word moves by exactly one each round
/// (promise_words_set 32). Part B, on the app's Metal device: a command buffer behind one waiting for an unsignalled
/// shared event, rendering into an IOSurface, is held and promises its write (held_promised: this process's
/// writer_promises path count moved across it); the event is signalled 30 ms after the commits, and the writer's
/// waitUntilScheduled returns no sooner than that, as Metal's does (held 1), both completing; and then a render pass's
/// four stage-boundary timestamp samples lie inside the sampleTimestamps pair around it (samples_outside 0), the blit's
/// resolve equal to the CPU's (cpu_blit_differ 0); lead_us reported. A kernel whose promise words lie over the anchor
/// table moves the table's published count by one a round and the anchors behind it, so Part A fails
/// anchor_bytes_changed and anchors_published_after, and the app's samples convert with a wrong anchor (samples_outside
/// 4).
struct FencePageRegionsTest: HWTest {
    let id = "gpu.kernel.fence_page_regions"
    let name = "Writer promises made and released leave the connection's clock anchors, and samples keep their time"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    static let surfaces = GPUConnection.FencePage.promiseWords, rounds = 3

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        guard let connection = GPUConnection() else { throw TestFailure("no TegraGM20B connection") }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        try Self.regions(connection, &metrics, &checks)
        Self.held(gpu, connection, &metrics, &checks)
        let sampled = try PromiseAnchors.sampled(gpu)
        metrics.set("samples_outside", sampled.outside)
        metrics.set("lead_us", sampled.leadMicroseconds, digits: 1)
        metrics.set("gpu_us", sampled.gpuMicroseconds, digits: 1)
        metrics.set("cpu_blit_differ", sampled.differ)
        metrics.set("samples_paths", sampled.paths.joined(separator: ","))
        checks.append(("samples_outside=\(sampled.outside) not 0", sampled.outside == 0))
        checks.append(("cpu_blit_differ=\(sampled.differ) not 0", sampled.differ == 0))
        checks.append(("the sampling and resolving command buffers raw", sampled.paths == ["gm20b", "gm20b"]))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// Part A
    static func regions(
        _ connection: GPUConnection, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]
    ) throws {
        typealias Page = GPUConnection.FencePage
        let (infoKr, info) = connection.call(Page.getInfo, [], outputSize: Page.infoBytes)
        guard infoKr == KERN_SUCCESS else { throw TestFailure("GetInfo refused: \(GPUConnection.hex(infoKr))") }
        let (anchorsKr, reply) = connection.call(Page.clockAnchors, [], outputSize: Page.clockAnchorsReplyBytes)
        let offset = Int(GPUConnection.uint64(reply, 0) % UInt64(Page.gpuBytes))
        metrics.set("anchors_kr", GPUConnection.hex(anchorsKr))
        metrics.set("anchors_offset", offset)
        checks.append(("anchors_kr=\(GPUConnection.hex(anchorsKr)) not 0", anchorsKr == KERN_SUCCESS))
        checks.append(
            ("anchors_offset=\(offset) not the mirror's \(Page.anchorsFirst)", offset == Page.anchorsFirst)
        )
        let token = GPUConnection.uint32(info, Page.infoFenceToken)
        guard let page = connection.map(token, bytes: Page.promisesFirst + 4 * Page.promiseWords) else {
            throw TestFailure("the fence page (token \(token)) did not map")
        }
        defer { connection.unmap(page) }
        let word = { (byte: Int) in page.base.load(fromByteOffset: byte, as: UInt32.self) }
        let anchors = { (0 ..< Page.anchorsBytes / 4).map { word(Page.anchorsFirst + 4 * $0) } }
        let promiseWords = { (0 ..< Page.promiseWords).map { word(Page.promisesFirst + 4 * $0) } }
        let published = word(Page.anchorsFirst)
        metrics.set("anchors_published", Int(published))
        checks.append(("anchors_published=\(published): not even the boot's anchor", published >= 1))

        let (channelKr, created) = connection.call(
            Page.channelCreate, GPUConnection.words([Page.copyOnlyEngines, 0]), outputSize: Page.channelReplyBytes
        )
        guard channelKr == KERN_SUCCESS else {
            throw TestFailure("ChannelCreate refused: \(GPUConnection.hex(channelKr))")
        }
        let channel = GPUConnection.uint32(created, 0)
        defer { _ = connection.call(Page.channelDestroy, GPUConnection.words([channel, 0]), outputSize: 0) }
        var handles: [UInt64] = []
        var surfaces: [IOSurface] = []
        defer {
            for handle in handles {
                let words = GPUConnection.words(PromiseCancelPathTest.halves(handle))
                _ = connection.call(PromiseCancelPathTest.boFree, words, outputSize: 0)
            }
            withExtendedLifetime(surfaces) {}
        }
        for _ in 0 ..< Self.surfaces {
            let rowBytes = PromiseAnchors.side * 4
            let properties: [IOSurfacePropertyKey: Any] = [
                .width: PromiseAnchors.side, .height: PromiseAnchors.side, .bytesPerElement: 4,
                .bytesPerRow: rowBytes, .allocSize: rowBytes * PromiseAnchors.side, .pixelFormat: 0x4247_5241,
            ]
            guard let surface = IOSurface(properties: properties) else {
                throw TestFailure("IOSurface creation failed")
            }
            surfaces.append(surface)
            handles.append(try PromiseCancelPathTest.imported(surface, connection))
        }

        let before = anchors(), start = promiseWords()
        var made = handles.map { PromiseCancelPathTest.make($0, connection, channel: channel) }
        let ids = Set(made.filter { $0.0 == KERN_SUCCESS }.map(\.1).filter { $0 < UInt32(Page.promiseWords) })
        var changed = 0, refused = 0
        var stepped = [Bool](repeating: true, count: Page.promiseWords)
        for round in 1 ... Self.rounds {
            for (kr, promise) in made where kr == KERN_SUCCESS {
                if PromiseCancelPathTest.cancel(promise, connection) != KERN_SUCCESS { refused += 1 }
            }
            made = []
            changed = max(changed, zip(anchors(), before).map { Self.bytesOff($0, $1) }.reduce(0, +))
            for (i, value) in promiseWords().enumerated() where value != start[i] &+ UInt32(round) {
                stepped[i] = false
            }
            if round < Self.rounds {
                made = handles.map { PromiseCancelPathTest.make($0, connection, channel: channel) }
                refused += made.filter { $0.0 != KERN_SUCCESS }.count
            }
        }
        let publishedAfter = word(Page.anchorsFirst)
        let set = stepped.filter { $0 }.count
        metrics.set("promises_made", ids.count)
        metrics.set("anchor_bytes_changed", changed)
        metrics.set("anchors_published_after", Int(publishedAfter))
        metrics.set("promise_words_set", set)
        metrics.set("promise_calls_refused", refused)
        checks.append(("promises_made=\(ids.count) not \(Self.surfaces), ids 0 to 31", ids.count == Self.surfaces))
        checks.append(("anchor_bytes_changed=\(changed) not 0", changed == 0))
        checks.append(
            ("anchors_published_after=\(publishedAfter) not \(published)", publishedAfter == published)
        )
        checks.append(("promise_words_set=\(set) not \(Page.promiseWords)", set == Page.promiseWords))
    }

    static func bytesOff(_ a: UInt32, _ b: UInt32) -> Int {
        let x = a ^ b
        return (0 ..< 4).filter { (x >> (8 * UInt32($0))) & 0xff != 0 }.count
    }

    /// Part B's hold: this process's writer_promises path count across it (debug kernels, GPUPaths)
    static func held(
        _ gpu: GPU, _ connection: GPUConnection, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]
    ) {
        let before = GPUPaths.read(connection)
        let held: PromiseAnchors.Held
        do {
            held = try PromiseAnchors.held(gpu)
        } catch {
            checks.append(("the held command buffer: \(GPU.describe(error))", false))
            return
        }
        let after = GPUPaths.read(connection)
        let completed = Int(MTLCommandBufferStatus.completed.rawValue)
        metrics.set("held", held.scheduledAtSignal)
        metrics.set("held_status", "\(held.head),\(held.writer)")
        metrics.set("held_paths", "\(held.headPath),\(held.writerPath)")
        checks.append(("the held writer ended \(held.writer), not completed", held.writer == completed))
        checks.append(("the held writer raw (\(held.writerPath))", held.writerPath == "gm20b"))
        checks.append(("the head ended \(held.head) (\(held.headPath)), not completed", held.head == completed))
        checks.append(("the writer's waitUntilScheduled returned before the signal", held.scheduledAtSignal))
        switch (before, after) {
        case (.counts(let first), .counts(let last)):
            let promised = last.taken("writer_promises", since: first)
            metrics.set("held_promised", promised)
            checks.append(("held_promised=\(promised): no writer promise while it was held", promised >= 1))
        case (.unreadable(let asked, let error), _) where asked == "release_kernel" && error == ENOENT:
            metrics.set("held_promised", "release_kernel")
        default:
            metrics.set("held_promised", "unreadable")
            checks.append(("the path counters around the hold are unreadable", false))
        }
    }
}
