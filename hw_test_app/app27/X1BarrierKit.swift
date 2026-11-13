import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// What the barrier probes draw (X1Probes.metal): pass A writes a seeded texel pattern over its whole target in
/// horizontal strips, top to bottom, so its last pixels are the bottom right's; pass B copies texels of a source into
/// a 64 x 64 tile of an atlas whose tiles are stacked down, so each trial's result is one contiguous run of the
/// readback. A trial that reads its source before A's writes land, or through a cache holding an earlier seed's
/// lines, copies another seed's texels.
struct BarrierKit {
    static let tile = 64

    let gpu: GPU
    let format: MTLPixelFormat
    let seeded: MTLRenderPipelineState
    let tap: MTLRenderPipelineState
    let box: MTLRenderPipelineState

    var bytesPerTexel: Int { format == .rgba16Float ? 8 : 4 }
    var name: String { format == .rgba16Float ? "rgba16f" : "bgra8" }

    init(_ gpu: GPU, _ format: MTLPixelFormat) throws {
        self.gpu = gpu
        self.format = format
        let half = format == .rgba16Float
        seeded = try gpu.render(fragment: half ? "x1_seeded_half" : "x1_seeded_unorm", format: format)
        tap = try gpu.render(fragment: half ? "x1_tap_half" : "x1_tap_unorm", format: format)
        box = try gpu.render(fragment: half ? "x1_box_half" : "x1_box_unorm", format: format)
    }

    enum Layout: String {
        case blockLinear = "bl"
        case pitch
        case compressed = "comp"
    }

    /// A render target the facade lays out as `layout`; compressed answers nil where the device made it plain.
    func target(_ width: Int, _ height: Int, _ layout: Layout) throws -> MTLTexture? {
        if layout == .compressed { try RawProbe.compressNext(gpu.device) }
        let texture = try gpu.texture(
            format, width, height, usage: [.renderTarget, .shaderRead],
            storage: layout == .pitch ? .shared : .private)
        if layout == .compressed, try RawProbe.facts(gpu.device, texture)["comptags"] != 1 { return nil }
        return texture
    }

    func atlas(_ trials: Int) throws -> MTLTexture {
        try gpu.texture(format, Self.tile, Self.tile * trials, usage: [.renderTarget, .shaderRead])
    }

