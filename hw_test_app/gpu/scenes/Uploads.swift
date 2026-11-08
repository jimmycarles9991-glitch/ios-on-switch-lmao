import Foundation
import HWTestCore
import Metal

/// The texel bytes of a texture as the CPU models them, and the rectangles uploads write into them.
struct TexelImage {
    let format: TexelFormat
    let width: Int
    let height: Int
    var bytes: [UInt8]

    init(_ format: TexelFormat, _ width: Int, _ height: Int, _ raster: Raster) {
        self.format = format
        self.width = width
        self.height = height
        bytes = Patterns.bytes(raster, format)
    }

    var rowBytes: Int { width * format.bytesPerTexel }

    mutating func write(_ rect: UploadRect, _ source: [UInt8]) {
        let texel = format.bytesPerTexel
        for row in 0..<rect.height {
            let from = row * rect.width * texel
            let to = (rect.y + row) * rowBytes + rect.x * texel
            bytes.replaceSubrange(to..<(to + rect.width * texel), with: source[from..<(from + rect.width * texel)])
        }
    }

    func read(_ rect: UploadRect) -> [UInt8] {
        let texel = format.bytesPerTexel
        return (0..<rect.height).flatMap { row -> ArraySlice<UInt8> in
            let at = (rect.y + row) * rowBytes + rect.x * texel
            return bytes[at..<(at + rect.width * texel)]
        }
    }
}

struct UploadRect {
    let x: Int
    let y: Int
    let width: Int
    let height: Int

    var region: MTLRegion { MTLRegionMake2D(x, y, width, height) }
    var name: String { "\(width)x\(height)@\(x),\(y)" }

    /// Rectangles at offsets no GOB (64 bytes x 8 rows) starts at: single texels, ones that straddle a GOB's edge,
    /// the last corner, a whole row and a whole column, clipped to the texture.
    static func unaligned(width: Int, height: Int) -> [UploadRect] {
        let wanted = [
            (1, 1, 1, 1), (37, 5, 13, 9), (63, 7, 3, 10), (width - 7, height - 3, 7, 3), (0, height / 2, width, 1),
            (width / 3, 0, 1, height), (17, 3, 70, 19),
        ]
        return wanted.compactMap { x, y, w, h in
            let (x, y) = (max(0, min(x, width - 1)), max(0, min(y, height - 1)))
            let rect = UploadRect(x: x, y: y, width: min(w, width - x), height: min(h, height - y))
            return rect.width > 0 && rect.height > 0 ? rect : nil
        }
    }
}

extension GPU {
    /// `rects` of `sources` copied from one staging buffer at padded rows and odd offsets, in one blit encoder.
    func blitUpload(_ rects: [UploadRect], _ sources: [[UInt8]], into texture: MTLTexture, texel: Int) async throws {
        try finish([try commitUpload(rects, sources, into: texture, texel: texel)])
    }

    func commitUpload(
        _ rects: [UploadRect], _ sources: [[UInt8]], into texture: MTLTexture, texel: Int
    ) throws
        -> MTLCommandBuffer
    {
        var staging: [UInt8] = []
        var places: [(offset: Int, rowBytes: Int)] = []
        for (rect, source) in zip(rects, sources) {
            staging += [UInt8](repeating: 0xEE, count: (16 - staging.count % 16) % 16 + texel * 3)
            let rowBytes = (rect.width + 3) * texel
            places.append((staging.count, rowBytes))
            for row in 0..<rect.height {
                staging += source[(row * rect.width * texel)..<((row + 1) * rect.width * texel)]
                staging += [UInt8](repeating: 0xEE, count: 3 * texel)
            }
        }
        let buffer = try buffer(staging)
        return try commit { commands in
            let blit = commands.makeBlitCommandEncoder()!
            for (rect, place) in zip(rects, places) {
                blit.copy(
                    from: buffer, sourceOffset: place.offset, sourceBytesPerRow: place.rowBytes,
                    sourceBytesPerImage: 0, sourceSize: MTLSize(width: rect.width, height: rect.height, depth: 1),
                    to: texture, destinationSlice: 0, destinationLevel: 0,
                    destinationOrigin: MTLOrigin(x: rect.x, y: rect.y, z: 0))
            }
            blit.endEncoding()
        }
    }

    /// replaceRegion with rows padded by three texels.
    static func replace(_ rect: UploadRect, _ source: [UInt8], in texture: MTLTexture, texel: Int) {
        let rowBytes = (rect.width + 3) * texel
        var padded = [UInt8](repeating: 0xEE, count: rowBytes * rect.height)
        for row in 0..<rect.height {
            padded.replaceSubrange(
                (row * rowBytes)..<(row * rowBytes + rect.width * texel),
                with: source[(row * rect.width * texel)..<((row + 1) * rect.width * texel)])
        }
        texture.replace(region: rect.region, mipmapLevel: 0, withBytes: padded, bytesPerRow: rowBytes)
    }

    static func getBytes(_ rect: UploadRect, from texture: MTLTexture, texel: Int) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: rect.width * rect.height * texel)
        texture.getBytes(&out, bytesPerRow: rect.width * texel, from: rect.region, mipmapLevel: 0)
        return out
    }
}

extension SceneResult {
    /// The texture read back twice, by the blit engine and by a compute kernel, each against `model`: a scene whose
    /// two readbacks disagree has a broken readback, not a broken scene.
    mutating func compareBothWays(_ label: String, _ texture: MTLTexture, _ model: TexelImage, gpu: GPU) async throws {
        let format = model.format
        let blit = try await gpu.read(texture, bytesPerPixel: format.bytesPerTexel)
        compare(
            label + " blit", bytes: blit, expected: model.bytes, format: format, width: model.width,
            height: model.height, tolerance: format.step() * 1.01)
        guard !format.isDepthOrStencil, texture.usage.contains(.shaderRead) else { return }
        let shader = try await gpu.computeRaster(texture, format)
        compare(
            label + " shader", shader, format.raster(model.bytes, width: model.width, height: model.height),
            tolerance: format.step(near: 1) * 1.01)
    }
}
