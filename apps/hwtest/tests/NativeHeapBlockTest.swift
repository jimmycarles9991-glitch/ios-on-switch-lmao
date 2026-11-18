import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A heap's block-compressed textures, a mip chain and a 2D array in a private heap and one level in a shared one
/// (drivers/TegraGM20BMetal/raw/runtime/RawMetalHeap.mm heapTextureAsk, raw::TextureAsk::Block): their size is the
/// device's own answer and each is made in the heap by the raw path (an object of raw's), no m2v device asked.
struct NativeHeapBlockTest: HWTest {
    let id = "metal.native.heap_block"
    let name = "A heap's block-compressed textures placed in it, with no m2v"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let needsBefore = RawFacade.m2vRecord(of: gpu.device)["needs"] as? [String: Any] ?? [:]
        func heap(_ mode: MTLStorageMode) throws -> MTLHeap {
            let descriptor = MTLHeapDescriptor()
            descriptor.storageMode = mode
            descriptor.size = 4 << 20
            guard let made = gpu.device.makeHeap(descriptor: descriptor) else { throw TestFailure("no heap") }
            return made
        }
        let privateHeap = try heap(.private), sharedHeap = try heap(.shared)
        let mips = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .astc_4x4_ldr, width: 100, height: 60, mipmapped: true)
        let layers = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .etc2_rgb8, width: 64, height: 64, mipmapped: false)
        layers.textureType = .type2DArray
        layers.arrayLength = 3
        let single = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .astc_4x4_ldr, width: 64, height: 64, mipmapped: false)
        mips.storageMode = .private
        layers.storageMode = .private
        single.storageMode = .shared
        var metrics = Metrics()
        var wrong: [String] = []
        for (name, descriptor, heap) in [
            ("mips", mips, privateHeap), ("array", layers, privateHeap), ("shared", single, sharedHeap),
        ] {
            let need = gpu.device.heapTextureSizeAndAlign(descriptor: descriptor)
            metrics.set("\(name)_size", need.size)
            metrics.set("\(name)_align", need.align)
            guard let texture = heap.makeTexture(descriptor: descriptor) else {
                wrong.append("\(name)_made")
                continue
            }
            let made = NativeObject.name(texture)
            metrics.set("\(name)_class", made ?? "-")
            if need.size == 0 { wrong.append("\(name)_size") }
            if texture.heap !== heap { wrong.append("\(name)_heap") }
            if made == nil { wrong.append("\(name)_not_raw") }
        }
        let needsAfter = RawFacade.m2vRecord(of: gpu.device)["needs"] as? [String: Any] ?? [:]
        let asked = Set(needsAfter.keys).subtracting(needsBefore.keys).sorted()
        metrics.set("m2v_needs_asked", asked.isEmpty ? "-" : asked.joined(separator: ","))
        if !asked.isEmpty { wrong.append("m2v") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