    static func pass(_ target: MTLTexture) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .load
        pass.colorAttachments[0].storeAction = .store
        return pass
    }

    /// Pass A: `seed`'s pattern over [x0, x1) x [y0, y1) of `target` (all of it by default), `spin` rounds a pixel
    func seed(
        _ commands: MTLCommandBuffer, _ target: MTLTexture, seed: Int, spin: Int, window: (Int, Int, Int, Int)? = nil
    ) {
        let encoder = commands.makeRenderCommandEncoder(descriptor: Self.pass(target))!
        encoder.setRenderPipelineState(seeded)
        var params = (UInt32(seed), UInt32(spin), Float(0), UInt32(0))
        encoder.setFragmentBytes(&params, length: 16, index: 0)
        let (x0, y0, x1, y1) = window ?? (0, 0, target.width, target.height)
        let strips = window == nil ? 4 : 1
        for s in 0..<strips {
            let top = y0 + (y1 - y0) * s / strips, bottom = y0 + (y1 - y0) * (s + 1) / strips
            var corners = GPU.pixelQuad(x0, top, x1, bottom, width: target.width, height: target.height)
            encoder.setVertexBytes(&corners, length: 64, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        }
        encoder.endEncoding()
    }

    /// Pass B: tile `index` of `atlas` from `source` at base + (x, y) * step
    func tap(
        _ commands: MTLCommandBuffer, _ source: MTLTexture, into atlas: MTLTexture, index: Int, base: SIMD2<Int32>,
        step: SIMD2<Int32>
    ) {
        let encoder = commands.makeRenderCommandEncoder(descriptor: Self.pass(atlas))!
        encoder.setRenderPipelineState(tap)
        encoder.setFragmentTexture(source, index: 0)
        var params = [
            base, step, SIMD2<Int32>(0, Int32(index * Self.tile)),
            SIMD2<Int32>(Int32(source.width), Int32(source.height)),
        ]
        encoder.setFragmentBytes(&params, length: 32, index: 0)
        var corners = GPU.pixelQuad(
            0, index * Self.tile, Self.tile, (index + 1) * Self.tile, width: atlas.width, height: atlas.height)
        encoder.setVertexBytes(&corners, length: 64, index: 0)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        encoder.endEncoding()
    }

    /// Pass B for many trials in one pass: tile k of `atlas` from all of `sources[k]` (no barrier between the draws)
    func tapAll(_ commands: MTLCommandBuffer, _ sources: [MTLTexture], into atlas: MTLTexture) {
        let encoder = commands.makeRenderCommandEncoder(descriptor: Self.pass(atlas))!
        encoder.setRenderPipelineState(tap)
        for (k, source) in sources.enumerated() {
            encoder.setFragmentTexture(source, index: 0)
            var params = [
                SIMD2<Int32>(0, 0), SIMD2(1, 1), SIMD2(0, Int32(k * Self.tile)),
                SIMD2(Int32(source.width), Int32(source.height)),
            ]
            encoder.setFragmentBytes(&params, length: 32, index: 0)
            var corners = GPU.pixelQuad(
                0, k * Self.tile, Self.tile, (k + 1) * Self.tile, width: atlas.width,
                height: atlas.height)
            encoder.setVertexBytes(&corners, length: 64, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        }
        encoder.endEncoding()
    }

    /// A pyramid level: `into` from the 2 x 2 boxes of `source`
    func level(_ commands: MTLCommandBuffer, _ source: MTLTexture, into: MTLTexture) {
        let encoder = commands.makeRenderCommandEncoder(descriptor: Self.pass(into))!
        encoder.setRenderPipelineState(box)
        encoder.setFragmentTexture(source, index: 0)
        var corners = GPU.fullScreen
        encoder.setVertexBytes(&corners, length: 64, index: 0)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        encoder.endEncoding()
    }

    /// The atlas into `buffer`, rows packed, at the end of the command buffer
    func readback(_ commands: MTLCommandBuffer, _ atlas: MTLTexture, into buffer: MTLBuffer) {
        let blit = commands.makeBlitCommandEncoder()!
        blit.copy(
            from: atlas, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(),
            sourceSize: MTLSize(width: atlas.width, height: atlas.height, depth: 1), to: buffer,
            destinationOffset: 0, destinationBytesPerRow: atlas.width * bytesPerTexel, destinationBytesPerImage: 0)
        blit.endEncoding()
    }

    var tileBytes: Int { Self.tile * Self.tile * bytesPerTexel }

    func tile(_ buffer: MTLBuffer, _ index: Int) -> UnsafeRawBufferPointer {
        UnsafeRawBufferPointer(start: buffer.contents() + index * tileBytes, count: tileBytes)
    }

    /// The bytes pass A's seed puts at (x, y) in memory (BGRA8: blue, green, red, alpha; RGBA16F: four halves)
    func texel(_ x: Int, _ y: Int, seed: Int) -> [UInt8] {
        let v = probeHash(UInt32(x), UInt32(y), UInt32(seed))
        if format == .rgba16Float {
            let halves = [v & 0x3ff, (v >> 10) & 0x3ff, (v >> 20) & 0x3ff, (v >> 22) & 0x3ff].map {
                UInt16(0x3c00 | $0)
            }
            return halves.flatMap { [UInt8($0 & 0xff), UInt8($0 >> 8)] }
        }
        return [UInt8((v >> 16) & 255), UInt8((v >> 8) & 255), UInt8(v & 255), UInt8(v >> 24)]
    }

    /// What a tap of `seed`'s pattern over a width x height source gives, the CPU's reading
    func expectedTap(seed: Int, base: SIMD2<Int32>, step: SIMD2<Int32>, width: Int, height: Int) -> [UInt8] {
        var out: [UInt8] = []
        out.reserveCapacity(tileBytes)
        for y in 0..<Self.tile {
            for x in 0..<Self.tile {
                let sx = min(max(Int(base.x) + x * Int(step.x), 0), width - 1)
                let sy = min(max(Int(base.y) + y * Int(step.y), 0), height - 1)
                out += texel(sx, sy, seed: seed)
            }
        }
        return out
    }
}
