import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// The raw encode's CPU cost of a command buffer shaped as Core Animation's (encode_profile.h, debug builds):
/// `passes` passes of Core Animation's untextured fill (PBGRAXm_A2Xhf, BGRA8 beside its memoryless colour 1 in the
/// line's format), each draw binding its vertices, transform and zero uniforms by setBytes with a scissor of its own,
/// `buffers` command buffers at each of two draw counts, each after as many at that count (whose recording storage they
/// reuse, as a steady frame does). Per command buffer at each count: the encode's wall time and its parts (us_<part>,
/// and walk_us the loop's time outside every part), its events (mallocs, frees, locks, sends, uses, use_adds,
/// descriptor builds and uploads, geometry vertices) and bindings applied per draw. A command buffer's mallocs, frees,
/// locks, Objective-C sends, job uses that reached a job's list, vertices the CPU read and set-0 uploads must not grow
/// with its draws, none may pass what the facade's cost harness counted of the same command buffers over the mock GPU
/// on a Mac with the encode and its retire lock-free (at most the trim's lock once in 64 command buffers, no send, 4
/// allocations and 2 frees; the harness ran over the m2v device and went with it), and every command buffer is the raw
/// path's. Times are reported, not judged: a fragment-slot-0 bind reads no selector id, a state with a selector table
/// reads it once at a draw whose bindings moved (us_bind, us_draw). Beside the encode's own: the runtime's locks on
/// every thread over the batch (facade_locks, lock.h's count of its wrappers), at most 4 a command buffer, and the
/// growth of the blocks in use in every malloc zone while the batch is recorded and held (held_blocks), a coarse bound
/// on what each keeps allocated (the whole process's, so no per-draw check). The pooled encoders' blocks
/// (object_pool.h): none freed in the batch (its pool kept too few for what the batch holds at once), and the most
/// each pool made in the process. The profile is the facade's; a release runtime has none and the test skips. A view of
/// the target taken at its creation keeps it plain where it was made plain for want of compressed memory
/// (private_comp.h): its move onto that memory, an encode ahead given back and a copy job inside a range's encode,
/// never lands in a measured batch.
struct EncodeCostPathTest: HWTest {
    let id = "gpu.path.encode_cost"
    let name = "CA-shaped command buffer encode by part, with no alloc, lock, send or vertex read per draw"
    var subsystem: Subsystem { .metal }
    static let width = 256, height = 256, passes = 6, drawCounts = [8, 16], buffers = 24, warmups = 20

