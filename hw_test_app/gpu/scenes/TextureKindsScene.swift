import Foundation
import HWTestCore
import Metal

/// Array slices, cube faces and 3D depth planes through the blit engine alone, raw's path: each slice, face or plane
/// is uploaded with its own pattern, one is copied into another texture, and every one is read back and compared
/// with its pattern byte for byte. A wrong slice or plane stride, or a face order, shows as another slice's bytes.
struct TextureKindsScene: GPUScene {
    let id = "gpu.texture.kinds"
    let name = "2D arrays, cubes and 3D textures through the blit engine"

    static let side = 32

    /// RGBA8 bytes of slice `index`: every texel of every slice differs.
    static func pattern(_ index: Int, side: Int = side) -> [UInt8] {
        (0..<(side * side)).flatMap { i -> [UInt8] in
            let (x, y) = (i % side, i / side)
            return [UInt8(x * 8 & 0xff), UInt8(y * 8 & 0xff), UInt8(index * 37 & 0xff), UInt8(255 - index)]
        }
    }

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        try await layered("array", .type2DArray, layers: 4, copied: 2, gpu, &result)
        try await layered("cube", .typeCube, layers: 6, copied: 4, gpu, &result)
        try await volume(gpu, &result)
        return result
    }

    private func layered(
        _ label: String, _ type: MTLTextureType, layers: Int, copied: Int, _ gpu: GPU, _ result: inout SceneResult
    ) async throws {
        let side = Self.side
        let descriptor = MTLTextureDescriptor()
        descriptor.textureType = type
        descriptor.pixelFormat = .rgba8Unorm
        descriptor.width = side
        descriptor.height = side
        descriptor.arrayLength = type == .typeCube ? 1 : layers
        descriptor.storageMode = .private
        descriptor.usage = [.shaderRead]
        guard let texture = gpu.device.makeTexture(descriptor: descriptor) else {
            result.expect(false, "\(label): makeTexture returned nil")
            return
        }
        let flat = try gpu.texture(.rgba8Unorm, side, side, usage: [.shaderRead])
        for layer in 0..<layers {
            try await gpu.upload(Self.pattern(layer), to: texture, bytesPerRow: side * 4, slice: layer)
        }
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: texture, sourceSlice: copied, sourceLevel: 0, sourceOrigin: MTLOrigin(),
                sourceSize: MTLSize(width: side, height: side, depth: 1), to: flat, destinationSlice: 0,
                destinationLevel: 0, destinationOrigin: MTLOrigin())
            blit.endEncoding()
        }
        for layer in 0..<layers {
            let got = try await gpu.read(texture, bytesPerPixel: 4, slice: layer)
            result.compare(
                "\(label) \(layer)", bytes: got, expected: Self.pattern(layer), format: .rgba8Unorm, width: side,
                height: side, tolerance: 0)
        }
        let got = try await gpu.read(flat, bytesPerPixel: 4)
        result.compare(
            "\(label) \(copied) copied to 2D", bytes: got, expected: Self.pattern(copied), format: .rgba8Unorm,
            width: side, height: side, tolerance: 0)
    }

    private func volume(_ gpu: GPU, _ result: inout SceneResult) async throws {
        let (side, depth, copied) = (Self.side, 4, 2)
        let descriptor = MTLTextureDescriptor()
        descriptor.textureType = .type3D
        descriptor.pixelFormat = .rgba8Unorm
        descriptor.width = side
        descriptor.height = side
        descriptor.depth = depth
        descriptor.storageMode = .private
        descriptor.usage = [.shaderRead]
        guard let texture = gpu.device.makeTexture(descriptor: descriptor) else {
            result.expect(false, "3D: makeTexture returned nil")
            return
        }
        let image = side * side * 4
        let staging = try gpu.buffer((0..<depth).flatMap { Self.pattern($0) })
        let out = try gpu.buffer(image * (depth + 1))
        let flat = try gpu.texture(.rgba8Unorm, side, side, usage: [.shaderRead])
        let size = MTLSize(width: side, height: side, depth: 1)
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: staging, sourceOffset: 0, sourceBytesPerRow: side * 4, sourceBytesPerImage: image,
                sourceSize: MTLSize(width: side, height: side, depth: depth), to: texture, destinationSlice: 0,
                destinationLevel: 0, destinationOrigin: MTLOrigin())
            blit.copy(
                from: texture, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: copied),
                sourceSize: size, to: flat, destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin())
            for plane in 0..<depth {
                blit.copy(
                    from: texture, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: plane),
                    sourceSize: size, to: out, destinationOffset: plane * image, destinationBytesPerRow: side * 4,
                    destinationBytesPerImage: image)
            }
            blit.copy(
                from: flat, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(), sourceSize: size, to: out,
                destinationOffset: depth * image, destinationBytesPerRow: side * 4, destinationBytesPerImage: image)
            blit.endEncoding()
        }
        let bytes = [UInt8](UnsafeRawBufferPointer(start: out.contents(), count: image * (depth + 1)))
        for plane in 0...depth {
            let want = Self.pattern(plane == depth ? copied : plane)
            let label = plane == depth ? "3D plane \(copied) copied to 2D" : "3D plane \(plane)"
            result.compare(
                label, bytes: Array(bytes[(plane * image)..<((plane + 1) * image)]), expected: want,
                format: .rgba8Unorm, width: side, height: side, tolerance: 0)
        }
    }
}
