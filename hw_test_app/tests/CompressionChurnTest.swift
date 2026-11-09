import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// GM20B colour and depth compression for memoryless targets follows what a pass needs now (boot 15: backboardd and
/// iconservicesagent made a compressed buffer for every size they drew, 494 of them, until the GPU pool refused 295 and
/// pushed coherent memory out to system pages; an idle process kept all of it). Passes of a memoryless RGBA16Float colour
/// and a memoryless Depth32Float_Stencil8 target in 24 sizes, sweep after sweep on the raw facade: compression is never
/// refused, one compressed buffer per attachment and kind holds every size, nothing grows from sweep to sweep, and with
/// nothing drawn for the idle time the process gives all its compressed memory back.
struct CompressionChurnTest: HWTest {
    let id = "gpu.compression_churn"
    let name = "Compressed memoryless targets are shared across sizes and freed when idle"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let sweeps = 6
    /// raw_comp.h kCompIdleNs, and the wake that frees what turned stale
    static let idleSeconds = 10.0 + 2.0

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw", !RawFacade.compression(of: gpu.device).isEmpty else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        var metrics = Metrics()
        var failures: [String] = []
        let sizes = (0..<24).map { (width: 720 - $0 * 27, height: 1280 - $0 * 50) }
        var targets: [(colour: MTLTexture, zeta: MTLTexture)] = []
        for size in sizes {
            let colour = try gpu.texture(
                .rgba16Float, size.width, size.height, usage: [.renderTarget], storage: .memoryless)
            let zeta = try gpu.texture(
                .depth32Float_stencil8, size.width, size.height, usage: [.renderTarget], storage: .memoryless)
            targets.append((colour: colour, zeta: zeta))
        }
        let start = RawFacade.compression(of: gpu.device)
        var paths: Set<String> = []
        func sweep(_ order: [Int]) async throws {
            for index in order {
                let target = targets[index]
                let timing = try await gpu.run { commands in
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = target.colour
                    pass.colorAttachments[0].loadAction = .clear
                    pass.colorAttachments[0].storeAction = .dontCare
                    pass.depthAttachment.texture = target.zeta
                    pass.depthAttachment.loadAction = .clear
                    pass.depthAttachment.storeAction = .dontCare
                    pass.stencilAttachment.texture = target.zeta
                    pass.stencilAttachment.loadAction = .clear
                    pass.stencilAttachment.storeAction = .dontCare
                    commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
                }
                paths.insert(timing.path)
            }
        }
        // the largest first, as a full-screen layer comes before the smaller ones
        try await sweep(Array(sizes.indices))
        for _ in 0..<100 where (RawFacade.compression(of: gpu.device)["made"] ?? 0) - (start["made"] ?? 0) < 2 {
            try await Task.sleep(nanoseconds: 20_000_000)
        }
        let warm = RawFacade.compression(of: gpu.device)
        var held: [Int] = []
        for round in 1..<Self.sweeps {
            try await sweep(sizes.indices.map { ($0 * 7 + round) % sizes.count })
            held.append(RawFacade.compression(of: gpu.device)["held_bytes"] ?? -1)
        }
        let swept = RawFacade.compression(of: gpu.device)
        try await Task.sleep(nanoseconds: UInt64(Self.idleSeconds * 1e9))
        let idle = RawFacade.compression(of: gpu.device)

        let made = (swept["made"] ?? 0) - (start["made"] ?? 0)
        let refused = (swept["refused"] ?? 0) - (start["refused"] ?? 0)
        metrics.set("paths", paths.sorted().joined(separator: ","))
        metrics.set("made", made)
        metrics.set("made_after_warmup", (swept["made"] ?? 0) - (warm["made"] ?? 0))
        metrics.set("refused", refused)
        metrics.set("held_kb", (swept["held_bytes"] ?? 0) >> 10)
        metrics.set("views", swept["views"] ?? 0)
        metrics.set("idle_held", idle["held"] ?? -1)
        metrics.set("idle_held_kb", (idle["held_bytes"] ?? 0) >> 10)
        metrics.set("idle_views", idle["views"] ?? -1)
        if paths != ["raw"] { failures.append("passes went \(paths.sorted().joined(separator: ","))") }
        if refused != 0 { failures.append("\(refused) compressed buffers refused") }
        if made == 0 { failures.append("no compressed buffer made for the targets") }
        if made > 2 { failures.append("\(made) compressed buffers for one colour and one zeta attachment, not 2") }
        if (swept["made"] ?? 0) != (warm["made"] ?? 0) {
            failures.append("sweeps after the first made \((swept["made"] ?? 0) - (warm["made"] ?? 0)) more")
        }
        if Set(held).count != 1 { failures.append("the compressed memory held moved between sweeps: \(held)") }
        if (idle["held"] ?? -1) != 0 || (idle["views"] ?? -1) != 0 {
            failures.append(
                "\(idle["held"] ?? -1) compressed buffers (\((idle["held_bytes"] ?? 0) >> 10) KiB) and "
                    + "\(idle["views"] ?? -1) backings still held \(Int(Self.idleSeconds)) s after the last pass")
        }
        return failures.isEmpty
            ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics, proof: gpu.proof)
    }
}
