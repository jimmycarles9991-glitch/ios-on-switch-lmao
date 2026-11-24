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
    let depthWrite: MTLDepthStencilState?

    var isDepth: Bool { format == .depth32Float }
    var atlasFormat: MTLPixelFormat { isDepth ? .r32Float : format }
    var bytesPerTexel: Int { format == .rgba32Float ? 16 : format == .rgba16Float ? 8 : 4 }
    var name: String {
        switch format {
        case .depth32Float: return "d32f"
        case .rgba32Float: return "rgba32f"
        case .rgba16Float: return "rgba16f"
        default: return "bgra8"
        }
    }

    /// BGRA8Unorm, RGBA16Float, RGBA32Float or Depth32Float
    init(_ gpu: GPU, _ format: MTLPixelFormat) throws {
        self.gpu = gpu
        self.format = format
        if format == .depth32Float {
            seeded = try gpu.render(fragment: "x1_seeded_depth", format: .invalid, depth: .depth32Float)
            tap = try gpu.render(fragment: "x1_tap_depth", format: .r32Float)
            box = tap
            let ds = MTLDepthStencilDescriptor()
            ds.depthCompareFunction = .always
            ds.isDepthWriteEnabled = true
            depthWrite = gpu.device.makeDepthStencilState(descriptor: ds)
            return
        }
        let half = format == .rgba16Float
        let seed = format == .rgba32Float ? "x1_seeded_float" : half ? "x1_seeded_half" : "x1_seeded_unorm"
        seeded = try gpu.render(fragment: seed, format: format)
        tap = try gpu.render(fragment: half ? "x1_tap_half" : "x1_tap_unorm", format: format)
        box = try gpu.render(fragment: half ? "x1_box_half" : "x1_box_unorm", format: format)
        depthWrite = nil
    }

    enum Layout: String {
        case blockLinear = "bl"
        case pitch
        case compressed = "comp"
    }

    /// A render target the facade lays out as `layout` (pitch: over a shared buffer's 256-byte rows, as a shared texture
    /// of its own memory is block-linear); compressed answers nil where the device made it plain.
    func target(_ width: Int, _ height: Int, _ layout: Layout) throws -> MTLTexture? {
        if layout == .pitch {
            let rowBytes = (width * bytesPerTexel + 255) / 256 * 256
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: format, width: width, height: height, mipmapped: false
            )
            descriptor.usage = [.renderTarget, .shaderRead]
            descriptor.storageMode = .shared
            return try gpu.buffer(rowBytes * height).makeTexture(
                descriptor: descriptor, offset: 0, bytesPerRow: rowBytes
            )
        }
        if layout == .compressed { try GM20BProbe.compressNext(gpu.device) }
        let texture = try gpu.texture(format, width, height, usage: [.renderTarget, .shaderRead])
        if layout == .compressed, try GM20BProbe.facts(gpu.device, texture)["comptags"] != 1 { return nil }
        return texture
    }

    func atlas(_ trials: Int) throws -> MTLTexture {
        try gpu.texture(atlasFormat, Self.tile, Self.tile * trials, usage: [.renderTarget, .shaderRead])
    }

    static func pass(_ target: MTLTexture) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .load
        pass.colorAttachments[0].storeAction = .store
        return pass
    }

    static func depthPass(_ target: MTLTexture) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        pass.depthAttachment.texture = target
        pass.depthAttachment.loadAction = .load
        pass.depthAttachment.storeAction = .store
        return pass
    }

    /// Pass A: `seed`'s pattern over [x0, x1) x [y0, y1) of `target` (all of it by default), `spin` rounds a pixel
    func seed(
        _ commands: MTLCommandBuffer, _ target: MTLTexture, seed: Int, spin: Int, window: (Int, Int, Int, Int)? = nil
    ) {
        let pass = isDepth ? Self.depthPass(target) : Self.pass(target)
        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
        encoder.setRenderPipelineState(seeded)
        if let depthWrite = depthWrite { encoder.setDepthStencilState(depthWrite) }
        var params = (UInt32(seed), UInt32(spin), Float(0), UInt32(0))
        encoder.setFragmentBytes(&params, length: 16, index: 0)
        let (x0, y0, x1, y1) = window ?? (0, 0, target.width, target.height)
        let strips = window == nil ? 4 : 1
        for s in 0 ..< strips {
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
            0, index * Self.tile, Self.tile, (index + 1) * Self.tile, width: atlas.width, height: atlas.height
        )
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
                height: atlas.height
            )
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
            destinationOffset: 0, destinationBytesPerRow: atlas.width * bytesPerTexel, destinationBytesPerImage: 0
        )
        blit.endEncoding()
    }

    var tileBytes: Int { Self.tile * Self.tile * bytesPerTexel }

    func tile(_ buffer: MTLBuffer, _ index: Int) -> UnsafeRawBufferPointer {
        UnsafeRawBufferPointer(start: buffer.contents() + index * tileBytes, count: tileBytes)
    }

    var pattern: BarrierPattern { format == .rgba32Float ? .rgba32f : format == .rgba16Float ? .rgba16f : .bgra8 }

    func texel(_ x: Int, _ y: Int, seed: Int) -> [UInt8] { pattern.texel(x, y, seed: seed) }

    /// What a tap of `seed`'s pattern over a width x height source gives, the CPU's reading
    func expectedTap(seed: Int, base: SIMD2<Int32>, step: SIMD2<Int32>, width: Int, height: Int) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: tileBytes)
        out.withUnsafeMutableBytes { bytes in
            for y in 0 ..< Self.tile {
                for x in 0 ..< Self.tile {
                    let sx = min(max(Int(base.x) + x * Int(step.x), 0), width - 1)
                    let sy = min(max(Int(base.y) + y * Int(step.y), 0), height - 1)
                    let offset = (y * Self.tile + x) * bytesPerTexel
                    if isDepth {
                        let v = probeHash(UInt32(sx), UInt32(sy), UInt32(seed))
                        let d = Float(v & 0xfffff) * (1.0 / 1_048_576.0)
                        bytes.storeBytes(of: d.bitPattern.littleEndian, toByteOffset: offset, as: UInt32.self)
                    } else {
                        pattern.put(sx, sy, seed: seed, into: bytes, at: offset)
                    }
                }
            }
        }
        return out
    }
}
