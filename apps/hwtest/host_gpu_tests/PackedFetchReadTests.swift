import Foundation
import HWTestCore
import HWTestGPU
import Metal
import XCTest

/// The read m2v's storage form gives a 16-bit packed target's loaded texel (fetch/packed.rs half_tile), measured on this
/// Mac's GPU: every texel of B5G6R5Unorm and BGR5A1Unorm, fetched as float4 and as half4, is each channel's nearest
/// half, a missing alpha 1; most are not the exact quotient, so the run tells the two apart.
final class PackedFetchReadTests: XCTestCase {
    func testEveryTexelReadsTheNearestHalf() async throws {
        let directory =
            (ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? "") + "apps/hwtest/shaders"
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let source = try ["HWTest.metal", "PackedFetchRead.metal"]
            .map { try String(contentsOfFile: directory + "/" + $0, encoding: .utf8) }.joined(separator: "\n")
        let gpu = GPU(
            device: device, queue: try XCTUnwrap(device.makeCommandQueue()),
            library: try await device.makeLibrary(source: source, options: MTLCompileOptions())
        )
        for format in PackedFetchRead.formats {
            for half in [false, true] {
                let r = try await PackedFetchRead.run(gpu, format, half: half)
                let arm = "\(format.name) \(half ? "half4" : "float4")"
                XCTAssertEqual(r.offHalf, 0, "\(arm): first \(r.first)")
                XCTAssertGreaterThan(r.offExact, 60000, "\(arm): the exact quotient would pass too")
            }
        }
    }
}
