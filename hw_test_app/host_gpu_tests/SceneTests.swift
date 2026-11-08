import Foundation
import HWTestCore
import HWTestGPU
import XCTest

/// Every GPU scene on this Mac's GPU, the shaders compiled from their sources: a scene whose CPU reference is wrong
/// fails here before it ever reaches the Switch.
final class SceneTests: XCTestCase {
    override class func setUp() {
        super.setUp()
        let directory =
            (ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? "")
            + "hw_test_app/shaders"
        GPU.makeLibrary = { device in
            let sources = try ["HWTest.metal", "Scenes.metal"].map {
                try String(contentsOfFile: directory + "/" + $0, encoding: .utf8)
            }
            return try device.makeLibrary(source: sources.joined(separator: "\n"), options: nil)
        }
    }

    func testEverySceneMatchesItsReferenceOnThisMac() async throws {
        let gpu = try GPU.make()
        for scene in Scenes.all {
            let result = try await scene.run(gpu)
            XCTAssertTrue(result.passed, "\(scene.id): \(result.reasons(limit: 4).joined(separator: "; "))")
        }
    }
}
