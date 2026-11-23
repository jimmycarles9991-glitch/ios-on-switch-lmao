import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A depth attachment cleared to NaN (pass.ds: the driver refused it to m2v, which refuses it too). Metal stores the quiet
/// NaN 0x7fc00000 in a Depth32Float and 0 in a Depth16Unorm, and clamps any other clear to [0, 1] (an M4 Max, macOS 27.2,
/// read back by a blit): each texture read back holds that, its passes on the driver.
struct NanClearDepthTest: HWTest {
    let id = "gpu.gm20b.nan_clear_depth"
    let name = "A depth clear of NaN or out of [0, 1] stores Metal's value, on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let n = 16
        var metrics = Metrics()
        var failures: [String] = []
        let cases: [(MTLPixelFormat, Double, UInt32, String)] = [
            (.depth32Float, .nan, 0x7fc0_0000, "f32_nan"), (.depth32Float, 2.0, 0x3f80_0000, "f32_two"),
            (.depth32Float, -1.0, 0, "f32_minus"), (.depth16Unorm, .nan, 0, "u16_nan"),
        ]
        for (format, clear, want, label) in cases {
            let texel = format == .depth16Unorm ? 2 : 4
            let depth = try gpu.texture(format, n, n, usage: [.renderTarget])
            let out = try gpu.buffer(n * n * texel)
            let timing = try await gpu.run { commands in
                let p = MTLRenderPassDescriptor()
                p.depthAttachment.texture = depth
                p.depthAttachment.loadAction = .clear
                p.depthAttachment.clearDepth = clear
                p.depthAttachment.storeAction = .store
                commands.makeRenderCommandEncoder(descriptor: p)!.endEncoding()
                let b = commands.makeBlitCommandEncoder()!
                b.copy(
                    from: depth, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                    sourceSize: MTLSize(width: n, height: n, depth: 1), to: out, destinationOffset: 0,
                    destinationBytesPerRow: n * texel, destinationBytesPerImage: n * n * texel
                )
                b.endEncoding()
            }
            var wrong = 0
            var first: UInt32 = 0
            for i in 0 ..< (n * n) {
                let v: UInt32 =
                    texel == 2
                        ? UInt32(out.contents().load(fromByteOffset: 2 * i, as: UInt16.self))
                        : out.contents().load(fromByteOffset: 4 * i, as: UInt32.self)
                if i == 0 { first = v }
                wrong += v == want ? 0 : 1
            }
            metrics.set("\(label)_path", timing.path)
            metrics.set("\(label)_first", String(format: "%08x", first))
            metrics.set("\(label)_wrong", wrong)
            if timing.path != "gm20b" { failures.append("\(label) went \(timing.path)") }
            if wrong != 0 { failures.append("\(label): \(wrong) texels not \(String(format: "%08x", want))") }
        }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }
}
