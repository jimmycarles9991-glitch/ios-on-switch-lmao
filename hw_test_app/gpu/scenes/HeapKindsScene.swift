import Foundation
import HWTestCore
import Metal

/// Every kind of texture a heap holds lies in the heap's own memory (raw::Device::arenaTexture): making 64 of a kind
/// grows none of the device's own (RawFacade.memory's pool), the GPU's writes to the first read back exactly, and a
/// command buffer that used the heap names as much with 4 of them as with 64 (the encode profile's uses and use_adds,
/// a debug runtime's). <kind>_cost: "constant" or the two counts; <kind>_own_bytes: the pool's growth; <kind>_read:
/// exact, wrong, or - for a kind read back no way the blit engine reads it.
struct HeapKindsScene: GPUScene {
    let id = "gpu.heap.kinds"
    let name = "Heap textures of every kind in the heap's own memory, named once however many"

    struct Kind {
        let label: String
        let descriptor: MTLTextureDescriptor
        let read: Read
    }

    enum Read { case texels, depth, none }

    static func descriptor(
        _ type: MTLTextureType, _ format: MTLPixelFormat, _ storage: MTLStorageMode, width: Int = 16, height: Int = 16,
        depth: Int = 1, layers: Int = 1, levels: Int = 1, samples: Int = 1, usage: MTLTextureUsage = [.shaderRead]
    ) -> MTLTextureDescriptor {
        let d = MTLTextureDescriptor()
        d.textureType = type
        d.pixelFormat = format
        d.width = width
        d.height = height
        d.depth = depth
        d.arrayLength = layers
        d.mipmapLevelCount = levels
        d.sampleCount = samples
        d.storageMode = storage
        d.usage = usage
        return d
    }

