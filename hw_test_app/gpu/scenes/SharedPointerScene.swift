import Darwin
import Foundation
import HWTestCore
import Metal

/// Shared cached memory must contain the GPU's completed writes even when the caller never asks for contents again.
struct SharedPointerScene: GPUScene {
    let id = "gpu.shared.retained_pointer"
    let name = "GPU writes visible through retained shared and no-copy pointers"

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let length = max(Int(getpagesize()), 16384)
        var allocation: UnsafeMutableRawPointer?
        guard posix_memalign(&allocation, Int(getpagesize()), length) == 0, let original = allocation else {
            throw TestFailure("aligned no-copy allocation failed")
        }
        original.initializeMemory(as: UInt8.self, repeating: 0xa5, count: length)
        guard
            let source = gpu.device.makeBuffer(
                bytesNoCopy: original, length: length, options: .storageModeShared,
                deallocator: { pointer, _ in free(pointer) })
        else {
            free(original)
            throw TestFailure("no-copy buffer creation failed")
        }
        let destination = try gpu.buffer(length)
        // Both CPU mappings are populated before GPU writes. Neither pointer is refreshed after completion.
        let retained = destination.contents()
        retained.initializeMemory(as: UInt8.self, repeating: 0x33, count: length)
        let filled = try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.fill(buffer: source, range: 0..<length, value: 0x5a)
            blit.endEncoding()
        }
        var wrong = 0
        for offset in 0..<length where original.load(fromByteOffset: offset, as: UInt8.self) != 0x5a {
            wrong += 1
        }
        result.metrics.set("fill_wrong_bytes", wrong)
        result.expect(wrong == 0, "completed GPU fill left \(wrong) stale bytes through original pointer")
        // A partial CPU update must preserve the GPU's other bytes, including bytes sharing this cache line.
        original.storeBytes(of: UInt8(0xc7), toByteOffset: 67, as: UInt8.self)
        let copied = try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(from: source, sourceOffset: 0, to: destination, destinationOffset: 0, size: length)
            blit.endEncoding()
        }
        wrong = 0
        for offset in 0..<length {
            let wanted: UInt8 = offset == 67 ? 0xc7 : 0x5a
            if retained.load(fromByteOffset: offset, as: UInt8.self) != wanted { wrong += 1 }
        }
        result.metrics.set("partial_copy_wrong_bytes", wrong)
        result.expect(wrong == 0, "partial CPU update and GPU copy changed \(wrong) bytes")
        result.metrics.set("paths", "\(filled.path),\(copied.path)")
        return result
    }
}
