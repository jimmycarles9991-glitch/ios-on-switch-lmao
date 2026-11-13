import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.raw.mip_twod's chains of one format: made by generateMipmaps, or drawn by 3D passes (mip_step_fragment) one
/// one-level texture a level, from the same level 0, and each step compared with MipReference
final class MipChains {
    let gpu: GPU
    let format: MipTwodTest.Format
    private var drawState: MTLRenderPipelineState?

    init(gpu: GPU, format: MipTwodTest.Format) {
        self.gpu = gpu
        self.format = format
    }

    var layout: MipLayout { format.layout }

    static func dims(_ w: Int, _ h: Int) -> [(Int, Int)] {
        var out = [(w, h)]
        while out.last! != (1, 1) {
            let (a, b) = out.last!
            out.append((max(1, a / 2), max(1, b / 2)))
        }
        return out
    }

    /// rule mip.twod's proven steps: every weight a multiple of 1/4
    static func proven(_ s: Int, _ d: Int) -> Bool { d > 0 && (2 * s) % d == 0 }

    /// The facade's debug switch of 2D-engine mips for the 3D engine's formats: what it was, nil without one
    static func probe(_ device: MTLDevice, _ on: Bool) -> Bool? {
        let selector = NSSelectorFromString("switchRawProbeMipCandidates:")
        guard let object = device as? NSObject, object.responds(to: selector) else { return nil }
        return (object.perform(selector, with: NSNumber(value: on))?.takeUnretainedValue() as? NSNumber)?.boolValue
            ?? false
    }

    func pattern(_ w: Int, _ h: Int, seed: Int) -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: w * h * layout.bytes)
        for y in 0..<h {
            for x in 0..<w {
                for (i, f) in layout.fields.enumerated() {
                    var v =
                        UInt32(truncatingIfNeeded: x &* 73_856_093) ^ UInt32(truncatingIfNeeded: y &* 19_349_663)
                        ^ UInt32(truncatingIfNeeded: (seed * 8 + i + 1) &* 83_492_791)
                    v ^= v >> 13
                    v = v &* 0x5bd1_e995
                    v ^= v >> 15
                    let code = f.kind == .float ? layout.code(of: Double(v & 0xffff) / 65535, f) : v & f.mask
                    layout.put(code, &bytes, texel: y * w + x, field: f)
                }
            }
        }
        return bytes
    }

    /// generateMipmaps of a chain whose level 0 is the pattern: every level's bytes and the command buffer's path
    func generated(_ w: Int, _ h: Int, seed: Int = 0) async throws -> ([[UInt8]], String) {
        let dims = Self.dims(w, h)
        let chain = try gpu.texture(format.pixel, w, h, usage: [.shaderRead, .renderTarget], mipmaps: dims.count)
        try await gpu.upload(pattern(w, h, seed: seed), to: chain, bytesPerRow: w * layout.bytes)
        let timing = try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.generateMipmaps(for: chain)
            blit.endEncoding()
        }
        var levels: [[UInt8]] = []
        for l in 0..<dims.count { levels.append(try await gpu.read(chain, bytesPerPixel: layout.bytes, level: l)) }
        return (levels, timing.path)
    }

    /// The chain drawn from `level0` by 3D passes into one-level textures
    func drawn(_ level0: [UInt8], _ w: Int, _ h: Int) async throws -> ([[UInt8]], String) {
        let textures = try targets(w, h)
        try await gpu.upload(level0, to: textures[0], bytesPerRow: w * layout.bytes)
        let state = try pipeline()
        let timing = try await gpu.run { draw($0, textures, state) }
        var levels = [level0]
        for t in textures.dropFirst() { levels.append(try await gpu.read(t, bytesPerPixel: layout.bytes)) }
        return (levels, timing.path)
    }

    /// The fastest of three GPU times of a size x size chain each way
    func times(_ size: Int) async throws -> (Double, Double) {
        let dims = Self.dims(size, size)
        let chain = try gpu.texture(format.pixel, size, size, usage: [.shaderRead, .renderTarget], mipmaps: dims.count)
        let textures = try targets(size, size)
        let level0 = pattern(size, size, seed: 1)
        try await gpu.upload(level0, to: chain, bytesPerRow: size * layout.bytes)
        try await gpu.upload(level0, to: textures[0], bytesPerRow: size * layout.bytes)
        let state = try pipeline()
        var twod = Double.infinity, passes = Double.infinity
        for _ in 0..<3 {
            twod = min(
                twod,
                try await gpu.run { commands in
                    let blit = commands.makeBlitCommandEncoder()!
                    blit.generateMipmaps(for: chain)
                    blit.endEncoding()
                }.gpuMilliseconds)
            passes = min(passes, try await gpu.run { draw($0, textures, state) }.gpuMilliseconds)
        }
        return (twod, passes)
    }

    /// Each step of `levels` against MipReference from the level above: into `proven` or `odd` by rule mip.twod, and each
    /// 2:1 step's rounding into `rounding`
    func compare(
        _ levels: [[UInt8]], _ w: Int, _ h: Int, proven: inout MipTally, odd: inout MipTally, rounding: MipRounding?
    ) {
        let dims = Self.dims(w, h)
        for l in 1..<min(dims.count, levels.count) {
            let (sw, sh) = dims[l - 1], (dw, dh) = dims[l]
            let want = MipReference.step(levels[l - 1], sw, sh, dw, dh, layout)
            if Self.proven(sw, dw) && Self.proven(sh, dh) {
                proven.add(levels[l], want, texels: dw * dh, layout)
            } else {
                odd.add(levels[l], want, texels: dw * dh, layout)
            }
            if let rounding, sw == 2 * dw, sh == 2 * dh { rounding.add(levels[l - 1], sw, levels[l], dw, dh, layout) }
        }
    }

    private func targets(_ w: Int, _ h: Int) throws -> [MTLTexture] {
        try Self.dims(w, h).map { try gpu.texture(format.pixel, $0.0, $0.1, usage: [.renderTarget, .shaderRead]) }
    }

    private func pipeline() throws -> MTLRenderPipelineState {
        if let drawState { return drawState }
        let state = try gpu.render(fragment: "mip_step_fragment", format: format.pixel)
        drawState = state
        return state
    }

    private func draw(_ commands: MTLCommandBuffer, _ textures: [MTLTexture], _ state: MTLRenderPipelineState) {
        for l in 1..<textures.count {
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = textures[l]
            pass.colorAttachments[0].loadAction = .dontCare
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            encoder.setFragmentTexture(textures[l - 1], index: 0)
            var inverse = SIMD2<Float>(1 / Float(textures[l].width), 1 / Float(textures[l].height))
            encoder.setFragmentBytes(&inverse, length: 8, index: 0)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: 64, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
    }
}
