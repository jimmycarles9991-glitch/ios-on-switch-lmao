import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The ROP's zero-source pixel kill (drivers/TegraGM20BMetal/raw/runtime/raw_blend_kill.h): a source-over state draws
/// a zero colour over RGBA16F holding -0, NaN, infinities and denormals and over BGRA8, and no bit changes; the same
/// state still blends a non-zero colour as source-over says; the facade counts the state made with the kill and its
/// binds; and the same draws with the kill forced off through the probe channel give the GPU time it saves. The zeta
/// arms draw zero-source pixels of a source-over state that writes depth (always, 0.5) or stencil (replace with 0x5a,
/// always) over a zeta cleared to 1.0 or 0: Metal writes the zeta at every pixel and keeps the colour, kill or not
/// (zeta_<depth|stencil>_wrong= texels other than Metal's, zeta_<...>_colour_changed_bytes=, zeta_<...>_kill_binds= the
/// binds the facade made with the kill on).
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
        for stencil in [false, true] {
            let label = stencil ? "zeta_stencil" : "zeta_depth"
            let binds = RawFacade.counters(of: gpu.device)["blend_kill_binds"] ?? 0
            let arm = try await Self.zetaArm(gpu, stencil: stencil)
            metrics.set("\(label)_wrong", arm.wrong)
            metrics.set("\(label)_colour_changed_bytes", arm.colourChanged)
            metrics.set("\(label)_kill_binds", (RawFacade.counters(of: gpu.device)["blend_kill_binds"] ?? 0) - binds)
            paths.append(contentsOf: arm.paths)
            if arm.wrong != 0 { wrong.append("\(label)_wrong") }
            if arm.colourChanged != 0 { wrong.append("\(label)_colour_changed_bytes") }
        }
        metrics.set("paths", paths.filter { $0 != "raw" }.isEmpty ? "raw" : paths.joined(separator: ","))
        if paths.contains(where: { $0 != "raw" }) { wrong.append("paths") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    static let zetaSide = 128
    static let zetaClear: [Double] = [0.2, 0.4, 0.6, 0.6]

    /// One pass of zero-source draws writing depth or stencil: the zeta texels other than Metal's, the colour bytes
    /// changed from the clear's, and the paths drawn
    static func zetaArm(_ gpu: GPU, stencil: Bool) async throws -> (wrong: Int, colourChanged: Int, paths: [String]) {
        let side = zetaSide
        let zetaFormat: MTLPixelFormat = stencil ? .stencil8 : .depth32Float
        let state = try gpu.render(
            fragment: "exact_fragment", format: .bgra8Unorm, depth: stencil ? .invalid : zetaFormat,
            stencil: stencil ? zetaFormat : .invalid, configure: sourceOver)
        let zetaState = MTLDepthStencilDescriptor()
        zetaState.depthCompareFunction = .always
        zetaState.isDepthWriteEnabled = !stencil
        if stencil {
            let face = MTLStencilDescriptor()
            face.stencilCompareFunction = .always
            face.depthStencilPassOperation = .replace
            face.writeMask = 0xff
            zetaState.frontFaceStencil = face
            zetaState.backFaceStencil = face
        }
        guard let zetaTest = gpu.device.makeDepthStencilState(descriptor: zetaState) else {
            throw TestFailure("makeDepthStencilState returned nil")
        }
        let colour = try gpu.texture(.bgra8Unorm, side, side, usage: [.renderTarget, .shaderRead])
        let zeta = try gpu.texture(zetaFormat, side, side, usage: [.renderTarget])
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = colour
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(
            red: zetaClear[0], green: zetaClear[1], blue: zetaClear[2], alpha: zetaClear[3])
        pass.colorAttachments[0].storeAction = .store
        if stencil {
            pass.stencilAttachment.texture = zeta
            pass.stencilAttachment.loadAction = .clear
            pass.stencilAttachment.clearStencil = 0
            pass.stencilAttachment.storeAction = .store
        } else {
            pass.depthAttachment.texture = zeta
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = 1
            pass.depthAttachment.storeAction = .store
        }
        let drawn = try await gpu.run { commands in
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            encoder.setDepthStencilState(zetaTest)
            encoder.setStencilReferenceValue(0x5a)
            var corners = GPU.fullScreen
            var value = SIMD4<Float>.zero
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentBytes(&value, length: 16, index: 0)
            for _ in 0..<4 { encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4) }
            encoder.endEncoding()
        }
        let texels = try await gpu.read(zeta, bytesPerPixel: stencil ? 1 : 4)
        var wrong = 0
        if stencil {
            wrong = texels.filter { $0 != 0x5a }.count
        } else {
            texels.withUnsafeBytes { raw in
                for depth in raw.bindMemory(to: Float.self) where depth != 0.5 { wrong += 1 }
            }
        }
        let cleared = [UInt8](
            (0..<side * side).flatMap { _ in
                [2, 1, 0, 3].map { UInt8((zetaClear[$0] * 255).rounded()) }
            })
        let bytes = try await gpu.read(colour, bytesPerPixel: 4)
        let changed = zip(bytes, cleared).filter { $0 != $1 }.count
        return (wrong, changed, [drawn.path])
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
