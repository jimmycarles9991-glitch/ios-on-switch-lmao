import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import HWTestVideo
import IOSurface
import Metal

struct MetalBlitTest: HWTest {
    let id = "metal.blit"
    let name = "Blit: copies, fill and mipmaps"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let size = 64
        let source = (0..<(size * size * 4)).map { UInt8(truncatingIfNeeded: $0 * 7 + $0 / 256) }
        let first = try gpu.texture(.rgba8Unorm, size, size, usage: [.shaderRead])
        let second = try gpu.texture(.rgba8Unorm, size, size, usage: [.shaderRead])
        try await gpu.upload(source, to: first, bytesPerRow: size * 4)
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: first, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 8, y: 4, z: 0),
                sourceSize: MTLSize(width: 32, height: 16, depth: 1), to: second, destinationSlice: 0,
                destinationLevel: 0,
                destinationOrigin: MTLOrigin(x: 16, y: 40, z: 0))
            blit.endEncoding()
        }
        let roundTrip = try await gpu.read(first, bytesPerPixel: 4)
        let copied = try await gpu.read(second, bytesPerPixel: 4)
        var regionWrong = 0
        for y in 0..<16 {
            for x in 0..<32 where copied.pixel(16 + x, 40 + y, width: size) != source.pixel(8 + x, 4 + y, width: size) {
                regionWrong += 1
            }
        }

        let fill = try gpu.buffer(1 << 20)
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.fill(buffer: fill, range: 4096..<(1 << 20) - 4096, value: 0xA5)
            blit.endEncoding()
        }
        let filled = UnsafeRawBufferPointer(start: fill.contents(), count: 1 << 20)
        let fillWrong = (0..<(1 << 20)).filter { filled[$0] != (($0 >= 4096 && $0 < (1 << 20) - 4096) ? 0xA5 : 0) }
            .count

        // Left half black, right half white: every mip level averages to mid grey.
        let mipped = try gpu.texture(.rgba8Unorm, size, size, usage: [.shaderRead], mipmaps: 7)
        let halves = (0..<(size * size)).flatMap { index -> [UInt8] in
            index % size < size / 2 ? [0, 0, 0, 255] : [255, 255, 255, 255]
        }
        try await gpu.upload(halves, to: mipped, bytesPerRow: size * 4)
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.generateMipmaps(for: mipped)
            blit.endEncoding()
        }
        let smallest = try await gpu.read(mipped, bytesPerPixel: 4, level: 6)

        var metrics = Metrics()
        metrics.set("region_wrong", regionWrong)
        metrics.set("fill_wrong", fillWrong)
        metrics.set("mip_1x1", Int(smallest[0]))
        return .judged(
            [
                ("buffer -> texture -> buffer exact", roundTrip == source),
                ("texture region copy exact", regionWrong == 0),
                ("fillBuffer exact, range respected", fillWrong == 0),
                ("generateMipmaps averages to grey", abs(Int(smallest[0]) - 128) <= 1 && smallest[3] == 255),
            ], metrics, proof: gpu.proof)
    }
}

struct MetalTextureViewTest: HWTest {
    let id = "metal.texture_view"
    let name = "Texture views: format, mip level and slice"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let size = 16
        let bytes = (0..<(size * size * 4)).map { UInt8(truncatingIfNeeded: $0 * 13 + 1) }
        let texture = try gpu.texture(.rgba8Unorm, size, size, usage: [.shaderRead, .pixelFormatView])
        try await gpu.upload(bytes, to: texture, bytesPerRow: size * 4)
        guard let asUInt = texture.makeTextureView(pixelFormat: .r32Uint) else {
            return .fail("r32Uint view returned nil")
        }
        let words = try gpu.buffer(size * size * 4)
        let readUInt = try gpu.compute("read_uint_view")
        try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(readUInt)
            encoder.setTexture(asUInt, index: 0)
            encoder.setBuffer(words, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: size, height: size, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1))
            encoder.endEncoding()
        }
        let read = [UInt8](UnsafeRawBufferPointer(start: words.contents(), count: size * size * 4))

        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba8Unorm, width: size, height: size, mipmapped: true)
        descriptor.textureType = .type2DArray
        descriptor.arrayLength = 2
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .private
        guard let array = gpu.device.makeTexture(descriptor: descriptor) else {
            return .fail("2D array texture returned nil")
        }
        let level1 = [UInt8](repeating: 0, count: 8 * 8 * 4).enumerated().map {
            UInt8(truncatingIfNeeded: $0.offset * 3)
        }
        try await gpu.upload([UInt8](repeating: 9, count: 8 * 8 * 4), to: array, bytesPerRow: 8 * 4, level: 1, slice: 0)
        try await gpu.upload(level1, to: array, bytesPerRow: 8 * 4, level: 1, slice: 1)
        guard
            let view = array.makeTextureView(
                pixelFormat: .rgba8Unorm, textureType: .type2D, levels: 1..<2, slices: 1..<2)
        else { return .fail("level/slice view returned nil") }
        let floats = try gpu.buffer(8 * 8 * 16)
        let readFloat = try gpu.compute("read_float_texture")
        try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(readFloat)
            encoder.setTexture(view, index: 0)
            encoder.setBuffer(floats, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: 8, height: 8, depth: 1), threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1))
            encoder.endEncoding()
        }
        let texels = floats.contents().bindMemory(to: SIMD4<Float>.self, capacity: 64)
        let viewWrong = (0..<64).filter { index in
            (0..<4).contains { Int((texels[index][$0] * 255).rounded()) != Int(level1[index * 4 + $0]) }
        }.count
        var metrics = Metrics()
        metrics.set("view_wrong", viewWrong)
        return .judged(
            [
                ("r32Uint view reads the rgba8 bytes", read == bytes),
                ("mip 1 / slice 1 view reads its own data", viewWrong == 0),
            ],
            metrics, proof: gpu.proof)
    }
}

