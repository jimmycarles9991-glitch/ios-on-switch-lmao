import Foundation
import HWTestCore
import HWTestGPU
import Metal
import XCTest

/// The mip HW Tests' texel layouts on this Mac's GPU: a format's last field is the bits Metal reads as alpha wherever it
/// has alpha, so every field is sliced where Metal keeps it.
final class MipFormatTests: XCTestCase {
    private func makeGPU() throws -> GPU {
        let directory =
            (ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? "")
            + "hw_test_app/shaders"
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let queue = try XCTUnwrap(device.makeCommandQueue())
        let source = try String(contentsOfFile: directory + "/HWTest.metal", encoding: .utf8)
        let library = try device.makeLibrary(source: source, options: MTLCompileOptions())
        return GPU(device: device, queue: queue, library: library)
    }

    private static let withAlpha: Set<String> = [
        "bgra8", "rgba8", "rgba16f", "bgra8srgb", "rgba8srgb", "bgr5a1", "rgb10a2", "bgr10a2", "rgba16", "rgba32f",
    ]

    func testTheLastFieldIsAlpha() async throws {
        let gpu = try makeGPU()
        for format in (MipFormats.twod + MipFormats.candidates) where Self.withAlpha.contains(format.name) {
            let layout = format.layout
            var bytes = [UInt8](repeating: 0, count: layout.bytes)
            let last = layout.fields.last!
            layout.put(layout.code(of: 1, last), &bytes, texel: 0, field: last)
            let texture = try gpu.texture(format.pixel, 1, 1, usage: [.shaderRead], storage: .shared)
            texture.replace(
                region: MTLRegionMake2D(0, 0, 1, 1), mipmapLevel: 0, withBytes: bytes, bytesPerRow: layout.bytes)
            let texel = try await gpu.texels(texture)[0]
            XCTAssertEqual(texel.w, 1, "\(format.name) alpha")
            XCTAssertEqual(texel.x + texel.y + texel.z, 0, "\(format.name) colour")
        }
    }
}
