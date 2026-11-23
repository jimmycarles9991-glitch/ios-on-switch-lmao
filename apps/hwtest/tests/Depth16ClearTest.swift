import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Depth16Unorm clears: Metal stores code floor(f x 65535) of the clear depth as a float f, the product exact (an M4 Max,
/// macOS 27.2: 3,880 clears swept, 0.25 -> 0x3fff where rounding to nearest gives 0x4000). The driver hands the chip
/// the code's value plus a quarter code, so its clears land on Metal's code whatever the chip's float-to-unorm rounding;
/// a sweep of codes and of values between codes is read back by a blit, on the driver.
struct Depth16ClearTest: HWTest {
    let id = "gpu.gm20b.depth16_clear"
    let name = "Depth16Unorm clears store Metal's code, on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    static func metalCode(_ d: Double) -> UInt16 {
        UInt16((Double(Float(min(max(d, 0), 1))) * 65535).rounded(.down))
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var clears: [Double] = [0.25, 0.5, 0.75, 1.0, 0, 0.0014801251239795529, 0.0029602502479591057]
        for c in stride(from: 1, to: 65535, by: 4093) {
            for frac in [0.0, 0.25, 0.5, 0.75] { clears.append((Double(c) + frac) / 65535) }
        }
        let depth = try gpu.texture(.depth16Unorm, 4, 4, usage: [.renderTarget])
        let out = try gpu.buffer(clears.count * 2)
        var paths = Set<String>()
        for (i, clear) in clears.enumerated() {
            let timing = try await gpu.run { commands in
                let p = MTLRenderPassDescriptor()
                p.depthAttachment.texture = depth
                p.depthAttachment.loadAction = .clear
                p.depthAttachment.clearDepth = clear
                p.depthAttachment.storeAction = .store
                commands.makeRenderCommandEncoder(descriptor: p)!.endEncoding()
                let b = commands.makeBlitCommandEncoder()!
                b.copy(
                    from: depth, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 1, y: 1, z: 0),
                    sourceSize: MTLSize(width: 1, height: 1, depth: 1), to: out, destinationOffset: 2 * i,
                    destinationBytesPerRow: 2, destinationBytesPerImage: 2
                )
                b.endEncoding()
            }
            paths.insert(timing.path)
        }
        var wrong: [String] = []
        var up = 0, down = 0
        for (i, clear) in clears.enumerated() {
            let got = out.contents().load(fromByteOffset: 2 * i, as: UInt16.self), want = Self.metalCode(clear)
            if got != want {
                if got > want { up += 1 } else { down += 1 }
                if wrong.count < 6 { wrong.append(String(format: "%.6f:%04x/%04x", clear, got, want)) }
            }
        }
        var metrics = Metrics()
        metrics.set("paths", paths.sorted().joined(separator: ","))
        metrics.set("clears", clears.count)
        metrics.set("quarter", String(format: "%04x", out.contents().load(as: UInt16.self)))
        metrics.set("wrong_up", up)
        metrics.set("wrong_down", down)
        metrics.set("first_wrong", wrong.isEmpty ? "-" : wrong.joined(separator: ","))
        var failures: [String] = []
        if paths != ["gm20b"] { failures.append("a clear went off the driver") }
        if up + down != 0 { failures.append("\(up + down) of \(clears.count) clears not Metal's code") }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }
}
