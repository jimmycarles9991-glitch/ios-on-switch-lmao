import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Block-compressed textures of several levels or layers, and shared ones, in the device's own memory: NIL's image
/// (fx::blockChainLayout). A shared one's CPU writes (replaceRegion) are read back by the copy engine and by getBytes; a
/// private one's level and slice are written and read by the copy engine. ETC2 and EAC are GM20B's own texture formats
/// (NIL's MaxwellA rows), as ASTC is: a known ETC2 block is sampled and every texel checked.
struct NativeBlockTexturesTest: NativeObjectTest {
    let id = "metal.native.block_textures"
    let name = "Block-compressed textures of levels and layers, shared or private, every block written and read back"

    struct Shape {
        let name: String
        let format: MTLPixelFormat
        let blockBytes: Int
        let type: MTLTextureType
        let width: Int, height: Int, layers: Int, levels: Int
        let storage: MTLStorageMode
    }

    static let shapes: [Shape] = [
        Shape(
            name: "astc_shared", format: .astc_4x4_ldr, blockBytes: 16, type: .type2D, width: 64, height: 40, layers: 1,
            levels: 5, storage: .shared),
        Shape(
            name: "etc2_shared_array", format: .etc2_rgb8, blockBytes: 8, type: .type2DArray, width: 32, height: 32,
            layers: 3, levels: 1, storage: .shared),
        Shape(
            name: "eac_private", format: .eac_rgba8, blockBytes: 16, type: .type2DArray, width: 32, height: 16,
            layers: 2, levels: 3, storage: .private),
        Shape(
            name: "astc_private_array", format: .astc_4x4_ldr, blockBytes: 16, type: .type2DArray, width: 64,
            height: 64, layers: 2, levels: 4, storage: .private),
    ]

    /// Every byte of level `level`, slice `slice`: distinct across the texture's images
    static func blocks(_ s: Shape, _ level: Int, _ slice: Int) -> (bytes: [UInt8], row: Int, rows: Int) {
        let across = (max(1, s.width >> level) + 3) / 4, down = (max(1, s.height >> level) + 3) / 4
        let row = across * s.blockBytes
        let bytes = (0..<(row * down)).map { UInt8(truncatingIfNeeded: $0 * 7 + level * 31 + slice * 61 + 1) }
        return (bytes, row, down)
    }

    /// An ETC2 RGB8 block in individual mode with every base colour 0x8 (0x88 expanded), table 0 and every index 0: each
    /// texel is 0x88 + 2 = 138 in red, green and blue (ETC1's modifier +2), alpha 255. Sampled by the texture unit, not
    /// the copy engine: GM20B's own ETC2 decode.
    static let etcBlock: [UInt8] = [0x88, 0x88, 0x88, 0, 0, 0, 0, 0]

    static func decodeWrong(_ gpu: GPU, paths: inout [String]) async throws -> Int {
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .etc2_rgb8, width: 4, height: 4, mipmapped: false)
        d.usage = .shaderRead
        d.storageMode = .shared
        guard let t = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no ETC2 RGB8 texture") }
        Self.etcBlock.withUnsafeBytes {
            t.replace(
                region: MTLRegionMake2D(0, 0, 4, 4), mipmapLevel: 0, slice: 0, withBytes: $0.baseAddress!,
                bytesPerRow: 8, bytesPerImage: 0)
        }
        let out = try gpu.buffer(16 * 16)
        let pipeline = try gpu.compute("sample_texture")
        let timing = try await gpu.run { commands in
            let e = commands.makeComputeCommandEncoder()!
            e.setComputePipelineState(pipeline)
            e.setTexture(t, index: 0)
            e.setBuffer(out, offset: 0, index: 0)
            e.dispatchThreads(
                MTLSize(width: 4, height: 4, depth: 1), threadsPerThreadgroup: MTLSize(width: 4, height: 4, depth: 1))
            e.endEncoding()
        }
        paths.append(timing.path)
        let texels = UnsafeBufferPointer(
            start: out.contents().bindMemory(to: SIMD4<Float>.self, capacity: 16), count: 16)
        return texels.filter { [$0.x, $0.y, $0.z, $0.w].map { Int(($0 * 255).rounded()) } != [138, 138, 138, 255] }
            .count
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        var work = NativeWork()
        let decoded = try await Self.decodeWrong(gpu, paths: &work.paths)
        metrics.set("wrong_etc2_decode", decoded)
        if decoded != 0 { work.failures.append("\(decoded) ETC2 texels the texture unit decodes otherwise") }
        for s in Self.shapes {
            let d = MTLTextureDescriptor()
            d.textureType = s.type
            d.pixelFormat = s.format
            d.width = s.width
            d.height = s.height
            d.arrayLength = s.layers
            d.mipmapLevelCount = s.levels
            d.usage = .shaderRead
            d.storageMode = s.storage
            guard let t = gpu.device.makeTexture(descriptor: d) else {
                work.failures.append("no \(s.name) texture")
                continue
            }
            work.objects.append(t)
            var wrong = 0
            for level in 0..<s.levels {
                for slice in 0..<s.layers {
                    let (bytes, row, rows) = Self.blocks(s, level, slice)
                    let size = MTLSize(width: max(1, s.width >> level), height: max(1, s.height >> level), depth: 1)
                    if s.storage == .shared {
                        bytes.withUnsafeBytes {
                            t.replace(
                                region: MTLRegion(origin: MTLOrigin(), size: size), mipmapLevel: level, slice: slice,
                                withBytes: $0.baseAddress!, bytesPerRow: row, bytesPerImage: 0)
                        }
                        var back = [UInt8](repeating: 0, count: bytes.count)
                        back.withUnsafeMutableBytes {
                            t.getBytes(
                                $0.baseAddress!, bytesPerRow: row, bytesPerImage: 0,
                                from: MTLRegion(origin: MTLOrigin(), size: size), mipmapLevel: level, slice: slice)
                        }
                        wrong += zip(back, bytes).filter { $0 != $1 }.count
                    } else {
                        let staging = try gpu.buffer(bytes)
                        let timing = try await gpu.run { commands in
                            let blit = commands.makeBlitCommandEncoder()!
                            blit.copy(
                                from: staging, sourceOffset: 0, sourceBytesPerRow: row, sourceBytesPerImage: 0,
                                sourceSize: size, to: t, destinationSlice: slice, destinationLevel: level,
                                destinationOrigin: MTLOrigin())
                            blit.endEncoding()
                        }
                        work.paths.append(timing.path)
                    }
                    let out = try gpu.buffer(row * rows)
                    let timing = try await gpu.run { commands in
                        let blit = commands.makeBlitCommandEncoder()!
                        blit.copy(
                            from: t, sourceSlice: slice, sourceLevel: level, sourceOrigin: MTLOrigin(),
                            sourceSize: size,
                            to: out, destinationOffset: 0, destinationBytesPerRow: row, destinationBytesPerImage: 0)
                        blit.endEncoding()
                    }
                    work.paths.append(timing.path)
                    let got = [UInt8](UnsafeRawBufferPointer(start: out.contents(), count: row * rows))
                    wrong += zip(got, bytes).filter { $0 != $1 }.count
                }
            }
            metrics.set("wrong_\(s.name)", wrong)
            if wrong != 0 { work.failures.append("\(wrong) bytes of the \(s.name) texture") }
        }
        return work
    }
}
