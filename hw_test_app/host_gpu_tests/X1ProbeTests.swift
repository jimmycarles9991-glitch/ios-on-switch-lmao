import Foundation
import HWTestCore
import HWTestGPU
import Metal
import XCTest

/// The X1 probes that need only Metal, on this Mac's GPU: each must run to its finding, and its numbers are Apple's
/// answers for the probes whose reference is an Apple GPU (which E4 model, d17's second pass, the sRGB and unorm16
/// rules). Their verdicts here are INCONCLUSIVE, since nothing runs on the raw path.
final class X1ProbeTests: XCTestCase {
    private func makeGPU() throws -> GPU {
        let directory =
            (ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? "")
            + "hw_test_app/shaders"
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let queue = try XCTUnwrap(device.makeCommandQueue())
        let sources = try ["HWTest.metal", "X1Probes.metal"].map {
            try String(contentsOfFile: directory + "/" + $0, encoding: .utf8)
        }
        let options = MTLCompileOptions()
        options.fastMathEnabled = false
        let library = try device.makeLibrary(source: sources.joined(separator: "\n"), options: options)
        return GPU(device: device, queue: queue, library: library)
    }

    /// Runs `body` and checks that every count it judges by is zero on this Mac: its reference is Apple's.
    private func check(_ name: String, _ body: (GPU) async throws -> ProbeFinding) async throws {
        let finding = try await body(makeGPU())
        let fields = finding.metrics.ordered.map { "\($0.0)=\($0.1.logText)" }.joined(separator: " ")
        print("X1 \(name) \(finding.verdict.rawValue) \(fields) \(finding.note)")
        for (key, value) in finding.metrics.ordered where key.hasSuffix("_wrong") {
            XCTAssertEqual(value, .integer(0), "\(name) \(key)")
        }
    }

    func testMetalOnlyProbesMatchAppleOnThisMac() async throws {
        try await check("buffer_zero", X1MemoryProbes.bufferZero)
        try await check("linear_align", X1MemoryProbes.linearAlign)
        try await check("rop_conversion", X1ConversionProbes.rop)
        try await check("sampler_conversion", X1ConversionProbes.sampler)
        try await check("blend_exact", X1BlendProbes.exact)
        try await check("f32_filter", X1FilterProbes.f32Filter)
        try await check("mip_filter", X1FilterProbes.mipFilter)
        try await check("visibility", X1PassProbes.visibility)
    }
}
