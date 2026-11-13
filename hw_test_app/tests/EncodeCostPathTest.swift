import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The raw encode's CPU cost of a command buffer shaped as Core Animation's (raw_encode_profile.h, debug builds):
/// `passes` passes of Core Animation's untextured fill (PBGRAXm_A2Xhf, BGRA8 beside its memoryless colour 1), each draw
/// binding its vertices, transform and zero uniforms by setBytes with a scissor of its own, `buffers` command buffers
/// at each of two draw counts, each after as many at that count (whose recording storage they reuse, as a steady frame
/// does). Per command buffer at each count: the encode's wall time and its parts (us_<part>, and
/// walk_us the loop's time outside every part), its events (mallocs, frees, locks, sends, uses, use_adds, descriptor
/// builds and uploads, geometry vertices) and bindings applied per draw. A command buffer's mallocs, frees, locks,
/// Objective-C sends, job uses that reached a job's list, vertices the CPU read and set-0 uploads must not grow with
/// its draws, none may pass what the host's facade harness counts of the same command buffers with the encode and its
/// retire lock-free (raw/test/facadecost.mm: at most the trim's lock once in 64 command buffers, no send, 4 allocations
/// and 2 frees), and every command buffer is the raw path's. Beside the encode's own: the runtime's locks on every
/// thread over the batch (facade_locks, raw_lock.h's count of its wrappers), at most 4 a command buffer, and the growth
/// of the blocks in use in every malloc zone while the batch is recorded and held (held_blocks), a coarse bound on what
/// each keeps allocated (the whole process's, so no per-draw check). The pooled encoders' blocks (raw_object_pool.h):
/// none freed in the batch (its pool kept too few for what the batch holds at once), and the most each pool made in the
/// process. The profile is the facade's; a release runtime has none and the test skips.
struct EncodeCostPathTest: HWTest {
    let id = "gpu.path.encode_cost"
    let name =
        "A Core Animation shaped command buffer's encode by part, with no allocation, lock, message or vertex read per draw"
    var subsystem: Subsystem { .metal }
    static let width = 256, height = 256, passes = 6, drawCounts = [8, 16], buffers = 24, warmups = 20

    static let parts = [
        "resources", "loop", "state", "apply", "pass", "pass_begin", "fusion", "pass_end", "pass_prologue", "draw",
        "prepare", "geometry", "variant", "descriptors", "textures", "bind", "use", "uniforms", "tail", "commit",
        "exec", "retire",
    ]
    static let events = [
        "mallocs", "frees", "locks", "sends", "uses", "use_adds", "descriptor_builds", "descriptor_uploads",
        "geometry_vertices", "facade_locks", "held_blocks", "render_encoders_freed",
    ]
    static let perDrawFree = [
        "mallocs", "frees", "locks", "sends", "use_adds", "geometry_vertices", "facade_locks", "descriptor_uploads",
    ]
    static let ceilings: [(event: String, most: Double)] = [
        ("locks", 0.1), ("sends", 0), ("mallocs", 4), ("frees", 2), ("geometry_vertices", 0), ("facade_locks", 4),
        ("held_blocks", heldBlocksMost), ("render_encoders_freed", 0),
    ]
    /// coarse: facadecost's held window grows 3 blocks a command buffer on the host
    static let heldBlocksMost = 8.0

