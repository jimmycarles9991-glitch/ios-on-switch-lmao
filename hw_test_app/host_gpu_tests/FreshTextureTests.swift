import Foundation
import HWTestCore
import HWTestGPU
import Metal
import XCTest

/// gpu.texture.fresh on this Mac's GPU: the first contents the raw path writes are the ones Apple's GPUs read.
final class FreshTextureTests: XCTestCase {
    private func makeGPU() throws -> GPU {
        let directory =
            (ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? "") + "hw_test_app/shaders"
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let queue = try XCTUnwrap(device.makeCommandQueue())
        let sources = try ["HWTest.metal", "Scenes.metal", "FramebufferFetch.metal"].map {
            try String(contentsOfFile: directory + "/" + $0, encoding: .utf8)
        }
        let library = try device.makeLibrary(source: sources.joined(separator: "\n"), options: MTLCompileOptions())
        return GPU(device: device, queue: queue, library: library)
    }

    func testNewPrivateTexturesReadWhatAppleGPUsRead() async throws {
        let gpu = try makeGPU()
        let scene = try XCTUnwrap(Scenes.all.first { $0.id == "gpu.texture.fresh" })
        let result = try await scene.run(gpu)
        XCTAssertTrue(result.passed, result.reasons(limit: 8).joined(separator: "; "))
    }
}
