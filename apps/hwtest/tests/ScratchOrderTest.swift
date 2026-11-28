import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUModels
import HWTestKit
import Metal

/// Passes over one memoryless attachment's scratch memory open without a drain between them (gpu/ScratchOrder.swift):
/// the driver rests their order on the ROP keeping write order on the same memory across a change of target, which
/// nothing else on the device shows. Per scratch format (BGRA8Unorm, RGBA16Float), `rounds` command buffers of pass A
/// (722x1282: a seed's pattern into colour 1, fetched into colour 0) then pass B (1792x1472 over the same scratch:
/// colour 1 cleared, fetched into colour 0). A's colour 0 is the pattern as the CPU computes it and B's every texel the
/// clear as an M4 Max stores it (`<format>_a_off`, `<format>_b_off` 0, texels over all rounds); a B that fetched what A
/// left in the scratch, or an A that fetched a stale round's, reads other texels.
struct ScratchOrderTest: HWTest {
    let id = "gpu.gm20b.scratch_order"
    let name = "Passes over one memoryless scratch keep their order with no drain between them"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let rounds = 8
    /// Pass B's colour 0 on an M4 Max: the clear (0.25, 0.5, 0.75, 1) fetched and stored as BGRA8 bytes
    static let clearStored: [UInt8] = [191, 128, 64, 255]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for (label, format) in [("bgra8", MTLPixelFormat.bgra8Unorm), ("rgba16f", .rgba16Float)] {
            let read = try await ScratchOrder.run(gpu, scratch: format, rounds: Self.rounds)
            let off = Self.off(read)
            metrics.set(label + "_a_off", off.a)
            metrics.set(label + "_b_off", off.b)
            checks.append(("\(label): \(off.a) of pass A's texels not its pattern", off.a == 0))
            checks.append(("\(label): \(off.b) of pass B's texels not its clear", off.b == 0))
            checks.append(("\(label): every command buffer raw", read.paths.allSatisfy { $0 == "gm20b" }))
        }
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// Texels off over every round: pass A's against the pattern of its seed, pass B's against `clearStored`
    static func off(_ read: ScratchOrder.Read) -> (a: Int, b: Int) {
        let clearWord = ByteCompare.word(clearStored[0], clearStored[1], clearStored[2], clearStored[3])
        var a = 0, b = 0
        for (round, bytes) in read.a.enumerated() {
            a += ScratchOrderCheck.wrongA(
                bytes, width: ScratchOrder.a.width, height: ScratchOrder.a.height, seed: UInt32(round + 1)
            )
        }
        for bytes in read.b {
            b += ScratchOrderCheck.wrongB(bytes, clearWord: clearWord)
        }
        return (a, b)
    }
}
