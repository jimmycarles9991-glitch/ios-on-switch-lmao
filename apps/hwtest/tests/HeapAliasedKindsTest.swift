import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A placement heap's private texture (PTE kind 0xfe) and aliased private buffer (PTE kind 0) at offset 0: writing the
/// texture populates GPU L2 lines under kind 0xfe, then filling the aliased buffer in a non-host-written command buffer
/// (and within one command buffer behind an MTLFence) must switch kinds before writing, or the buffer's sectors land in
/// the other GOB order and read back scrambled.
struct HeapAliasedKindsTest: HWTest {
    let id = "gpu.heap.aliased_kinds"
    let name = "Placement heap buffer aliasing a block-linear texture reads back clean across PTE kinds"
    let subsystem = Subsystem.metal

    static let side = 64
    static let byteCount = side * side * 4
    static let stripe = 32

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let pattern = (0 ..< Self.byteCount).map { UInt8(truncatingIfNeeded: $0 * 13 + ($0 >> 8) * 37 + 1) }
        let staging = try gpu.buffer(pattern)

        let crossWrong = try await runCrossCommandBuffer(gpu, staging: staging)
        let sameWrong = try await runSameCommandBuffer(gpu, staging: staging)

        var metrics = Metrics()
        metrics.set("cross_cb_wrong_bytes", crossWrong)
        metrics.set("same_cb_wrong_bytes", sameWrong)
        return Outcome.judged(
            [
                ("aliased buffer filled in next command buffer matches (\(crossWrong) wrong)", crossWrong == 0),
                ("aliased buffer filled after fence in same CB matches (\(sameWrong) wrong)", sameWrong == 0),
            ],
            metrics,
            proof: gpu.proof
        ).onRawPath(gpu)
    }

    private func makeAliasedPair(_ gpu: GPU) throws -> (MTLHeap, MTLTexture, MTLBuffer) {
        let heapDescriptor = MTLHeapDescriptor()
        heapDescriptor.type = .placement
        heapDescriptor.storageMode = .private
        heapDescriptor.size = 1 << 20
        guard let heap = gpu.device.makeHeap(descriptor: heapDescriptor) else {
            throw TestFailure("placement heap returned nil")
        }
        let textureDescriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba8Unorm, width: Self.side, height: Self.side, mipmapped: false
        )
        textureDescriptor.storageMode = .private
        textureDescriptor.usage = [.shaderRead]
        guard let texture = heap.makeTexture(descriptor: textureDescriptor, offset: 0),
              let buffer = heap.makeBuffer(length: Self.byteCount, options: .storageModePrivate, offset: 0)
        else {
            throw TestFailure("placement heap aliased resources returned nil")
        }
        return (heap, texture, buffer)
    }

    private func runCrossCommandBuffer(_ gpu: GPU, staging: MTLBuffer) async throws -> Int {
        let (heap, texture, aliased) = try makeAliasedPair(gpu)
        let readback = try gpu.buffer(Self.byteCount)
        memset(readback.contents(), 0, Self.byteCount)

        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: staging, sourceOffset: 0, sourceBytesPerRow: Self.side * 4,
                sourceBytesPerImage: Self.byteCount,
                sourceSize: MTLSize(width: Self.side, height: Self.side, depth: 1),
                to: texture, destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin()
            )
            blit.endEncoding()
        }

        var expected = [UInt8](repeating: 0, count: Self.byteCount)
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            for off in stride(from: 0, to: Self.byteCount, by: Self.stripe) {
                let value = UInt8(0x11 + ((off / Self.stripe) * 7 + (off / 256) * 29) % 223)
                for i in off ..< (off + Self.stripe) {
                    expected[i] = value
                }
                blit.fill(buffer: aliased, range: off ..< (off + Self.stripe), value: value)
            }
            blit.endEncoding()
        }

        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(from: aliased, sourceOffset: 0, to: readback, destinationOffset: 0, size: Self.byteCount)
            blit.endEncoding()
        }

        return withExtendedLifetime(heap) {
            Self.wrongBytes(readback, expected: expected)
        }
    }

    private func runSameCommandBuffer(_ gpu: GPU, staging: MTLBuffer) async throws -> Int {
        let (heap, texture, aliased) = try makeAliasedPair(gpu)
        let readback = try gpu.buffer(Self.byteCount)
        memset(readback.contents(), 0, Self.byteCount)
        guard let fence = gpu.device.makeFence() else { throw TestFailure("makeFence returned nil") }

        var expected = [UInt8](repeating: 0, count: Self.byteCount)
        try await gpu.run { commands in
            let first = commands.makeBlitCommandEncoder()!
            first.copy(
                from: staging, sourceOffset: 0, sourceBytesPerRow: Self.side * 4,
                sourceBytesPerImage: Self.byteCount,
                sourceSize: MTLSize(width: Self.side, height: Self.side, depth: 1),
                to: texture, destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin()
            )
            first.updateFence(fence)
            first.endEncoding()

            let second = commands.makeBlitCommandEncoder()!
            second.waitForFence(fence)
            for off in stride(from: 0, to: Self.byteCount, by: Self.stripe) {
                let value = UInt8(0x21 + ((off / Self.stripe) * 5 + (off / 256) * 19) % 211)
                for i in off ..< (off + Self.stripe) {
                    expected[i] = value
                }
                second.fill(buffer: aliased, range: off ..< (off + Self.stripe), value: value)
            }
            second.endEncoding()
        }

        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(from: aliased, sourceOffset: 0, to: readback, destinationOffset: 0, size: Self.byteCount)
            blit.endEncoding()
        }

        return withExtendedLifetime(heap) {
            Self.wrongBytes(readback, expected: expected)
        }
    }

    private static func wrongBytes(_ buffer: MTLBuffer, expected: [UInt8]) -> Int {
        let got = UnsafeBufferPointer(
            start: buffer.contents().assumingMemoryBound(to: UInt8.self), count: expected.count
        )
        var wrong = 0
        for i in 0 ..< expected.count where got[i] != expected[i] {
            wrong += 1
        }
        return wrong
    }
}