    static var kinds: [Kind] {
        var out: [Kind] = []
        for (storage, suffix) in [(MTLStorageMode.private, "private"), (.shared, "shared")] {
            out += [
                Kind(
                    label: "mips_\(suffix)",
                    descriptor: descriptor(
                        .type2D, .rgba8Unorm, storage, width: 32, height: 32,
                        levels: 3), read: .texels),
                Kind(
                    label: "array_\(suffix)", descriptor: descriptor(.type2DArray, .rgba8Unorm, storage, layers: 4),
                    read: .texels),
                Kind(
                    label: "volume_\(suffix)", descriptor: descriptor(.type3D, .rgba8Unorm, storage, depth: 4),
                    read: .texels),
            ]
        }
        let target: MTLTextureUsage = [.renderTarget, .shaderRead]
        return out + [
            Kind(label: "cube", descriptor: descriptor(.typeCube, .rgba8Unorm, .private), read: .texels),
            Kind(
                label: "line", descriptor: descriptor(.type1D, .rgba8Unorm, .private, width: 64, height: 1),
                read: .texels),
            Kind(
                label: "msaa",
                descriptor: descriptor(
                    .type2DMultisample, .rgba8Unorm, .private, samples: 4,
                    usage: .renderTarget), read: .none),
            Kind(label: "depth", descriptor: descriptor(.type2D, .depth32Float, .private, usage: target), read: .depth),
            Kind(
                label: "depth_stencil",
                descriptor: descriptor(
                    .type2D, .depth32Float_stencil8, .private,
                    usage: .renderTarget), read: .none),
            Kind(label: "bc1", descriptor: descriptor(.type2D, .bc1_rgba, .private), read: .none),
            Kind(label: "rgb9e5", descriptor: descriptor(.type2D, .rgb9e5Float, .private), read: .texels),
        ]
    }

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let raw = RawFacade.kind(of: gpu.device) == "raw"
        let target = try gpu.texture(.rgba8Unorm, 8, 8)
        for kind in Self.kinds {
            let need = gpu.device.heapTextureSizeAndAlign(descriptor: kind.descriptor)
            let heapDescriptor = MTLHeapDescriptor()
            heapDescriptor.storageMode = kind.descriptor.storageMode
            heapDescriptor.size = ((need.size + need.align - 1) & ~(need.align - 1)) * 64 + need.align
            guard let heap = gpu.device.makeHeap(descriptor: heapDescriptor) else {
                throw TestFailure("\(kind.label): makeHeap returned nil")
            }
            var grown = 0
            let make = { (count: Int) -> [MTLTexture] in
                let before = RawFacade.memory(of: gpu.device)["pool"] ?? 0
                let made = (0..<count).compactMap { _ in heap.makeTexture(descriptor: kind.descriptor) }
                grown += (RawFacade.memory(of: gpu.device)["pool"] ?? 0) - before
                return made
            }
            let few = make(4)
            let fewCost = try await cost(gpu, heap, target)
            let many = few + make(60)
            let manyCost = try await cost(gpu, heap, target)
            guard many.count == 64 else {
                if raw { result.expect(false, "\(kind.label): the heap made \(many.count) of 64") }
                result.notes.append("\(kind.label): the heap made \(many.count) of 64")
                continue
            }
            if raw {
                result.metrics.set("\(kind.label)_own_bytes", grown)
                result.expect(grown == 0, "\(kind.label): the device's own memory grew \(grown) bytes")
            }
            if fewCost.isEmpty || fewCost.values.allSatisfy({ $0 == 0 }) {
                result.metrics.set("\(kind.label)_cost", "unmeasured")
            } else {
                let same = fewCost == manyCost
                result.metrics.set(
                    "\(kind.label)_cost",
                    same
                        ? "constant"
                        : "\(fewCost)|\(manyCost)"
                            .replacingOccurrences(of: " ", with: ""))
                result.expect(same, "\(kind.label): a command buffer of 64 named more than one of 4")
            }
            let read = try await readBack(gpu, kind, many[0])
            result.metrics.set("\(kind.label)_read", read)
            result.expect(read != "wrong", "\(kind.label): the GPU's writes read back wrong")
        }
        return result
    }

    /// The encode profile's uses and use_adds of an empty pass that used the heap
    func cost(_ gpu: GPU, _ heap: MTLHeap, _ target: MTLTexture) async throws -> [String: Double] {
        let before = RawFacade.encodeProfile(of: gpu.device)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let render = commands.makeRenderCommandEncoder(descriptor: pass)!
            render.useHeap(heap, stages: .fragment)
            render.endEncoding()
        }
        let after = RawFacade.encodeProfile(of: gpu.device)
        return ["uses", "use_adds"].reduce(into: [:]) { $0[$1] = (after[$1] ?? 0) - (before[$1] ?? 0) }
    }

    /// exact or wrong: a blit's texels into slice 0, level 0 and back, or a pass's depth clear; - for another kind
    func readBack(_ gpu: GPU, _ kind: Kind, _ texture: MTLTexture) async throws -> String {
        switch kind.read {
        case .none:
            return "-"
        case .depth:
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.depthAttachment.texture = texture
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.storeAction = .store
                pass.depthAttachment.clearDepth = 0.25
                commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
            }
            let bytes = try await gpu.read(texture, bytesPerPixel: 4)
            let want = withUnsafeBytes(of: Float(0.25).bitPattern.littleEndian, Array.init)
            return bytes == Array([[UInt8]](repeating: want, count: bytes.count / 4).joined()) ? "exact" : "wrong"
        case .texels:
            let width = texture.width, height = texture.height
            let pattern = (0..<width * height * 4).map { UInt8(truncatingIfNeeded: $0 * 7 + 3) }
            let source = try gpu.buffer(pattern)
            try await gpu.run { commands in
                let blit = commands.makeBlitCommandEncoder()!
                blit.copy(
                    from: source, sourceOffset: 0, sourceBytesPerRow: width * 4,
                    sourceBytesPerImage: width * height * 4,
                    sourceSize: MTLSize(width: width, height: height, depth: 1), to: texture, destinationSlice: 0,
                    destinationLevel: 0, destinationOrigin: MTLOrigin())
                blit.endEncoding()
            }
            return try await gpu.read(texture, bytesPerPixel: 4) == pattern ? "exact" : "wrong"
        }
    }
}
