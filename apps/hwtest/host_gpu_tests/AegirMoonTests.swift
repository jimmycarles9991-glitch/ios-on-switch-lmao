import Foundation
import HWTestCore
import HWTestGPU
import Metal
import XCTest

/// gpu.gm20b.aegir_moon on this Mac's GPU: Aegir's frame holds the bytes the device test judges against
final class AegirMoonTests: XCTestCase {
    func testTheMoonFrameHoldsMetalsBytes() async throws {
        let directory =
            (ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? "") + "apps/hwtest/shaders"
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let source = try String(contentsOfFile: directory + "/AegirMoon.metal", encoding: .utf8)
        let gpu = GPU(
            device: device, queue: try XCTUnwrap(device.makeCommandQueue()),
            library: try await device.makeLibrary(source: source, options: MTLCompileOptions())
        )
        let reading = try await AegirMoon.run(gpu)
        let samples = AegirMoon.samples(reading).map { "\($0)=\($1)" }.joined(separator: " ")
        print("aegir_moon on \(device.name): \(samples)")
        for (check, off) in AegirMoon.misses(reading) {
            XCTAssertEqual(off, 0, "\(check): \(samples)")
        }
    }
}