    func run(_: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        if RawFacade.encodeProfile(of: gpu.device).isEmpty {
            return RawFacade.counters(of: gpu.device)["state_segment_binds"] == nil
                ? .skip("no encode profile: a release runtime")
                : .fail("a debug runtime without switchRawEncodeProfile")
        }
        let library: MTLLibrary
        do {
            library = try gpu.device.makeLibrary(URL: CAState.library)
        } catch {
            return .skip("no QuartzCore library here (\(GPU.describe(error)))")
        }
        guard let line = CACorpus.state("PBGRAXm_A2Xhf") else { return .fail("CACorpus has no PBGRAXm_A2Xhf") }
        let pipeline = try CAState.pipeline(line, library: library, device: gpu.device)
        let target = try gpu.texture(.bgra8Unorm, Self.width, Self.height)
        let scratch = try gpu.texture(
            .bgra8Unorm, Self.width, Self.height, usage: [.renderTarget], storage: .memoryless)
        let encode = { (commands: MTLCommandBuffer, draws: Int) in
            for pass in 0..<Self.passes {
                let descriptor = MTLRenderPassDescriptor()
                descriptor.colorAttachments[0].texture = target
                descriptor.colorAttachments[0].loadAction = pass == 0 ? .clear : .load
                descriptor.colorAttachments[0].storeAction = .store
                descriptor.colorAttachments[1].texture = scratch
                descriptor.colorAttachments[1].loadAction = .clear
                descriptor.colorAttachments[1].storeAction = .dontCare
                let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor)!
                encoder.setRenderPipelineState(pipeline)
                for draw in 0..<draws {
                    let x = Float(draw % 8) / 4 - 1, y = Float(pass) / 3 - 1
                    let colour = SIMD4<Float16>(Float16(draw % 4) / 4, Float16(pass) / 8, 0.5, 1)
                    CAState.bind(
                        encoder, vertices: CAState.rectangle(x0: x, y0: y, x1: x + 0.25, y1: y + 0.3, color: colour),
                        source: nil)
                    encoder.setScissorRect(MTLScissorRect(x: (draw % 8) * 32, y: pass * 32, width: 32, height: 64))
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                encoder.endEncoding()
            }
        }
        var warm = 0
        let warmup = { (commands: MTLCommandBuffer) in encode(commands, Self.drawCounts[0]) }
        while warm < Self.warmups, try await gpu.run(tolerateError: true, warmup).path != "raw" {
            warm += 1
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        var metrics = Metrics()
        metrics.set("warm", warm)
        var arms: [[String: Double]] = []
        for draws in Self.drawCounts {
            try gpu.finish((0..<Self.buffers).map { _ in try gpu.commit { encode($0, draws) } })
            let before = RawFacade.encodeProfile(of: gpu.device)
            let blocks = Self.blocksInUse()
            let held = try (0..<Self.buffers).map { _ in try autoreleasepool { try gpu.commit { encode($0, draws) } } }
            let heldBlocks = (Self.blocksInUse() - blocks) / Double(Self.buffers)
            try gpu.finish(held)
            var measured = Self.perCommandBuffer(before, RawFacade.encodeProfile(of: gpu.device))
            measured["held_blocks"] = heldBlocks
            arms.append(measured)
            let arm = arms.last!
            let suffix = "_d\(draws)"
            metrics.set("encodes" + suffix, Int(arm["encodes"] ?? 0))
            metrics.set("wall_us" + suffix, arm["wall_us"] ?? 0, digits: 0)
            metrics.set("walk_us" + suffix, arm["loop_self_us"] ?? 0, digits: 0)
            for part in Self.parts { metrics.set("us_" + part + suffix, arm[part + "_us"] ?? 0, digits: 0) }
            for event in Self.events { metrics.set(event + suffix, arm[event] ?? 0, digits: 1) }
            let drawn = arm["draw_calls"] ?? 0
            metrics.set("applied_per_draw" + suffix, drawn > 0 ? (arm["applied"] ?? 0) / drawn : 0, digits: 2)
            metrics.set("mallocs_counted" + suffix, arm["malloc_encodes"] ?? 0 > 0)
        }
        let process = RawFacade.encodeProfile(of: gpu.device)
        for kind in ["render", "blit", "compute"] {
            for count in ["made", "freed"] {
                let key = kind + "_encoders_" + count
                metrics.set(key, Int(process[key] ?? 0))
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        var failures: [String] = []
        if gpu.paths.paths.keys.contains(where: { $0 != "raw" }) {
            failures.append("command buffers went \(gpu.paths.text)")
        }
        if arms.contains(where: { ($0["encodes"] ?? 0) < 1 }) {
            failures.append("an arm encoded nothing on the raw path")
        }
        for (event, most) in Self.ceilings {
            metrics.set(event + "_ceiling", most, digits: 1)
            for (draws, arm) in zip(Self.drawCounts, arms) where (arm[event] ?? 0) > most {
                failures.append("\(event) per command buffer \(arm[event] ?? 0) at \(draws) draws, over \(most)")
            }
        }
        for event in Self.perDrawFree {
            let counts = arms.map { $0[event] ?? 0 }
            if abs(counts[1] - counts[0]) >= 0.5 {
                failures.append(
                    "\(event) per command buffer grew with its draws: \(Self.drawCounts[0]) draws \(counts[0]), "
                        + "\(Self.drawCounts[1]) draws \(counts[1])")
            }
        }
        return failures.isEmpty
            ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics, proof: gpu.proof)
    }

    static func blocksInUse() -> Double {
        var stats = malloc_statistics_t()
        malloc_zone_statistics(nil, &stats)
        return Double(stats.blocks_in_use)
    }

    /// The profile's change between two reads, per encode (the counters' and the times'; mallocs and frees over the
    /// encodes that counted them)
    static func perCommandBuffer(_ before: [String: Double], _ after: [String: Double]) -> [String: Double] {
        let encodes = (after["encodes"] ?? 0) - (before["encodes"] ?? 0)
        let mallocEncodes = (after["malloc_encodes"] ?? 0) - (before["malloc_encodes"] ?? 0)
        var out: [String: Double] = ["encodes": encodes, "malloc_encodes": mallocEncodes]
        guard encodes > 0 else { return out }
        for (key, value) in after where key != "encodes" && key != "malloc_encodes" {
            let delta = value - (before[key] ?? 0)
            let per = key == "mallocs" || key == "frees" ? max(mallocEncodes, 1) : encodes
            out[key] = delta / per
        }
        return out
    }
}
