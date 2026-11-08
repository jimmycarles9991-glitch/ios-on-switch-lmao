import Foundation
import HWTestCore
import Metal

/// A glyph cache the way text renderers keep one: a 512x512 R8 or A8 atlas, small glyphs packed at odd offsets with
/// one-texel gaps, uploaded a batch at a time between draws that sample it. Each draw copies the whole atlas through
/// the texture unit (nearest sampling at texel centres, so exactly) into an R8 target; the atlas is also read back by
/// the blit engine. The second batch lands while the first draw may still be in flight, so an upload that is not
/// ordered after the draw before it, or a texel the sampler reads from the wrong place in the block-linear layout,
/// shows up as noise in the glyphs: what Settings' "Wi-Fi" label looked like.
struct GlyphAtlasScene: GPUScene {
    let id = "gpu.upload.glyph_atlas"
    let name = "Glyph atlas: uploads at odd offsets between draws that sample it"

    static let size = 512

    struct Glyph {
        let rect: UploadRect
        let bytes: [UInt8]
    }

    static func glyphs(count: Int) -> [Glyph] {
        var (x, y, shelf) = (1, 1, 0)
        return (0..<count).map { index in
            let (w, h) = (3 + (index * 7) % 15, 5 + (index * 11) % 17)
            if x + w + 1 > size {
                (x, y, shelf) = (1, y + shelf + 1, 0)
            }
            defer {
                x += w + 1 + (index % 2)
                shelf = max(shelf, h)
            }
            let raster = Patterns.glyph(width: w, height: h, index: index)
            return Glyph(
                rect: UploadRect(x: x, y: y, width: w, height: h),
                bytes: raster.samples.map {
                    UInt8(($0 * 255).rounded())
                })
        }
    }

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let glyphs = Self.glyphs(count: 160)
        let size = Self.size
        let canReplacePrivate = RawFacade.kind(of: gpu.device) == "raw"
        for format in [TexelFormat.r8Unorm, .a8Unorm] {
            let atlas = try gpu.texture(format.metal, size, size, usage: [.shaderRead])
            var model = TexelImage(format, size, size, Raster(width: size, height: size, channels: 1) { _, _, _ in 0 })
            try await gpu.blitUpload(
                [UploadRect(x: 0, y: 0, width: size, height: size)], [model.bytes], into: atlas, texel: 1)
            let pipeline = try gpu.render(
                vertex: "textured_vertex", fragment: format == .a8Unorm ? "alpha_fragment" : "nearest_fragment",
                format: .r8Unorm)
            let batches = [0..<50, 50..<110, 110..<160]
            var snapshots: [[UInt8]] = []
            var pending: [MTLCommandBuffer] = []
            var targets: [MTLTexture] = []
            for (batch, range) in batches.enumerated() {
                let slice = Array(glyphs[range])
                let target = try gpu.texture(.r8Unorm, size, size)
                targets.append(target)
                if batch == 2 && canReplacePrivate {
                    try gpu.finish(pending)
                    pending = []
                    for glyph in slice { GPU.replace(glyph.rect, glyph.bytes, in: atlas, texel: 1) }
                } else {
                    pending.append(try gpu.commitUpload(slice.map(\.rect), slice.map(\.bytes), into: atlas, texel: 1))
                }
                for glyph in slice { model.write(glyph.rect, glyph.bytes) }
                snapshots.append(model.bytes)
                pending.append(try gpu.commit { draw($0, pipeline, atlas, into: target) })
            }
            try gpu.finish(pending)
            for (batch, target) in targets.enumerated() {
                let how = batch == 2 && canReplacePrivate ? "replaceRegion" : "blit"
                let drawn = try await gpu.raster(target, .r8Unorm)
                let want = TexelFormat.r8Unorm.raster(snapshots[batch], width: size, height: size)
                result.compare("\(format.rawValue) batch \(batch) (\(how)) sampled", drawn, want, tolerance: 1.01 / 255)
            }
            let back = try await gpu.read(atlas, bytesPerPixel: 1)
            result.compare(
                "\(format.rawValue) atlas blit readback", bytes: back, expected: model.bytes, format: format,
                width: size, height: size, tolerance: 1.01 / 255)
        }
        return result
    }

    func draw(
        _ commands: MTLCommandBuffer, _ pipeline: MTLRenderPipelineState, _ atlas: MTLTexture, into target: MTLTexture
    ) {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.5, green: 0, blue: 0, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
        encoder.setRenderPipelineState(pipeline)
        var corners = GPU.fullScreen
        var uvs: [SIMD2<Float>] = [SIMD2(0, 1), SIMD2(1, 1), SIMD2(0, 0), SIMD2(1, 0)]
        encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
        encoder.setVertexBytes(&uvs, length: MemoryLayout<SIMD2<Float>>.stride * 4, index: 1)
        encoder.setFragmentTexture(atlas, index: 0)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        encoder.endEncoding()
    }
}
