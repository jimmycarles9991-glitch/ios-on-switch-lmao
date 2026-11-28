import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// An R8Uint texel buffer of 2^27 + 1 texels, one past the device's maxTextureBufferWidth answer (NVK's 2^27), made by
/// the facade's debug switchMetalTexelBuffer:format:width: (TegraGM20BMetalDevice.mm; the texture header's width holds 32 bits),
/// over a buffer holding 0x5a at byte 0, 0xa5 at 2^27 - 1 and 0x3c at 2^27, read by a kernel (r5_read_texel_buffer).
/// v0 and v_last_in must be the bytes there, as any texel buffer reads its texels; v_past (the texel past 2^27) is the
/// finding, reported in reach_ok (1: the texture unit reaches it), not judged. SKIP where the facade has no such call.
struct TexelBufferReachTest: HWTest {
    let id = "gpu.gm20b.texel_buffer_reach"
    let name = "A texel buffer of 2^27 + 1 texels reads its texels, and the one past 2^27, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    typealias Make = @convention(c) (AnyObject, Selector, MTLBuffer, UInt, UInt) -> Unmanaged<AnyObject>?
    static let selector = NSSelectorFromString("switchMetalTexelBuffer:format:width:")
    static let width = (1 << 27) + 1
    static let marks: [(offset: Int, byte: UInt8)] = [(0, 0x5a), ((1 << 27) - 1, 0xa5), (1 << 27, 0x3c)]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        guard let facade = gpu.device as? NSObject, facade.responds(to: Self.selector) else {
            return .skip(GM20BFacade.missing("switchMetalTexelBuffer:format:width:", on: gpu.device))
        }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        do {
            let backing = try gpu.buffer(Self.width)
            for mark in Self.marks {
                backing.contents().storeBytes(of: mark.byte, toByteOffset: mark.offset, as: UInt8.self)
            }
            let made = autoreleasepool {
                unsafeBitCast(facade.method(for: Self.selector), to: Make.self)(
                    facade, Self.selector, backing, MTLPixelFormat.r8Uint.rawValue, UInt(Self.width)
                )?.takeUnretainedValue() as? MTLTexture
            }
            guard let texels = made else { throw TestFailure("the facade made no texel buffer of 2^27 + 1 texels") }
            let at = try gpu.buffer(Self.marks.map { UInt32($0.offset) })
            let out = try gpu.buffer(Self.marks.count * 4)
            let kernel = try gpu.compute("r5_read_texel_buffer")
            let path = try await gpu.run { commands in
                let e = commands.makeComputeCommandEncoder()!
                e.setComputePipelineState(kernel)
                e.setTexture(texels, index: 0)
                e.setBuffer(at, offset: 0, index: 0)
                e.setBuffer(out, offset: 0, index: 1)
                e.dispatchThreadgroups(
                    MTLSize(width: 1, height: 1, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: Self.marks.count, height: 1, depth: 1)
                )
                e.endEncoding()
            }.path
            let values = (0 ..< Self.marks.count).map {
                Int(out.contents().load(fromByteOffset: 4 * $0, as: UInt32.self))
            }
            for (key, value) in zip(["v0", "v_last_in", "v_past"], values) { metrics.set(key, value) }
            metrics.set("reach_ok", values[2] == Int(Self.marks[2].byte))
            metrics.set("path", path)
            checks.append(("texel 0 reads 0x5a", values[0] == Int(Self.marks[0].byte)))
            checks.append(("texel 2^27 - 1 reads 0xa5", values[1] == Int(Self.marks[1].byte)))
        } catch {
            checks.append(("the read: \(GPU.describe(error))", false))
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
