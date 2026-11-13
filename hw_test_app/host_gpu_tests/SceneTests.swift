import Foundation
import HWTestCore
import HWTestGPU
import Metal
import XCTest

/// Every GPU scene on this Mac's GPU, the shaders compiled from their sources: a scene whose CPU reference is wrong
/// fails here before it ever reaches the Switch.
final class SceneTests: XCTestCase {
    private func makeGPU(strictMath: Bool = false) throws -> GPU {
        let directory =
            (ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? "")
            + "hw_test_app/shaders"
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let queue = try XCTUnwrap(device.makeCommandQueue())
        let sources = try ["HWTest.metal", "Scenes.metal", "FramebufferFetch.metal"].map {
            try String(contentsOfFile: directory + "/" + $0, encoding: .utf8)
        }
        let options = MTLCompileOptions()
        options.fastMathEnabled = !strictMath
        let library = try device.makeLibrary(source: sources.joined(separator: "\n"), options: options)
        return GPU(device: device, queue: queue, library: library)
    }

    func testEverySceneMatchesItsReferenceOnThisMac() async throws {
        let gpu = try makeGPU()
        for scene in Scenes.all {
            let result = try await scene.run(gpu)
            XCTAssertTrue(result.passed, "\(scene.id): \(result.reasons(limit: 4).joined(separator: "; "))")
        }
    }

    func testFramebufferFetchOnThisMac() async throws {
        let scene = try XCTUnwrap(Scenes.all.first { $0.id == "gpu.raw.framebuffer_fetch" })
        let result = try await scene.run(makeGPU())
        XCTAssertTrue(result.passed, result.reasons(limit: 8).joined(separator: "; "))
        XCTAssertEqual(result.checks.count, 56)
    }

    func testFramebufferFetchDepthOnThisMac() async throws {
        let gpu = try makeGPU()
        let scene = try XCTUnwrap(Scenes.all.first { $0.id == "gpu.raw.framebuffer_fetch_depth" })
        let result = try await scene.run(gpu)
        XCTAssertTrue(result.passed, result.reasons(limit: 8).joined(separator: "; "))
        XCTAssertEqual(result.checks.count, [1, 2, 4, 8].filter(gpu.device.supportsTextureSampleCount).count * 4)
    }

    func testFetchBlendOrderOnThisMac() async throws {
        let scene = try XCTUnwrap(Scenes.all.first { $0.id == "gpu.raw.fetch_blend_order" })
        let result = try await scene.run(makeGPU())
        XCTAssertTrue(result.passed, result.reasons(limit: 8).joined(separator: "; "))
        XCTAssertEqual(result.checks.count, 29)
    }

    func testMSAAResolveIntoLevelsSlicesAndPlanesOnThisMac() async throws {
        let scene = try XCTUnwrap(Scenes.all.first { $0.id == "gpu.msaa.resolve" })
        let result = try await scene.run(makeGPU())
        XCTAssertTrue(result.passed, result.reasons(limit: 8).joined(separator: "; "))
        XCTAssertEqual(result.checks.count, 2 * 2 * 23, "two formats, resolved and drawn, 23 subresources each")
    }

    func testArchiveMissOnThisMac() async throws {
        let scene = try XCTUnwrap(Scenes.all.first { $0.id == "gpu.raw.archive_miss" })
        let result = try await scene.run(makeGPU())
        XCTAssertTrue(result.passed, result.reasons(limit: 8).joined(separator: "; "))
    }

    func testStoredMSAAOnThisMac() async throws {
        let gpu = try makeGPU()
        let scene = try XCTUnwrap(Scenes.all.first { $0.id == "gpu.raw.stored_msaa" })
        let result = try await scene.run(gpu)
        XCTAssertTrue(result.passed, result.reasons(limit: 8).joined(separator: "; "))
        let perCount = gpu.device.areProgrammableSamplePositionsSupported ? 19 : 18
        XCTAssertEqual(result.checks.count, [2, 4].filter(gpu.device.supportsTextureSampleCount).count * perCount)
    }

    func testPassFusionOnThisMac() async throws {
        let scene = try XCTUnwrap(Scenes.all.first { $0.id == "gpu.raw.pass_fusion" })
        let result = try await scene.run(makeGPU())
        XCTAssertTrue(result.passed, result.reasons(limit: 8).joined(separator: "; "))
        XCTAssertEqual(result.checks.count, 12)
    }

    func testSM53ProbeControlOnThisMac() async throws {
        let scene = try XCTUnwrap(Scenes.all.first { $0.id == "gpu.sm53_probe" })
        let result = try await scene.run(makeGPU(strictMath: true))
        XCTAssertTrue(result.passed, result.reasons(limit: 8).joined(separator: "; "))
    }

    func testHalfArithmeticOnThisMac() async throws {
        let scene = try XCTUnwrap(Scenes.all.first { $0.id == "gpu.compute.half_alu" })
        let result = try await scene.run(makeGPU(strictMath: true))
        XCTAssertTrue(result.passed, result.reasons(limit: 8).joined(separator: "; "))
    }

    func testSM53CandidateSemantics() {
        XCTAssertEqual(SM53Reference.flushedHalf(Float16(bitPattern: 1)).bitPattern, 0)
        XCTAssertEqual(SM53Reference.flushedHalf(Float16(bitPattern: 0x8001)).bitPattern, 0x8000_0000)
        XCTAssertEqual(SM53Reference.flushedHalf(Float16(bitPattern: 0x0400)), 0x1p-14)
        let aboveMidpoint = Float(bitPattern: 0x3880_1001)
        XCTAssertEqual(Float16(aboveMidpoint).bitPattern, 0x0401)
        XCTAssertEqual(SM53Reference.truncatedSource(aboveMidpoint).bitPattern, 0x0400)
        XCTAssertEqual(SM53Reference.truncatedSource(.greatestFiniteMagnitude).bitPattern, 0x7BFF)
        XCTAssertEqual(SM53Reference.truncatedSource(-.greatestFiniteMagnitude).bitPattern, 0xFBFF)
        XCTAssertEqual(SM53Reference.truncatedSource(.infinity).bitPattern, 0x7C00)
        XCTAssertTrue(SM53Reference.truncatedSource(.nan).isNaN)
        let halfSubnormal = SM53Reference.halfResult(Float16(bitPattern: 0x1D), Float16(bitPattern: 0x803B), 1)
        XCTAssertEqual(halfSubnormal[0].bitPattern, 0x8000_0000)
    }

    func testPlacementHeapOnThisMac() async throws {
        let scene = try XCTUnwrap(Scenes.all.first { $0.id == "gpu.heap.placement" })
        let result = try await scene.run(makeGPU())
        XCTAssertTrue(result.passed, result.reasons(limit: 8).joined(separator: "; "))
        XCTAssertEqual(result.checks.count, 4)
    }
}
