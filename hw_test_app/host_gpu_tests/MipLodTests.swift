import Foundation
import HWTestCore
import HWTestGPU
import Metal
import XCTest

/// gpu.raw.mip_lod's references on this Mac's GPU, whose sampling is Metal's: every level arm within the tolerance of
/// the formula, every icon draw within MipLod.iconTolerance of the CPU's reference.
final class MipLodTests: XCTestCase {
    private func makeGPU() throws -> GPU {
        let directory =
            (ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? "")
            + "hw_test_app/shaders"
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let queue = try XCTUnwrap(device.makeCommandQueue())
        let sources = try ["HWTest.metal", "Scenes.metal", "MipLod.metal"].map {
            try String(contentsOfFile: directory + "/" + $0, encoding: .utf8)
        }
        let library = try device.makeLibrary(source: sources.joined(separator: "\n"), options: MTLCompileOptions())
        return GPU(device: device, queue: queue, library: library)
    }

    func testLevelArmsFollowMetalsFormula() async throws {
        let levels = try await MipLod.levels(makeGPU())
        print("mip_lod \(levels.fields.joined(separator: ","))")
        XCTAssertEqual(levels.wrong, [])
    }

    func testIconDrawsMatchTheReference() async throws {
        let icon = try await MipLod.icon(makeGPU())
        for cell in icon.cells {
            print("mip_lod icon_\(cell.side) beyond=\(cell.beyond) max_diff=\(cell.maxDiff)")
            XCTAssertEqual(cell.beyond, 0, "icon \(cell.side)")
        }
    }
}
