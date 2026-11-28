import Foundation
import HWTestCore
import HWTestGPU
import Metal
import XCTest

/// Apple's answer for ARGB8Unorm_sRGB (gpu/Argb8SrgbForms.swift), on this Mac's GPU. Every field 0 means m2v can hold
/// format 85 as R8G8B8A8_SRGB under ARGB8Unorm's rotated view (metal/format/mod.rs formats_named_not_held); the line it
/// prints is the measurement.
final class Argb8SrgbFormTests: XCTestCase {
    private func makeGPU() throws -> GPU {
        let directory =
            (ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? "") + "apps/hwtest/shaders"
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let queue = try XCTUnwrap(device.makeCommandQueue())
        let source = try String(contentsOfFile: directory + "/HWTest.metal", encoding: .utf8)
        let library = try device.makeLibrary(source: source, options: MTLCompileOptions())
        return GPU(device: device, queue: queue, library: library)
    }

    func testArgb8SrgbIsRgba8SrgbRotated() async throws {
        let gpu = try makeGPU()
        let r = try await Argb8SrgbForms.measure(gpu)
        print("argb8_srgb_forms on \(gpu.device.name): \(r.line)")
        XCTAssertEqual([r.readsOff, r.linearOff, r.passWritesOff, r.kernelWritesOff], [0, 0, 0, 0], r.line)
    }
}
