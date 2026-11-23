import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A kernel's store of (0.5, 0.25, -0.25, 0.75) into a 64-texel texture buffer of each extended-range format over a
/// zeroed shared buffer (shaders/StandInFormats.metal xr_texel_buffer_store) leaves an M4 Max's words in every texel and
/// nothing past them. The words are the M4 Max's (m2v-textures4 probes/tbstore.mm, and this test's case code on it):
/// 552 0x80004000 0xbf809fc0, 553 0xa4801b80 0xbf80bdc0, 554 0xe7f80100, 555 0xef7a486e. w552_lo= w552_hi= w553_lo=
/// w553_hi= w554= w555= texel 0's words in hex, off= texels of the four buffers off Apple's words, past= words past the
/// texels that changed; every command buffer raw, failed_cbs=0.
struct XrTexelBufferStoresTest: HWTest {
    let id = "gpu.gm20b.xr_texel_buffer_stores"
    let name = "A kernel's stores into extended-range texture buffers leave an M4 Max's words, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    static let value = SIMD4<Float>(0.5, 0.25, -0.25, 0.75)
    static let texels = 64
    static let apple: [(format: UInt, words: [UInt32])] = [
        (552, [0x8000_4000, 0xbf80_9fc0]), (553, [0xa480_1b80, 0xbf80_bdc0]), (554, [0xe7f8_0100]),
        (555, [0xef7a_486e]),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        var off = 0, past = 0
        for arm in Self.apple {
            do {
                let stored = try await Self.stored(gpu, arm.format, wordsPerTexel: arm.words.count)
                for (key, word) in Self.keys(arm.format, stored.first) { metrics.set(key, word) }
                let judged = Self.judged(stored.words, arm.words)
                off += judged.off
                past += judged.past
                checks.append(("\(arm.format): every texel Apple's words, nothing past", judged == (0, 0)))
            } catch {
                checks.append(("\(arm.format): \(GPU.describe(error))", false))
            }
        }
        metrics.set("off", off)
        metrics.set("past", past)
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        metrics.fitLog(within: 720, keeping: ["w552_lo", "w552_hi", "w553_lo", "w553_hi", "w554", "w555", "failed_cbs"])
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func keys(_ format: UInt, _ words: [UInt32]) -> [(String, String)] {
        let hex = words.map { String(format: "%08x", $0) }
        return hex.count == 2 ? [("w\(format)_lo", hex[0]), ("w\(format)_hi", hex[1])] : [("w\(format)", hex[0])]
    }

    static func judged(_ words: [UInt32], _ apple: [UInt32]) -> (off: Int, past: Int) {
        let stored = texels * apple.count
        let off = stride(from: 0, to: stored, by: apple.count).filter { Array(words[$0 ..< $0 + apple.count]) != apple }
        return (off.count, words[stored...].filter { $0 != 0 }.count)
    }

    static func stored(
        _ gpu: GPU, _ format: UInt, wordsPerTexel: Int
    ) async throws -> (
        words: [UInt32], first: [UInt32]
    ) {
        let count = 2 * texels * wordsPerTexel
        let buffer = try gpu.buffer([UInt32](repeating: 0, count: count))
        let descriptor = MTLTextureDescriptor.textureBufferDescriptor(
            with: MTLPixelFormat(rawValue: format)!, width: texels, resourceOptions: .storageModeShared,
            usage: [.shaderRead, .shaderWrite]
        )
        guard
            let texture = buffer.makeTexture(
                descriptor: descriptor, offset: 0, bytesPerRow: texels * 4 * wordsPerTexel
            )
        else { throw TestFailure("no \(format) texture buffer") }
        let pipeline = try gpu.compute("xr_texel_buffer_store")
        var value = Self.value
        try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(texture, index: 0)
            encoder.setBytes(&value, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: texels, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1)
            )
            encoder.endEncoding()
        }
        let words = Array(
            UnsafeBufferPointer(start: buffer.contents().bindMemory(to: UInt32.self, capacity: count), count: count)
        )
        return (words, Array(words[0 ..< wordsPerTexel]))
    }
}