    static let parts = [
        "resources", "loop", "state", "apply", "pass", "pass_begin", "fusion", "pass_end", "pass_prologue", "draw",
        "prepare", "geometry", "variant_lookup", "variant", "descriptors", "textures", "bind", "use", "uniforms",
        "tail",
        "commit",
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
    /// coarse: the same harness's held window grew 3 blocks a command buffer on the host
    static let heldBlocksMost = 8.0

    func run(_: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else {
            return .skip("the default device is not the facade (\(GM20BFacade.kind(of: gpu.device)))")
        }
        if GM20BFacade.encodeProfile(of: gpu.device).isEmpty {
            return .skip("no encode profile (build with --//bazel:prof_gpu)")
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
        let keptPlain = target.makeTextureView(pixelFormat: .bgra8Unorm)
        let scratch = try gpu.texture(
            CACorpus.colorFormat(line, 1), Self.width, Self.height, usage: [.renderTarget], storage: .memoryless
        )
        let encode = { (commands: MTLCommandBuffer, draws: Int) in
            Self.encodeBatch(commands, draws: draws, pipeline: pipeline, target: target, scratch: scratch)
        }
        var warm = 0
        let warmup = { (commands: MTLCommandBuffer) in encode(commands, Self.drawCounts[0]) }
        while warm < Self.warmups, try await gpu.run(tolerateError: true, warmup).path != "gm20b" {
            warm += 1
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        // every count's batch once before any is measured: a process that just began (boot 22's, after a crash) pays its
        // pools' first growth inside the first arm otherwise
        for draws in Self.drawCounts {
            try gpu.finish((0 ..< Self.buffers).map { _ in try gpu.commit { encode($0, draws) } })
        }
        var metrics = Metrics()
        metrics.set("warm", warm)
        let arms = try Self.measureArms(gpu: gpu, encode: encode, metrics: &metrics)
        let failures = Self.judge(arms: arms, gpu: gpu, metrics: &metrics)
        withExtendedLifetime(keptPlain) { _ = keptPlain }
        let judged = Set(Self.ceilings.map(\.event) + Self.perDrawFree)
        metrics.fitLog(within: 720, keeping: Set(Self.drawCounts.flatMap { d in judged.map { $0 + "_d\(d)" } }))
        return failures.isEmpty
            ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics, proof: gpu.proof)
    }

    private static func encodeBatch(
        _ commands: MTLCommandBuffer, draws: Int, pipeline: MTLRenderPipelineState, target: MTLTexture,
        scratch: MTLTexture
    ) {
        for pass in 0 ..< Self.passes {
            let descriptor = MTLRenderPassDescriptor()
            descriptor.colorAttachments[0].texture = target
            descriptor.colorAttachments[0].loadAction = pass == 0 ? .clear : .load
            descriptor.colorAttachments[0].storeAction = .store
            descriptor.colorAttachments[1].texture = scratch
            descriptor.colorAttachments[1].loadAction = .clear
            descriptor.colorAttachments[1].storeAction = .dontCare
            let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor)!
            encoder.setRenderPipelineState(pipeline)
            for draw in 0 ..< draws {
                let x = Float(draw % 8) / 4 - 1, y = Float(pass) / 3 - 1
                let colour = SIMD4<Float16>(Float16(draw % 4) / 4, Float16(pass) / 8, 0.5, 1)
                CAState.bind(
                    encoder, vertices: CAState.rectangle(x0: x, y0: y, x1: x + 0.25, y1: y + 0.3, color: colour),
                    source: nil
                )
                encoder.setScissorRect(MTLScissorRect(x: (draw % 8) * 32, y: pass * 32, width: 32, height: 64))
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            encoder.endEncoding()
        }
    }

    private static func measureArms(
        gpu: GPU, encode: (MTLCommandBuffer, Int) -> Void, metrics: inout Metrics
    ) throws -> [[String: Double]] {
        var arms: [[String: Double]] = []
        for draws in Self.drawCounts {
            try gpu.finish((0 ..< Self.buffers).map { _ in try gpu.commit { encode($0, draws) } })
            let before = GM20BFacade.encodeProfile(of: gpu.device)
            let blocks = Self.blocksInUse()
            let held = try (0 ..< Self.buffers)
                .map { _ in try autoreleasepool { try gpu.commit { encode($0, draws) } } }
            let heldBlocks = (Self.blocksInUse() - blocks) / Double(Self.buffers)
            try gpu.finish(held)
            var measured = Self.perCommandBuffer(before, GM20BFacade.encodeProfile(of: gpu.device))
            measured["held_blocks"] = heldBlocks
            arms.append(measured)
            let suffix = "_d\(draws)"
            metrics.set("encodes" + suffix, Int(measured["encodes"] ?? 0))
            metrics.set("wall_us" + suffix, measured["wall_us"] ?? 0, digits: 0)
            metrics.set("walk_us" + suffix, measured["loop_self_us"] ?? 0, digits: 0)
            for part in Self.parts { metrics.set("us_" + part + suffix, measured[part + "_us"] ?? 0, digits: 0) }
            for event in Self.events { metrics.set(event + suffix, measured[event] ?? 0, digits: 1) }
            let drawn = measured["draw_calls"] ?? 0
            metrics.set("applied_per_draw" + suffix, drawn > 0 ? (measured["applied"] ?? 0) / drawn : 0, digits: 2)
            metrics.set("mallocs_counted" + suffix, measured["malloc_encodes"] ?? 0 > 0)
        }
        return arms
    }

    private static func judge(arms: [[String: Double]], gpu: GPU, metrics: inout Metrics) -> [String] {
        let process = GM20BFacade.encodeProfile(of: gpu.device)
        for kind in ["render", "blit", "compute"] {
            for count in ["made", "freed"] {
                let key = kind + "_encoders_" + count
                metrics.set(key, Int(process[key] ?? 0))
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        var failures: [String] = []
        if gpu.paths.paths.keys.contains(where: { $0 != "gm20b" }) {
            failures.append("command buffers went \(gpu.paths.text)")
        }
        if arms.contains(where: { ($0["encodes"] ?? 0) < 1 }) {
            failures.append("an arm encoded nothing on the driver")
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
                        + "\(Self.drawCounts[1]) draws \(counts[1])"
                )
            }
        }
        return failures
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
