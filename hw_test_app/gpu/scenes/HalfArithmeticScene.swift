import Foundation
import HWTestCore
import Metal

/// Half sums and products of every finite half, bit for bit against IEEE half arithmetic on the CPU: subnormal results
/// kept, zeros signed, overflow to infinity. On the Switch the kernel is m2v's, compiled by NAK: halves read from memory
/// go through float arithmetic rounded to half, and halves that are themselves results of half arithmetic through the
/// GPU's packed half ALU (HADD2, HMUL2), so both are checked.
struct HalfArithmeticScene: GPUScene {
    let id = "gpu.compute.half_alu"
    let name = "Half sums and products, bit exact"

    static let forms = ["sum", "product", "sum_of_results", "product_of_results"]

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
        let (a, b) = Self.pairs()
        let out = try gpu.buffer(a.count * 4 * 2)
        let pipeline = try gpu.compute("half_arithmetic")
        try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setBuffer(try gpu.buffer(a), offset: 0, index: 0)
            encoder.setBuffer(try gpu.buffer(b), offset: 0, index: 1)
            encoder.setBuffer(try gpu.buffer([Float16(1)]), offset: 0, index: 2)
            encoder.setBuffer(out, offset: 0, index: 3)
            encoder.dispatchThreads(
                MTLSize(width: a.count / 4, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(
                    width: min(256, pipeline.maxTotalThreadsPerThreadgroup), height: 1, depth: 1))
            encoder.endEncoding()
        }
        let got = UnsafeBufferPointer(
            start: out.contents().bindMemory(to: UInt16.self, capacity: a.count * 4), count: a.count * 4)
        var wrong = [Int](repeating: 0, count: Self.forms.count)
        var first: [String] = []
        for i in 0..<a.count {
            let x = Float16(bitPattern: a[i]), y = Float16(bitPattern: b[i])
            let sum = (x + y).bitPattern, product = (x * y).bitPattern
            for form in Self.forms.indices {
                let want = form % 2 == 0 ? sum : product
                let have = got[((i / 4) * 4 + form) * 4 + i % 4]
                guard have != want else { continue }
                wrong[form] += 1
                if first.count < 4 {
                    first.append("\(Self.forms[form]) \(hex(a[i])) \(hex(b[i])): \(hex(have)), want \(hex(want))")
                }
            }
        }
        result.metrics.set("pairs", a.count)
        for (form, count) in zip(Self.forms, wrong) { result.metrics.set(form + "_wrong", count) }
        result.expect(first.isEmpty, "\(wrong.reduce(0, +)) wrong: " + first.joined(separator: "; "))
        return result
    }

    private func hex(_ bits: UInt16) -> String { String(bits, radix: 16) }
}
