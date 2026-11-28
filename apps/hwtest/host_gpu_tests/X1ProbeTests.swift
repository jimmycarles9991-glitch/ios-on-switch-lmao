import Foundation
import HWTestCore
import HWTestGPU
import HWTestX1
import Metal
import XCTest

/// The X1 probes that need only Metal, on this Mac's GPU: each must run to its finding, and its numbers are Apple's
/// answers for the probes whose reference is an Apple GPU (which E4 model, d17's second pass, the sRGB and unorm16
/// rules). Their verdicts here are INCONCLUSIVE, since nothing runs on the driver.
final class X1ProbeTests: XCTestCase {
    private func makeGPU() throws -> GPU {
        let directory =
            (ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? "")
            + "apps/hwtest/shaders"
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

    /// Runs `body` and checks that every count it judges by is zero on this Mac: its reference is Apple's. A count's
    /// <prefix>_flushed part is what Apple's G13 does and G16 does not (X1ConversionProbes.flushedSubnormal).
    private func check(_ name: String, _ body: (GPU) async throws -> ProbeFinding) async throws {
        let gpu = try makeGPU()
        let host = HostFacts.fields(gpu.device)
        let finding = try await body(gpu)
        let fields = finding.metrics.ordered.map { "\($0.0)=\($0.1.logText)" }.joined(separator: " ")
        print("X1 \(name) \(host) \(finding.verdict.rawValue) \(fields) \(finding.note)")
        for (key, value) in finding.metrics.ordered where key.hasSuffix("_wrong") {
            let flushed = finding.metrics[String(key.dropLast("_wrong".count)) + "_flushed"] ?? .integer(0)
            XCTAssertEqual(value, flushed, "\(name) \(key) on \(host)")
        }
    }

    func testOnlyAZeroForASubnormalHalfCountsAsFlushed() {
        XCTAssertTrue(X1ConversionProbes.flushedSubnormal(0, nearest: 0x0100))
        XCTAssertTrue(X1ConversionProbes.flushedSubnormal(0, nearest: 0x0300))
        XCTAssertFalse(X1ConversionProbes.flushedSubnormal(0x00FF, nearest: 0x0100))
        XCTAssertFalse(X1ConversionProbes.flushedSubnormal(0x8000, nearest: 0x0100))
        XCTAssertFalse(X1ConversionProbes.flushedSubnormal(0, nearest: 0x0400))
        XCTAssertFalse(X1ConversionProbes.flushedSubnormal(0x3BFE, nearest: 0x3BFD))
        XCTAssertFalse(X1ConversionProbes.flushedSubnormal(0, nearest: 0))
    }

    func testMetalOnlyProbesMatchAppleOnThisMac() async throws {
        try await check("buffer_zero", X1MemoryProbes.bufferZero)
        try await check("linear_align", X1MemoryProbes.linearAlign)
        try await check("rop_conversion", X1ConversionProbes.rop)
        try await check("sampler_conversion", X1ConversionProbes.sampler)
        try await check("blend_exact", X1BlendProbes.exact)
        try await check("f32_filter", X1FilterProbes.f32Filter)
        try await check("mip_filter", X1FilterProbes.mipFilter)
        try await check("filter_weight_bits", X1FilterProbes.filterWeights)
        try await check("rop_unorm10_rounding", X1Unorm10Probe.rounding)
        try await check("visibility", X1PassProbes.visibility)
    }
}
