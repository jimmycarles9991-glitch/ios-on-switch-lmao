import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The ROP's zero-source pixel kill (drivers/TegraGM20BMetal/raw/runtime/raw_blend_kill.h): a source-over state draws
/// a zero colour over RGBA16F holding -0, NaN, infinities and denormals and over BGRA8, and no bit changes; the same
/// state still blends a non-zero colour as source-over says; the facade counts the state made with the kill and its
/// binds; and the same draws with the kill forced off through the probe channel give the GPU time it saves.
struct BlendKillTest: HWTest {
    let id = "gpu.raw.blend_kill"
    let name = "Zero-source pixels are dropped by the ROP, bits kept, blending unchanged"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let size = 512
    static let draws = 32
    static let rounds = 4
    static let specials: [UInt16] = [
        0x8000, 0x7e00, 0xfe00, 0x7c00, 0xfc00, 0x0001, 0x8001, 0x03ff, 0x3c00, 0xbc00, 0x7bff, 0x0000, 0x3555, 0x0400,
        0x8400, 0x7d00,
    ]

    static func sourceOver(_ descriptor: MTLRenderPipelineDescriptor) {
        let attachment = descriptor.colorAttachments[0]!
        attachment.isBlendingEnabled = true
        attachment.sourceRGBBlendFactor = .one
        attachment.sourceAlphaBlendFactor = .one
        attachment.destinationRGBBlendFactor = .oneMinusSourceAlpha
        attachment.destinationAlphaBlendFactor = .oneMinusSourceAlpha
    }

    static func pass(_ target: MTLTexture, load: MTLLoadAction) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = load
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 0.6)
        pass.colorAttachments[0].storeAction = .store
        return pass
    }

    static func draw(
        _ state: MTLRenderPipelineState, _ target: MTLTexture, colour: SIMD4<Float>, count: Int, load: MTLLoadAction
    ) -> (MTLCommandBuffer) -> Void {
        { commands in
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass(target, load: load))!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            var value = colour
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentBytes(&value, length: 16, index: 0)
            for _ in 0..<count { encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4) }
            encoder.endEncoding()
        }
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        var metrics = Metrics()
        var wrong: [String] = []
        let before = RawFacade.counters(of: gpu.device)
        var paths: [String] = []
        var onMs = 0.0
        var offMs = 0.0
        var emitted = 0
        for half in [true, false] {
            let label = half ? "f16" : "unorm"
            let format: MTLPixelFormat = half ? .rgba16Float : .bgra8Unorm
            let count = Self.size * Self.size * 4
            let initial: [UInt8] =
                half
                ? (0..<count).flatMap { i -> [UInt8] in
                    let h = Self.specials[i % Self.specials.count]
                    return [UInt8(h & 0xff), UInt8(h >> 8)]
                } : (0..<count).map { UInt8(($0 * 53 + 7) & 255) }
            let target = try gpu.texture(format, Self.size, Self.size, usage: [.renderTarget, .shaderRead])
            try await gpu.upload(initial, to: target, bytesPerRow: initial.count / Self.size)
            let state = try gpu.render(fragment: "exact_fragment", format: format, configure: Self.sourceOver)
            let zero = Self.draw(state, target, colour: .zero, count: Self.draws, load: .load)
            for _ in 0..<Self.rounds {
                let on = try await gpu.run(zero)
                paths.append(on.path)
                onMs += on.gpuMilliseconds
            }
            let killed = try await gpu.read(target, bytesPerPixel: half ? 8 : 4)
            let changed = zip(killed, initial).filter { $0 != $1 }.count
            metrics.set("\(label)_changed_bytes", changed)
            if changed != 0 { wrong.append("\(label)_changed_bytes") }
            for _ in 0..<Self.rounds {
                let off = try await RawMethods.draw(gpu, before: [(0x02f4, 0)], after: [(0x02f4, 1)], zero)
                paths.append(off.path)
                offMs += off.gpuMilliseconds
                emitted += off.emitted
            }
            let blended = try await gpu.read(target, bytesPerPixel: half ? 8 : 4)
            metrics.set("\(label)_off_changed_bytes", zip(blended, killed).filter { $0 != $1 }.count)
            let control = try gpu.texture(format, 64, 64, usage: [.renderTarget, .shaderRead])
            paths.append(
                try await gpu.run(
                    Self.draw(state, control, colour: SIMD4(0.5, 0.25, 0.125, 0.5), count: 1, load: .clear)
                ).path)
            let worst = try await Self.controlError(gpu, control, half: half)
            metrics.set("\(label)_control_max_diff", worst, digits: 4)
            if worst > (half ? 1.0 / 1024 : 1.0) { wrong.append("\(label)_control") }
        }
        let counters = RawFacade.counters(of: gpu.device)
        let states = (counters["blend_kill_states"] ?? 0) - (before["blend_kill_states"] ?? 0)
        let binds = (counters["blend_kill_binds"] ?? 0) - (before["blend_kill_binds"] ?? 0)
        metrics.set("kill_states", states)
        metrics.set("kill_binds", binds)
        if binds == 0 { wrong.append("kill_binds") }
        metrics.set("on_gpu_ms", onMs, digits: 3)
        metrics.set("off_gpu_ms", offMs, digits: 3)
        metrics.set("off_emitted", emitted)
        metrics.set("paths", paths.filter { $0 != "raw" }.isEmpty ? "raw" : paths.joined(separator: ","))
        if paths.contains(where: { $0 != "raw" }) { wrong.append("paths") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    /// The largest difference from source-over of (0.5, 0.25, 0.125, 0.5) on the clear colour: codes for BGRA8, values
    /// for RGBA16F
    static func controlError(_ gpu: GPU, _ texture: MTLTexture, half: Bool) async throws -> Double {
        let clear: [Double] = [0.2, 0.4, 0.6, 0.6]
        let source: [Double] = [0.5, 0.25, 0.125, 0.5]
        let bytes = try await gpu.read(texture, bytesPerPixel: half ? 8 : 4)
        var worst = 0.0
        for texel in 0..<(texture.width * texture.height) {
            for c in 0..<4 {
                if half {
                    let bits = UInt16(bytes[texel * 8 + 2 * c]) | UInt16(bytes[texel * 8 + 2 * c + 1]) << 8
                    let got = Double(Float(Float16(bitPattern: bits)))
                    let d = Double(Float(Float16(Float(clear[c]))))
                    worst = max(worst, abs(got - (source[c] + d * 0.5)))
                } else {
                    let stored = [2, 1, 0, 3][c]
                    let d = (clear[c] * 255).rounded() / 255
                    let want = (source[c] + d * 0.5) * 255
                    worst = max(worst, abs(Double(bytes[texel * 4 + stored]) - want))
                }
            }
        }
        return worst
    }
}
