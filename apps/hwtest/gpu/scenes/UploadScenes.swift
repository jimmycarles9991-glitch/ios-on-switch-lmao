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

/// replaceRegion and getBytes of level 0 of a mipmapped shared texture on the raw facade: the copy engine moves them, as
/// it moves every later level (a chain's level 0 is the texture itself, which the device refused, so its CPU copies fell
/// to the CPU's swizzle). The bytes read back are the ones written, every level apart keeps its own, and the facade's
/// texture_uploads and texture_readbacks count one each.
struct ChainLevelZeroScene: GPUScene {
    let id = "gpu.upload.chain_level0"
    let name = "replaceRegion and getBytes of a mipmapped texture's level 0 by the copy engine"

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let (width, height, format) = (64, 64, TexelFormat.bgra8Unorm)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format.metal, width: width, height: height, mipmapped: true)
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: descriptor) else {
            throw TestFailure("makeTexture of a mipmapped shared 64x64 BGRA8 returned nil")
        }
        let before = RawFacade.counters(of: gpu.device)
        let levelOne = UploadRect(x: 0, y: 0, width: width / 2, height: height / 2)
        let one = Patterns.bytes(
            Patterns.busy(width: levelOne.width, height: levelOne.height, channels: format.channels, seed: 9), format)
        texture.replace(
            region: levelOne.region, mipmapLevel: 1, withBytes: one, bytesPerRow: levelOne.width * format.bytesPerTexel)
        let whole = UploadRect(x: 0, y: 0, width: width, height: height)
        let zero = Patterns.bytes(
            Patterns.busy(width: width, height: height, channels: format.channels, seed: 3), format)
        GPU.replace(whole, zero, in: texture, texel: format.bytesPerTexel)
        let back = GPU.getBytes(whole, from: texture, texel: format.bytesPerTexel)
        result.compare(
            "level 0 getBytes", bytes: back, expected: zero, format: format, width: width, height: height, tolerance: 0)
        var levelOneBack = [UInt8](repeating: 0, count: one.count)
        texture.getBytes(
            &levelOneBack, bytesPerRow: levelOne.width * format.bytesPerTexel, from: levelOne.region, mipmapLevel: 1)
        result.compare(
            "level 1 kept", bytes: levelOneBack, expected: one, format: format, width: levelOne.width,
            height: levelOne.height, tolerance: 0)
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            result.notes.append("the copy-engine route is the raw facade's")
            return result
        }
        let after = RawFacade.counters(of: gpu.device)
        for key in ["texture_uploads", "texture_readbacks"] {
            guard let first = before[key], let last = after[key] else {
                result.notes.append("\(key) route counters unavailable in this build")
                continue
            }
            result.metrics.set(key, last - first)
            result.expect(last - first >= 2, "\(key) \(last - first): level 0 did not go through the copy engine")
        }
        return result
    }
}
