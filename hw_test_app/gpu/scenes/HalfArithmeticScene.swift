import Foundation
import HWTestCore
import Metal

/// Finite half inputs keep NaN payload differences out of the bitwise oracle. FMA also covers cancellation after an
/// overflowing product, where separately rounded multiply/add cannot produce the same result.
struct HalfArithmeticScene: GPUScene {
    let id = "gpu.compute.half_alu"
    let name = "Half arithmetic and fused FMA, bit exact"

    static let forms = ["sum", "product", "sum_of_results", "product_of_results", "fma"]

    static let fusedEdges: [(UInt16, UInt16, UInt16)] = [
        (0x8000, 0x3C00, 0x8000), (0, 0x3C00, 0x8000),
        (1, 0x3C00, 0), (0x8001, 0x3C00, 0x8000),
        (0x7BFF, 0x4000, 0xFBFF), (0xFBFF, 0x4000, 0x7BFF),
        (0x7BFF, 0x4000, 0), (0xFBFF, 0x4000, 0x8000),
        (0x3C01, 0x3BFE, 0xBC00), (1, 0x3800, 0),
        (0x8001, 0x3800, 0x8000), (3, 0x3800, 0),
    ]

    /// Every finite half three times: against a scattering of all of them, against its own negation one unit off
    /// (cancellation down to subnormals and zeros), and against the subnormals and the first normal binade.
    static func pairs() -> (a: [UInt16], b: [UInt16]) {
        let finite = (0...UInt16.max).filter { $0 & 0x7C00 != 0x7C00 }
        let n = finite.count
        var a: [UInt16] = [], b: [UInt16] = []
        a.reserveCapacity(3 * n)
        b.reserveCapacity(3 * n)
        for i in 0..<n {
            a.append(finite[i])
            b.append(finite[(i * 40503) % n])
        }
        for i in 0..<n {
            a.append(finite[i])
            b.append(finite[i] ^ 0x8001)
        }
        for i in 0..<n {
            a.append(finite[i])
            b.append(finite[(i * 977) % 2048] | (i & 1 == 0 ? 0 : 0x8000))
        }
        return (a, b)
    }

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        var (a, b) = Self.pairs()
        var c = a.indices.map { a[($0 * 7919) % a.count] }
        for i in 0..<1024 {
            let (x, y, z) = Self.fusedEdges[i % Self.fusedEdges.count]
            a.append(x)
            b.append(y)
            c.append(z)
        }
        let out = try gpu.buffer(a.count * Self.forms.count * 2)
        let pipeline = try gpu.compute("half_arithmetic")
        let timing = try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setBuffer(try gpu.buffer(a), offset: 0, index: 0)
            encoder.setBuffer(try gpu.buffer(b), offset: 0, index: 1)
            encoder.setBuffer(try gpu.buffer([Float16(1)]), offset: 0, index: 2)
            encoder.setBuffer(out, offset: 0, index: 3)
            encoder.setBuffer(try gpu.buffer(c), offset: 0, index: 4)
            encoder.dispatchThreads(
                MTLSize(width: a.count / 4, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(
                    width: min(256, pipeline.maxTotalThreadsPerThreadgroup), height: 1, depth: 1))
            encoder.endEncoding()
        }
        let got = UnsafeBufferPointer(
            start: out.contents().bindMemory(to: UInt16.self, capacity: a.count * Self.forms.count),
            count: a.count * Self.forms.count)
        var wrong = [Int](repeating: 0, count: Self.forms.count)
        var first: [String] = []
        var fusedWitnesses = 0
        for i in 0..<a.count {
            let x = Float16(bitPattern: a[i]), y = Float16(bitPattern: b[i]), z = Float16(bitPattern: c[i])
            let sum = (x + y).bitPattern, product = (x * y).bitPattern
            let fused = z.addingProduct(x, y).bitPattern
            if fused != ((x * y) + z).bitPattern { fusedWitnesses += 1 }
            let expected = [sum, product, sum, product, fused]
            for form in Self.forms.indices {
                let want = expected[form]
                let have = got[((i / 4) * Self.forms.count + form) * 4 + i % 4]
                guard have != want else { continue }
                wrong[form] += 1
                if first.count < 4 {
                    first.append(
                        "\(Self.forms[form]) \(hex(a[i])) \(hex(b[i])) \(hex(c[i])): \(hex(have)), want \(hex(want))")
                }
            }
        }
        result.metrics.set("pairs", a.count)
        result.metrics.set("path", timing.path)
        if !RawFacade.counters(of: gpu.device).isEmpty {
            result.expect(timing.path == "raw", "Half arithmetic left raw: \(timing.path)")
        }
        result.metrics.set("fma_separate_rounding_witnesses", fusedWitnesses)
        for (form, count) in zip(Self.forms, wrong) { result.metrics.set(form + "_wrong", count) }
        result.expect(fusedWitnesses > 0, "FMA inputs must distinguish fused and separately rounded arithmetic")
        result.expect(first.isEmpty, "\(wrong.reduce(0, +)) wrong: " + first.joined(separator: "; "))
        return result
    }

    private func hex(_ bits: UInt16) -> String { String(bits, radix: 16) }
}
