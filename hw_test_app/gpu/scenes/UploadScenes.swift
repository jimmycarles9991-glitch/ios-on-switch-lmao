import Foundation
import HWTestCore
import Metal

/// Buffer-to-texture blits into private textures (block-linear where the shape tiles) at offsets and sizes no GOB
/// lines up with, from a staging buffer with padded rows: the copy engine's block-linear writes on the raw facade.
struct BlitUploadScene: GPUScene {
    let id = "gpu.upload.blit_buffer"
    let name = "Buffer-to-texture uploads at unaligned offsets"

    static let formats: [TexelFormat] = [
        .r8Unorm, .a8Unorm, .rg8Unorm, .rgba8Unorm, .bgra8Unorm, .rgba16Float, .r32Float,
    ]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        for format in Self.formats {
            for (width, height) in [(64, 64), (333, 77), (1024, 16)] {
                let texture = try gpu.texture(format.metal, width, height, usage: [.shaderRead])
                var model = TexelImage(
                    format, width, height, Patterns.busy(width: width, height: height, channels: format.channels))
                try await gpu.blitUpload(
                    [UploadRect(x: 0, y: 0, width: width, height: height)], [model.bytes], into: texture,
                    texel: format.bytesPerTexel)
                let rects = UploadRect.unaligned(width: width, height: height)
                let sources = rects.enumerated().map { index, rect in
                    Patterns.bytes(
                        Patterns.busy(
                            width: rect.width, height: rect.height, channels: format.channels, seed: index + 2), format)
                }
                for (rect, source) in zip(rects, sources) { model.write(rect, source) }
                try await gpu.blitUpload(rects, sources, into: texture, texel: format.bytesPerTexel)
                try await result.compareBothWays("\(format.rawValue) \(width)x\(height)", texture, model, gpu: gpu)
            }
        }
        return result
    }
}

/// replaceRegion and getBytes at unaligned offsets: into shared textures (rows), copied on the GPU into private ones;
/// and, on the raw facade, straight into private R8, A8 and BGRA8 textures, which it keeps block-linear and whose
/// texels its CPU writes swizzle (the way Core Animation's glyph atlas is filled). Metal on a Mac refuses CPU access
/// to private textures, so that half runs only on the Switch's devices.
struct ReplaceRegionScene: GPUScene {
    let id = "gpu.upload.replace_region"
    let name = "replaceRegion and getBytes at unaligned offsets"

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let kind = RawFacade.kind(of: gpu.device)
        let before = RawFacade.counters(of: gpu.device)
        for format in [TexelFormat.r8Unorm, .a8Unorm, .bgra8Unorm, .rgba8Unorm, .rgba16Float] {
            let (width, height) = (333, 77)
            var storages: [MTLStorageMode] = [.shared]
            if kind == "raw" && [.r8Unorm, .a8Unorm, .bgra8Unorm].contains(format) { storages.append(.private) }
            for storage in storages {
                let label = "\(format.rawValue) \(storage == .shared ? "shared" : "private")"
                let texture = try gpu.texture(format.metal, width, height, usage: [.shaderRead], storage: storage)
                var model = TexelImage(
                    format, width, height, Patterns.busy(width: width, height: height, channels: format.channels))
                texture.replace(
                    region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0, withBytes: model.bytes,
                    bytesPerRow: model.rowBytes)
                for (index, rect) in UploadRect.unaligned(width: width, height: height).enumerated() {
                    let source = Patterns.bytes(
                        Patterns.busy(
                            width: rect.width, height: rect.height, channels: format.channels, seed: index + 5),
                        format)
                    GPU.replace(rect, source, in: texture, texel: format.bytesPerTexel)
                    model.write(rect, source)
                    let back = GPU.getBytes(rect, from: texture, texel: format.bytesPerTexel)
                    result.compare(
                        "\(label) getBytes \(rect.name)", bytes: back, expected: model.read(rect), format: format,
                        width: rect.width, height: rect.height, tolerance: format.step() * 1.01)
                }
                try await result.compareBothWays(label, texture, model, gpu: gpu)
                let copy = try gpu.texture(format.metal, width, height, usage: [.shaderRead])
                try await gpu.run { commands in
                    let blit = commands.makeBlitCommandEncoder()!
                    blit.copy(from: texture, to: copy)
                    blit.endEncoding()
                }
                try await result.compareBothWays(label + " copied to private", copy, model, gpu: gpu)
            }
        }
        if kind == "raw" {
            let after = RawFacade.counters(of: gpu.device)
            for key in ["texture_uploads", "texture_readbacks"] {
                if let first = before[key], let last = after[key] {
                    result.metrics.set(key, last - first)
                    result.expect(last > first, "\(key) did not exercise the native copy-engine route")
                } else {
                    result.notes.append("\(key) route counters unavailable in this build")
                }
            }
        } else {
            result.notes.append("private-texture replaceRegion only on the raw facade")
        }
        return result
    }
}
