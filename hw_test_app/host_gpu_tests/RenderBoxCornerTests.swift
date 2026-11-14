import Foundation
import HWTestCore
import HWTestGPU
import Metal
import XCTest

/// gpu.raw.icon_corners' GPU arms on this Mac's GPU, whose Metal is Apple's: RenderBox's icon masks at the App
/// Library's small and large sizes within RenderBoxCorner.tolerance of its own CPU drawing in every corner pixel, as
/// antialiased as it, a stepped corner beyond it; the fetching indexed strips' sums; and the reduction the
/// IconServices arms compare through.
final class RenderBoxCornerTests: XCTestCase {
    private func makeGPU() throws -> GPU {
        let directory =
            (ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? "")
            + "hw_test_app/shaders"
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let queue = try XCTUnwrap(device.makeCommandQueue())
        let sources = try ["HWTest.metal", "FramebufferFetch.metal"].map {
            try String(contentsOfFile: directory + "/" + $0, encoding: .utf8)
        }
        let library = try device.makeLibrary(source: sources.joined(separator: "\n"), options: MTLCompileOptions())
        return GPU(device: device, queue: queue, library: library)
    }

    func testFetchingIndexedStripsSumEachPixelOnce() async throws {
        for cell in try await FetchStrip.cells(makeGPU()) {
            print("icon_corners \(cell.name) path=\(cell.path) wrong=\(cell.wrong)")
            XCTAssertEqual(cell.wrong, 0, cell.name)
        }
    }

    func testMasksMatchRenderBoxsCPUDrawing() async throws {
        try XCTSkipUnless(RenderBoxCorner.available, "no RenderBox")
        let gpu = try makeGPU()
        for (side, radius) in [(54, 12.1), (136, 30.4)] {
            for storage in [MTLStorageMode.private, .shared] {
                let cell = try await RenderBoxCorner.cell(gpu, side: side, radius: radius, storage: storage)
                print(
                    "icon_corners rb\(side)_\(cell.storage) beyond=\(cell.beyond) max_diff=\(cell.maxDiff) "
                        + "partial=\(cell.partial)/\(cell.referencePartial)")
                XCTAssertEqual(cell.beyond, 0, "\(side) \(cell.storage)")
                XCTAssertGreaterThan(cell.referencePartial, 0)
                XCTAssertGreaterThanOrEqual(cell.partial * 4, cell.referencePartial * 3, "\(side) \(cell.storage)")
            }
        }
    }

    func testASteppedCornerIsBeyondTheTolerance() {
        let side = 54, radius = 12.1
        let smooth = RenderBoxCorner.continuousMask(side: side, radius: radius)
        let stepped = smooth.map { $0 >= 128 ? UInt8(255) : 0 }
        let cell = RenderBoxCorner.judge(stepped, smooth, side: side, radius: radius)
        XCTAssertGreaterThan(cell.beyond, 0)
        XCTAssertEqual(cell.partial, 0)
        XCTAssertEqual(RenderBoxCorner.judge(smooth, smooth, side: side, radius: radius).beyond, 0)
    }

    func testReductionAveragesTheCoveredArea() {
        var big = [Double](repeating: 0, count: 10 * 10)
        for y in 0..<10 { for x in 0..<5 { big[y * 10 + x] = 1 } }
        let small = RenderBoxCorner.reduce(big, from: 10, to: 4)
        XCTAssertEqual(small[0], 1, accuracy: 1e-9)
        XCTAssertEqual(small[1], 1, accuracy: 1e-9)
        XCTAssertEqual(small[2], 0, accuracy: 1e-9)
        XCTAssertEqual(small[3], 0, accuracy: 1e-9)
        let third = RenderBoxCorner.reduce(big, from: 10, to: 3)
        XCTAssertEqual(third[1], 0.5, accuracy: 1e-9)
        XCTAssertEqual(RenderBoxCorner.cornerPixels(side: 10, corner: 2).count, 16)
    }
}
