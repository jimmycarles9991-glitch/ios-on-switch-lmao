import Foundation
import HWTestCore
import Metal

struct PlacementHeapScene: GPUScene {
    let id = "gpu.heap.placement"
    let name = "Placement heap offsets, overlapping resources and backing lifetime"

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        for storage in [MTLStorageMode.shared, .private] {
            let label = storage == .shared ? "shared" : "private"
            let options: MTLResourceOptions = storage == .shared ? .storageModeShared : .storageModePrivate
            let descriptor = MTLHeapDescriptor()
            descriptor.type = .placement
            descriptor.storageMode = storage
            descriptor.size = 4 << 20
            guard let heap = gpu.device.makeHeap(descriptor: descriptor) else {
                throw TestFailure("\(label) placement heap returned nil")
            }
            let unit = max(256, gpu.device.heapBufferSizeAndAlign(length: 1024, options: options).align)
            let offset = unit * 2, length = unit * 4
            guard let first = heap.makeBuffer(length: length, options: options, offset: offset),
                let twin = heap.makeBuffer(length: length, options: options, offset: offset),
                let middle = heap.makeBuffer(length: unit * 2, options: options, offset: offset + unit),
                let readback = gpu.device.makeBuffer(length: length, options: .storageModeShared)
            else { throw TestFailure("\(label) placement buffers returned nil") }
            result.expect(
                first.heapOffset == offset && twin.heapOffset == offset && middle.heapOffset == offset + unit,
                "\(label) buffer offsets were ignored")
            result.expect(first.isAliasable() && twin.isAliasable(), "\(label) placement buffers were not aliasable")
            if storage == .shared {
                first.contents().initializeMemory(as: UInt8.self, repeating: 0x5A, count: length)
                let aliasBytes = UnsafeBufferPointer(
                    start: twin.contents().assumingMemoryBound(to: UInt8.self), count: length)
                result.expect(aliasBytes.allSatisfy { $0 == 0x5A }, "shared buffer CPU addresses did not alias")
            }
            try await gpu.run { commands in
                let blit = commands.makeBlitCommandEncoder()!
                blit.fill(buffer: first, range: 0..<length, value: 0x3C)
                blit.endEncoding()
            }
            first.makeAliasable()
            try await gpu.run { commands in
                let blit = commands.makeBlitCommandEncoder()!
                blit.fill(buffer: middle, range: 0..<(unit * 2), value: 0x99)
                blit.endEncoding()
            }
            let timing = try await gpu.run { commands in
                let blit = commands.makeBlitCommandEncoder()!
                blit.copy(from: twin, sourceOffset: 0, to: readback, destinationOffset: 0, size: length)
                blit.endEncoding()
            }
            result.metrics.set("buffer_path_\(label)", timing.path)
            let bytes = Array(
                UnsafeBufferPointer(start: readback.contents().assumingMemoryBound(to: UInt8.self), count: length))
            let expected = (0..<length).map { UInt8($0 >= unit && $0 < unit * 3 ? 0x99 : 0x3C) }
            result.compare(
                "\(label) overlapping buffers", bytes: bytes, expected: expected,
                format: .r8Unorm, width: length, height: 1, tolerance: 0)

            let textureDescriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .rgba8Unorm, width: 61, height: 37, mipmapped: false)
            textureDescriptor.storageMode = storage
            textureDescriptor.usage = [.renderTarget, .shaderRead]
            let need = gpu.device.heapTextureSizeAndAlign(descriptor: textureDescriptor)
            let textureOffset = ((1 << 20) + need.align - 1) & ~(need.align - 1)
            var writer: MTLTexture? = heap.makeTexture(descriptor: textureDescriptor, offset: textureOffset)
            guard let reader = heap.makeTexture(descriptor: textureDescriptor, offset: textureOffset), writer != nil
            else { throw TestFailure("\(label) placement textures returned nil") }
            result.expect(
                writer!.heapOffset == textureOffset && reader.heapOffset == textureOffset,
                "\(label) texture offsets were ignored")
            result.expect(
                writer!.isAliasable() && reader.isAliasable(), "\(label) placement textures were not aliasable")
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = writer
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                pass.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1)
                commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
            }
            writer!.makeAliasable()
            writer = nil
            let image = try await gpu.read(reader, bytesPerPixel: 4)
            let expectedImage = Array(repeating: [UInt8](arrayLiteral: 64, 128, 191, 255), count: 61 * 37).flatMap {
                $0
            }
            result.compare(
                "\(label) aliased texture after writer release", bytes: image, expected: expectedImage,
                format: .rgba8Unorm, width: 61, height: 37, tolerance: 1.0 / 255)
            result.expect(
                heap.usedSize == 0 && heap.maxAvailableSize(alignment: 256) == 0,
                "\(label) placement heap reported automatic allocation occupancy")
        }
        return result
    }
}
