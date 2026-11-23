import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The scenes of gpu.gm20b.same_bits (c38-audit m2v-pass-forms/probe/bitsprobe.mm), 16x16: a depth texture as colour 0
/// under a state of the colour format of the same bits, cleared to 0.25 and drawn 0.75 over the left half; an RGBA8Unorm
/// texture (private, or over a buffer) as the depth attachment beside an RGBA8Unorm colour, filled with 0x11 bytes,
/// cleared to 0.5 (or loaded) and drawn over the left half at z 0.25 under Less with depth writes.
enum SameBitsCases {
    static let side = 16
    static let factsSelector = NSSelectorFromString("switchMetalProbeFacts:")

    struct AsDepth {
        var depth: [UInt8] = []
        var colour: [UInt8] = []
        var blockLinear: Int?
    }

    static func leftHalf(z: Float) -> [SIMD4<Float>] {
        GPU.pixelQuad(0, 0, side / 2, side, width: side, height: side, z: z)
    }

    static func depthAsColour(
        _ gpu: GPU, _ format: MTLPixelFormat, state: MTLPixelFormat, bytes: Int
    ) async throws
        -> [UInt8]
    {
        let target = try gpu.texture(format, side, side)
        let pipeline = try gpu.render(fragment: "solid_fragment", format: state)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.25, green: 0.5, blue: 0.75, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(pipeline)
            GPU.draw(e, corners: leftHalf(z: 0.5), color: SIMD4(0.75, 0.5, 0.25, 1))
            e.endEncoding()
        }
        return try await gpu.read(target, bytesPerPixel: bytes)
    }

    static func colourAsDepth(_ gpu: GPU, pitch: Bool, load: Bool) async throws -> AsDepth {
        let rowBytes = 256
        let depth: MTLTexture
        if pitch {
            let d = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .rgba8Unorm, width: side, height: side, mipmapped: false
            )
            d.usage = [.renderTarget, .shaderRead]
            d.storageMode = .shared
            let backing = try gpu.buffer(rowBytes * side)
            guard let t = backing.makeTexture(descriptor: d, offset: 0, bytesPerRow: rowBytes) else {
                throw TestFailure("no RGBA8Unorm texture over a buffer")
            }
            depth = t
        } else {
            depth = try gpu.texture(.rgba8Unorm, side, side)
        }
        try await gpu.upload(
            [UInt8](repeating: 0x11, count: rowBytes * side), to: depth, bytesPerRow: pitch ? rowBytes : side * 4
        )
        let colour = try gpu.texture(.rgba8Unorm, side, side)
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm, depth: .depth32Float)
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .less
        d.isDepthWriteEnabled = true
        guard let less = gpu.device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no depth state") }
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = colour
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = depth
            pass.depthAttachment.loadAction = load ? .load : .clear
            pass.depthAttachment.clearDepth = 0.5
            pass.depthAttachment.storeAction = .store
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(pipeline)
            e.setDepthStencilState(less)
            GPU.draw(e, corners: leftHalf(z: 0.25), color: SIMD4(0.75, 0.5, 0.25, 1))
            e.endEncoding()
        }
        var result = AsDepth()
        result.depth = try await gpu.read(depth, bytesPerPixel: 4)
        result.colour = try await gpu.read(colour, bytesPerPixel: 4)
        result.blockLinear = facts(gpu, depth)["block_linear"]
        return result
    }

    /// The facade's switchMetalProbeFacts: of `texture` (debug builds; empty elsewhere)
    static func facts(_ gpu: GPU, _ texture: MTLTexture) -> [String: Int] {
        guard let facade = gpu.device as? NSObject, facade.responds(to: factsSelector),
              let line = facade.perform(factsSelector, with: texture)?.takeUnretainedValue() as? String
        else { return [:] }
        var out: [String: Int] = [:]
        for field in line.split(separator: " ") {
            let pair = field.split(separator: "=", maxSplits: 1)
            if pair.count == 2, let value = Int(pair[1]) { out[String(pair[0])] = value }
        }
        return out
    }

    static func word(_ bytes: [UInt8], _ texel: Int, _ size: Int) -> UInt32 {
        (0 ..< size).reduce(UInt32(0)) { $0 | UInt32(bytes[texel * size + $1]) << (8 * $1) }
    }

    /// Each half's texel in hex where every texel of the half holds it, else the centre texel's and "_mixed"
    static func halves(_ bytes: [UInt8], size: Int) -> (left: String, right: String) {
        func half(_ xs: Range<Int>) -> String {
            let words = (0 ..< side).flatMap { y in xs.map { word(bytes, y * side + $0, size) } }
            let centre = String(word(bytes, (side / 2) * side + xs.lowerBound + xs.count / 2, size), radix: 16)
            return Set(words).count == 1 ? centre : centre + "_mixed"
        }
        return (half(0 ..< side / 2), half(side / 2 ..< side))
    }

    /// Colour texels off the draw's reach: written over the left half, the clear's zero over the right
    static func colourOff(_ colour: [UInt8], written: Bool) -> Int {
        (0 ..< side * side).filter { t in
            let drawn = colour[4 * t ..< 4 * t + 4].contains { $0 != 0 }
            return drawn != (written && t % side < side / 2)
        }.count
    }
}
