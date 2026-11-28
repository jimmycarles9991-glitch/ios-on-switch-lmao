import Foundation
import Metal
import XCTest

/// Which zero Apple's GPU gives for Metal's fmax, fmin and clamp at a tie of +0 and -0, half and float, in both
/// orders and both math modes. The shader compiler's GM20B form of a half's min and max (HSETP2 and SEL) answers
/// Metal's definition, y where x < y and x otherwise, which gives the first operand at a tie; this records what the
/// GPU Metal runs on gives, so the two can be compared.
final class SignedZeroTests: XCTestCase {
    private static let source = """
    #include <metal_stdlib>
    using namespace metal;
    kernel void ties(device const float2 *in [[buffer(0)]], device uint4 *out [[buffer(1)]],
                     uint i [[thread_position_in_grid]])
    {
        const float x = in[i].x, y = in[i].y;
        const half hx = half(x), hy = half(y);
        out[2 * i] = uint4(as_type<uint>(fmax(x, y)), as_type<uint>(fmin(x, y)),
                           as_type<uint>(clamp(x, y, 1.0f)), 0);
        out[2 * i + 1] = uint4(as_type<ushort>(fmax(hx, hy)), as_type<ushort>(fmin(hx, hy)),
                               as_type<ushort>(clamp(hx, hy, half(1))), 0);
    }
    """
    private static let pairs: [(Float, Float)] = [(-0.0, 0.0), (0.0, -0.0), (-0.0, -0.0), (0.0, 0.0)]

    private func answers(fastMath: Bool) throws -> [[UInt32]] {
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let options = MTLCompileOptions()
        options.mathMode = fastMath ? .fast : .safe
        let library = try device.makeLibrary(source: Self.source, options: options)
        let pipeline = try device.makeComputePipelineState(function: try XCTUnwrap(library.makeFunction(name: "ties")))
        let input = Self.pairs.flatMap { [$0.0, $0.1] }
        let inBuffer = try XCTUnwrap(device.makeBuffer(bytes: input, length: input.count * 4))
        let outBuffer = try XCTUnwrap(device.makeBuffer(length: Self.pairs.count * 32))
        let queue = try XCTUnwrap(device.makeCommandQueue())
        let commands = try XCTUnwrap(queue.makeCommandBuffer())
        let encoder = try XCTUnwrap(commands.makeComputeCommandEncoder())
        encoder.setComputePipelineState(pipeline)
        encoder.setBuffer(inBuffer, offset: 0, index: 0)
        encoder.setBuffer(outBuffer, offset: 0, index: 1)
        encoder.dispatchThreads(MTLSize(width: Self.pairs.count, height: 1, depth: 1),
                                threadsPerThreadgroup: MTLSize(width: Self.pairs.count, height: 1, depth: 1))
        encoder.endEncoding()
        commands.commit()
        commands.waitUntilCompleted()
        let words = outBuffer.contents().bindMemory(to: UInt32.self, capacity: Self.pairs.count * 8)
        return (0 ..< Self.pairs.count * 2).map { r in (0 ..< 3).map { words[4 * r + $0] } }
    }

    func testTiesOfSignedZeros() throws {
        for fast in [false, true] {
            let rows = try answers(fastMath: fast)
            var report = "math \(fast ? "fast" : "safe") on \(MTLCreateSystemDefaultDevice()?.name ?? "?")\n"
            for (k, (x, y)) in Self.pairs.enumerated() {
                let f = rows[2 * k], h = rows[2 * k + 1]
                let sign = { (w: UInt32, top: UInt32) in w & top != 0 ? "-0" : "+0" }
                report += "x \(x.sign == .minus ? "-0" : "+0") y \(y.sign == .minus ? "-0" : "+0"): "
                report += "float max \(sign(f[0], 0x8000_0000)) min \(sign(f[1], 0x8000_0000)) "
                report += "clamp \(sign(f[2], 0x8000_0000)); "
                report += "half max \(sign(h[0], 0x8000)) min \(sign(h[1], 0x8000)) clamp \(sign(h[2], 0x8000))\n"
                for w in f { XCTAssertEqual(w & 0x7FFF_FFFF, 0, report) }
                for w in h { XCTAssertEqual(w & 0x7FFF, 0, report) }
            }
            print(report)
        }
    }
}
