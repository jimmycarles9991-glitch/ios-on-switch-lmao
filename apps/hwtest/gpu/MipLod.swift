import Foundation
import HWTestCore
import Metal

/// gpu.raw.mip_lod's draws (shaders/MipLod.metal): a mipmapped texture drawn over a whole target, minified by a uniform
/// ratio, so every pixel's level of detail is Metal's log2 of the texels a pixel spans, plus the bias, within the
/// sampler's clamps.
///
/// Levels: a 256x256 RGBA8 chain whose level k is one colour (red 60k, capped at 255), drawn at 0.4 to 1.0 of its size
/// with a nearest and a linear mip filter, a shader bias, the sampler's level clamps, no mipmapping and a view of levels
/// 1 to 3; the red the target reads against the level, or the two levels' blend, Metal's formula gives
/// (`lod_<arm>=<got>/<want>`, wrong beyond `levelTolerance` codes: a twentieth of a level).
///
/// Icon: a 128x128 white rounded rectangle (corner radius 30, coverage-antialiased, premultiplied) whose levels
/// generateMipmaps makes, drawn at 29 to 100 pixels with a trilinear sampler against the CPU's reference of Metal's
/// result (MipReference's levels, bilinear at 1/256 and the levels blended by the fraction of the level of detail):
/// `icon_<side>_beyond` counts the codes beyond `iconTolerance`, `icon_<side>_max_diff` the largest difference.
public enum MipLod {
    public static let levelTolerance = 3
    public static let iconTolerance = 3

    struct Arm {
        let name: String
        let side: Int
        var mip: MTLSamplerMipFilter = .linear
        var bias: Float = 0
        var minClamp: Float = 0
        var maxClamp: Float = .greatestFiniteMagnitude
        var view = false
    }

    static let levelSide = 256

    static let arms: [Arm] =
        [256, 230, 205, 190, 170, 154, 128, 115, 102].flatMap { side in
            [Arm(name: "n\(side)", side: side, mip: .nearest), Arm(name: "l\(side)", side: side)]
        } + [
            Arm(name: "b_m05", side: 154, bias: -0.5), Arm(name: "b_p05", side: 154, bias: 0.5),
            Arm(name: "b_p1", side: 154, bias: 1), Arm(name: "minclamp", side: 256, minClamp: 1.5),
            Arm(name: "maxclamp", side: 102, maxClamp: 0.25), Arm(name: "notmip", side: 102, mip: .notMipmapped),
            Arm(name: "view", side: 64, view: true),
        ]

    static func levelRed(_ level: Int) -> Double { Double(min(255, 60 * level)) }

    /// Metal's red for an arm: the level of detail log2(texels a pixel spans) plus the bias, clamped, then one level
    /// (nearest: rounded; none: the base) or two blended by its fraction
    static func wantRed(_ a: Arm) -> Double {
        let base = a.view ? 1 : 0, size = Double(levelSide >> base)
        let last = Double((a.view ? 3 : 8) - base)
        var lod = log2(size / Double(a.side)) + Double(a.bias)
        lod = min(max(lod, Double(a.minClamp)), Double(a.maxClamp))
        lod = min(max(lod, 0), last)
        switch a.mip {
        case .notMipmapped: return levelRed(base)
        case .nearest: return levelRed(base + Int((lod + 0.5).rounded(.down)))
        default:
            let l = Int(lod.rounded(.down)), f = lod - Double(l)
            return levelRed(base + l) * (1 - f) + levelRed(base + min(l + 1, Int(last))) * f
        }
    }

    static func sampler(_ device: MTLDevice, _ a: Arm) throws -> MTLSamplerState {
        let d = MTLSamplerDescriptor()
        d.minFilter = .linear
        d.magFilter = .linear
        d.mipFilter = a.mip
        d.sAddressMode = .clampToEdge
        d.tAddressMode = .clampToEdge
        d.lodMinClamp = a.minClamp
        d.lodMaxClamp = a.maxClamp
        guard let s = device.makeSamplerState(descriptor: d) else { throw TestFailure("makeSamplerState returned nil") }
        return s
    }

    /// `texture` drawn over a side x side BGRA8 target through `sampler` (with `bias` through the bias function): its
    /// texels and the command buffer's path
    static func draw(
        _ gpu: GPU, _ texture: MTLTexture, _ sampler: MTLSamplerState, side: Int, bias: Float?
    ) async throws -> ([UInt8], String) {
        let target = try gpu.texture(.bgra8Unorm, side, side, usage: [.renderTarget, .shaderRead])
        let state = try gpu.render(
            vertex: "textured_vertex", fragment: bias == nil ? "mip_lod_fragment" : "mip_lod_bias_fragment",
            format: .bgra8Unorm)
        let path = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .dontCare
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            var uvs: [SIMD2<Float>] = [SIMD2(0, 1), SIMD2(1, 1), SIMD2(0, 0), SIMD2(1, 0)]
            encoder.setVertexBytes(&corners, length: 64, index: 0)
            encoder.setVertexBytes(&uvs, length: 32, index: 1)
            encoder.setFragmentTexture(texture, index: 0)
            encoder.setFragmentSamplerState(sampler, index: 0)
            if var b = bias { encoder.setFragmentBytes(&b, length: 4, index: 0) }
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }.path
        return (try await gpu.read(target, bytesPerPixel: 4), path)
    }

    static func levelChain(_ gpu: GPU) async throws -> MTLTexture {
        let levels = Int(log2(Double(levelSide))) + 1
        let chain = try gpu.texture(.rgba8Unorm, levelSide, levelSide, usage: [.shaderRead], mipmaps: levels)
        for level in 0..<levels {
            let side = max(1, levelSide >> level), red = UInt8(levelRed(level))
            let texel: [UInt8] = [red, 255 - red, 128, 255]
            try await gpu.upload(
                [UInt8]((0..<side * side).flatMap { _ in texel }), to: chain, bytesPerRow: side * 4, level: level)
        }
        return chain
    }

    /// Every level arm's line field and the arms beyond the tolerance
    public static func levels(_ gpu: GPU) async throws -> (fields: [String], wrong: [String], paths: Set<String>) {
        let chain = try await levelChain(gpu)
        guard
            let view = chain.makeTextureView(
                pixelFormat: .rgba8Unorm, textureType: .type2D, levels: 1..<4, slices: 0..<1)
        else { throw TestFailure("makeTextureView returned nil") }
        var fields: [String] = [], wrong: [String] = []
        var paths: Set<String> = []
        for a in arms {
            let (texels, path) = try await draw(
                gpu, a.view ? view : chain, try sampler(gpu.device, a), side: a.side, bias: a.bias == 0 ? nil : a.bias)
            paths.insert(path)
            let centre = (a.side / 2 * a.side + a.side / 2) * 4
            let got = Int(texels[centre + 2]), want = wantRed(a)
            var spread = 0
            for t in 0..<a.side * a.side { spread = max(spread, abs(Int(texels[t * 4 + 2]) - got)) }
            fields.append("\(a.name)=\(got)/\(Int(want.rounded()))")
            if abs(Double(got) - want) > Double(levelTolerance) || spread > 1 { wrong.append(a.name) }
        }
        return (fields, wrong, paths)
    }
}
