import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Opaque border colours on the raw path (boot 21 gpu.heap.m2v_target: such a sampler was m2v's, call.sampler "border
/// 2"): a 4x4 texture drawn into the middle of a 12x12 target through a clamp-to-border sampler, its ring sampled past
/// the edge. Metal reads the colour in the format's channels, a missing red, green or blue 0 and alpha 1, A8Unorm's
/// alpha the colour's red, then the view's swizzle (m2v metal/sampler.rs border_texel, measured). RGBA8 reads the
/// sampler's own header; R8, A8 and a swizzled view read the headers made for them, and the A8 and swizzled arms tell
/// whether the texture unit returns the header's border words as they are (raw_sampler.h borderTexel's assumption).
struct OpaqueBorderTest: HWTest {
    let id = "gpu.raw.opaque_border"
    let name = "Opaque border colours read Metal's texel of each texture format and swizzle, on the raw path"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let texel: [UInt8] = [200, 100, 50, 150]

    struct Arm {
        let label: String
        let texture: MTLTexture
        let inside: [UInt8]
        let white: [UInt8]
        let black: [UInt8]
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let rgba = try gpu.texture(.rgba8Unorm, 4, 4, usage: [.shaderRead, .pixelFormatView])
        try await gpu.upload(Array([[UInt8]](repeating: Self.texel, count: 16).joined()), to: rgba, bytesPerRow: 16)
        let red = try gpu.texture(.r8Unorm, 4, 4, usage: [.shaderRead])
        try await gpu.upload([UInt8](repeating: Self.texel[0], count: 16), to: red, bytesPerRow: 4)
        let alpha = try gpu.texture(.a8Unorm, 4, 4, usage: [.shaderRead])
        try await gpu.upload([UInt8](repeating: Self.texel[3], count: 16), to: alpha, bytesPerRow: 4)
        let swizzle = MTLTextureSwizzleChannels(red: .alpha, green: .blue, blue: .green, alpha: .red)
        guard
            let swizzled = rgba.makeTextureView(
                pixelFormat: .rgba8Unorm, textureType: .type2D, levels: 0..<1, slices: 0..<1, swizzle: swizzle)
        else { throw TestFailure("no swizzled view") }
        let t = Self.texel
        let arms = [
            Arm(label: "rgba8", texture: rgba, inside: t, white: [255, 255, 255, 255], black: [0, 0, 0, 255]),
            Arm(
                label: "r8", texture: red, inside: [t[0], 0, 0, 255], white: [255, 0, 0, 255], black: [0, 0, 0, 255]),
            Arm(label: "a8", texture: alpha, inside: [0, 0, 0, t[3]], white: [0, 0, 0, 255], black: [0, 0, 0, 0]),
            Arm(
                label: "abgr_view", texture: swizzled, inside: [t[3], t[2], t[1], t[0]], white: [255, 255, 255, 255],
                black: [255, 0, 0, 0]),
        ]
        let state = try gpu.render(fragment: "opaque_border_fragment", format: .rgba8Unorm)
        var metrics = Metrics()
        var wrong: [String] = []
        var paths: [String] = []
        for (colour, border) in [("white", MTLSamplerBorderColor.opaqueWhite), ("black", .opaqueBlack)] {
            let descriptor = MTLSamplerDescriptor()
            descriptor.sAddressMode = .clampToBorderColor
            descriptor.tAddressMode = .clampToBorderColor
            descriptor.borderColor = border
            guard let sampler = gpu.device.makeSamplerState(descriptor: descriptor) else {
                throw TestFailure("no \(colour) border sampler")
            }
            let targets = try arms.map { _ in try gpu.texture(.rgba8Unorm, 12, 12) }
            paths.append(
                try await gpu.run { commands in
                    for (arm, target) in zip(arms, targets) {
                        let pass = MTLRenderPassDescriptor()
                        pass.colorAttachments[0].texture = target
                        pass.colorAttachments[0].loadAction = .clear
                        pass.colorAttachments[0].storeAction = .store
                        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                        encoder.setRenderPipelineState(state)
                        var corners = GPU.fullScreen
                        encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                        encoder.setFragmentTexture(arm.texture, index: 0)
                        encoder.setFragmentSamplerState(sampler, index: 0)
                        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                        encoder.endEncoding()
                    }
                }.path)
            for (arm, target) in zip(arms, targets) {
                let bytes = try await gpu.read(target, bytesPerPixel: 4)
                let want = colour == "white" ? arm.white : arm.black
                var off = 0
                for y in 0..<12 {
                    for x in 0..<12 {
                        let inside = (4..<8).contains(x) && (4..<8).contains(y)
                        let at = 4 * (x + 12 * y)
                        off += Array(bytes[at..<at + 4]) == (inside ? arm.inside : want) ? 0 : 1
                    }
                }
                let label = "\(arm.label)_\(colour)"
                metrics.set("\(label)_wrong", off)
                metrics.set("\(label)_border", bytes[0..<4].map { String($0) }.joined(separator: "/"))
                if off > 0 { wrong.append(label) }
            }
        }
        metrics.set("paths", paths.joined(separator: ","))
        if paths.contains(where: { $0 != "raw" }) { wrong.append("paths") }
        return wrong.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(wrong.joined(separator: ","), metrics)
    }
}
