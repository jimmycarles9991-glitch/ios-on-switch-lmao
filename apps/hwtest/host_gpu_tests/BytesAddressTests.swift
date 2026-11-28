import Foundation
import HWTestCore
import HWTestGPU
import Metal
import XCTest

/// gpu.gm20b.bytes_address on this Mac's GPU: two setVertexBytes bindings have two addresses (magenta), one buffer bound
/// to both one (black)
final class BytesAddressTests: XCTestCase {
    func testTwoBytesBindingsHaveTwoAddresses() async throws {
        let directory =
            (ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? "") + "apps/hwtest/shaders"
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let source = try String(contentsOfFile: directory + "/BytesAddress.metal", encoding: .utf8)
        let gpu = GPU(
            device: device, queue: try XCTUnwrap(device.makeCommandQueue()),
            library: try await device.makeLibrary(source: source, options: MTLCompileOptions())
        )
        let bytes = try await BytesAddress.run(gpu)
        let same = try await BytesAddress.run(gpu, same: true)
        XCTAssertEqual(bytes.pixel, [255, 0, 255, 255])
        XCTAssertEqual(same.pixel, [0, 0, 0, 0], "one buffer, one address")
    }
}