/// Zero-copy between the CPU and GPU through IOSurface, both ways, and the CVMetalTextureCache path video apps use
/// for NV12.
struct MetalIOSurfaceTest: HWTest {
    let id = "metal.iosurface"
    let name = "IOSurface-backed textures, both directions"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let size = 128
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: size, .height: size, .bytesPerElement: 4, .pixelFormat: kCVPixelFormatType_32BGRA,
        ]
        guard let surface = IOSurface(properties: properties) else { return .fail("IOSurface creation failed") }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: size, height: size, mipmapped: false)
        descriptor.usage = [.renderTarget, .shaderRead]
        descriptor.storageMode = .shared
        guard
            let texture = gpu.device.makeTexture(
                descriptor: descriptor, iosurface: unsafeBitCast(surface, to: IOSurfaceRef.self), plane: 0)
        else { return .fail("makeTexture(iosurface:) returned nil") }

        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        surface.lock(options: .readOnly, seed: nil)
        let cpuSees = Array(
            UnsafeRawBufferPointer(start: surface.baseAddress + (size / 2) * surface.bytesPerRow + size * 2, count: 4))
        surface.unlock(options: .readOnly, seed: nil)

        surface.lock(options: [], seed: nil)
        for y in 0..<size {
            let row = surface.baseAddress + y * surface.bytesPerRow
            for x in 0..<size {
                row.storeBytes(
                    of: UInt32(0xFF00_0000 | (y << 16) | (x << 8) | 0x40), toByteOffset: x * 4, as: UInt32.self)
            }
        }
        surface.unlock(options: [], seed: nil)
        let gpuSees = try await readTexels(gpu, texture)
        let surfaceWrong = (0..<(size * size)).filter { index in
            let (x, y) = (index % size, index / size)
            return unorm8(gpuSees[index]) != [y, x, 0x40, 255]
        }.count

        let nv12Wrong = try await checkNV12(gpu)
        var metrics = Metrics()
        metrics.set("gpu_to_cpu", cpuSees.map(String.init).joined(separator: ","))
        metrics.set("cpu_to_gpu_wrong", surfaceWrong)
        metrics.set("nv12_wrong", nv12Wrong)
        return .judged(
            [
                ("GPU clear visible to the CPU", cpuSees == [153, 102, 51, 255]),
                ("CPU writes visible to the GPU", surfaceWrong == 0),
                ("NV12 planes through CVMetalTextureCache", nv12Wrong == 0),
            ], metrics, proof: gpu.proof)
    }

    func unorm8(_ texel: SIMD4<Float>) -> [Int] { (0..<4).map { Int((texel[$0] * 255).rounded()) } }

    func readTexels(_ gpu: GPU, _ texture: MTLTexture) async throws -> [SIMD4<Float>] {
        let count = texture.width * texture.height
        let out = try gpu.buffer(count * 16)
        let pipeline = try gpu.compute("read_float_texture")
        try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(texture, index: 0)
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: texture.width, height: texture.height, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1))
            encoder.endEncoding()
        }
        return Array(
            UnsafeBufferPointer(start: out.contents().bindMemory(to: SIMD4<Float>.self, capacity: count), count: count))
    }

    /// The pattern in an NV12 CVPixelBuffer, read on the GPU through r8 and rg8 plane textures; every luma and chroma
    /// sample must match the buffer's own bytes.
    func checkNV12(_ gpu: GPU) async throws -> Int {
        let pattern = Pattern(width: 256, height: 128, clip: 0x4E56)
        let pixels = try PatternPixels.make(pattern, frame: 3, bitDepth: 8, pool: nil)
        var cache: CVMetalTextureCache?
        guard CVMetalTextureCacheCreate(nil, nil, gpu.device, nil, &cache) == kCVReturnSuccess, let cache else {
            throw TestFailure("CVMetalTextureCacheCreate failed")
        }
        var wrong = 0
        for (plane, format, width, height) in [(0, MTLPixelFormat.r8Unorm, 256, 128), (1, .rg8Unorm, 128, 64)] {
            var cvTexture: CVMetalTexture?
            let status = CVMetalTextureCacheCreateTextureFromImage(
                nil, cache, pixels, nil, format, width, height, plane, &cvTexture)
            guard status == kCVReturnSuccess, let cvTexture, let texture = CVMetalTextureGetTexture(cvTexture) else {
                throw TestFailure("plane \(plane) texture: \(status)")
            }
            let texels = try await readTexels(gpu, texture)
            CVPixelBufferLockBaseAddress(pixels, .readOnly)
            let base = CVPixelBufferGetBaseAddressOfPlane(pixels, plane)!
            let rowBytes = CVPixelBufferGetBytesPerRowOfPlane(pixels, plane)
            for y in 0..<height {
                for x in 0..<width {
                    let texel = texels[y * width + x]
                    let channels = plane == 0 ? 1 : 2
                    for c in 0..<channels
                    where unorm8(texel)[c]
                        != Int(base.load(fromByteOffset: y * rowBytes + x * channels + c, as: UInt8.self))
                    {
                        wrong += 1
                    }
                }
            }
            CVPixelBufferUnlockBaseAddress(pixels, .readOnly)
        }
        return wrong
    }
}
