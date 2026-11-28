import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// makeAliasable on a texture of an automatic heap gives the heap back all the room the texture took: usedSize returns
/// to its value before the texture was made. An extended-range texture the driver samples is placed with an integer
/// stand-in beside it (gm20b::Device::arenaTexture), which makeAliasable must give back with it; a BGRA8 texture, which
/// has none, is the control. Per format, in a private heap of its own: <f>_used the bytes the texture made usedSize
/// take, <f>_left what makeAliasable left of them (0), <f>_size heapTextureSizeAndAlign's size.
struct HeapAliasableUsedTest: HWTest {
    let id = "gpu.heap.aliasable_used"
    let name = "makeAliasable on a heap texture gives the heap back all its room, an extended-range one's stand-in too"
    let subsystem = Subsystem.metal

    static let side = 64

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        let formats: [(String, MTLPixelFormat)] = [("bgr10_xr", .bgr10_xr), ("bgra8", .bgra8Unorm)]
        for (key, format) in formats {
            let heapDescriptor = MTLHeapDescriptor()
            heapDescriptor.size = 4 << 20
            heapDescriptor.storageMode = .private
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: format, width: Self.side, height: Self.side, mipmapped: false
            )
            descriptor.storageMode = .private
            descriptor.usage = [.renderTarget, .shaderRead]
            guard let heap = gpu.device.makeHeap(descriptor: heapDescriptor) else { throw TestFailure("no heap") }
            let empty = heap.usedSize
            guard let texture = heap.makeTexture(descriptor: descriptor) else {
                throw TestFailure("\(key): the heap made no texture")
            }
            let used = heap.usedSize - empty
            // alive past the read: its release gives its room back whether or not makeAliasable did
            let left: Int = withExtendedLifetime(texture) {
                texture.makeAliasable()
                return heap.usedSize - empty
            }
            metrics.set("\(key)_used", used)
            metrics.set("\(key)_left", left)
            metrics.set("\(key)_size", gpu.device.heapTextureSizeAndAlign(descriptor: descriptor).size)
            checks.append(("\(key): the texture took room", used > 0))
            checks.append(("\(key): makeAliasable gave all of it back", left == 0))
        }
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
