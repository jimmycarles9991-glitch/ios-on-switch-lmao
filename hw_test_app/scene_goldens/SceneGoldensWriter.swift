import Foundation
import HWTestCore
import HWTestGPU
import Metal
import XCTest

/// The GPU scenes' pictures that have no CPU model (SceneResult.picture), drawn on this Mac's GPU with the IPSW's own
/// QuartzCore library and the app's shaders compiled from source, written to hw_test_app/goldens/scenes_<build>.txt in
/// the checkout under `bazel run` (the test's outputs otherwise). GPUSceneTest holds the Switch's pictures to them.
final class SceneGoldensWriter: XCTestCase {
    func testWriteTheReference() async throws {
        let environment = ProcessInfo.processInfo.environment
        let runfiles = (environment["TEST_SRCDIR"] ?? ".") + "/"
        let build = try XCTUnwrap(environment["CA_BUILD"])
        let quartzCore = try XCTUnwrap(environment["CA_LIBRARY"])
        CAState.library = URL(fileURLWithPath: runfiles + quartzCore)
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let sources = try ["HWTest.metal", "Scenes.metal", "FramebufferFetch.metal"].map {
            try String(contentsOfFile: runfiles + "_main/hw_test_app/shaders/" + $0, encoding: .utf8)
        }
        let library = try await device.makeLibrary(
            source: sources.joined(separator: "\n"), options: MTLCompileOptions())
        let gpu = GPU(device: device, queue: try XCTUnwrap(device.makeCommandQueue()), library: library)
        var lines = ["# QuartzCore \(build) on \(device.name), 8x8 block means"]
        for scene in Scenes.all + [Scenes.glass, Scenes.glassSplit] {
            guard let result = try? await scene.run(gpu) else { continue }
            for picture in result.pictures where picture.golden == picture.label {
                lines.append(
                    SceneGoldens.line(
                        scene: scene.id, picture: picture.label, metalFormat: picture.metalFormat, picture.signature))
            }
        }
        XCTAssertGreaterThan(lines.count, 1, "no scene drew a picture with this QuartzCore")
        let folder =
            environment["BUILD_WORKSPACE_DIRECTORY"].map { $0 + "/hw_test_app/goldens" }
            ?? environment["TEST_UNDECLARED_OUTPUTS_DIR"] ?? NSTemporaryDirectory()
        let url = URL(fileURLWithPath: folder).appendingPathComponent("scenes_\(build).txt")
        try (lines.joined(separator: "\n") + "\n").write(to: url, atomically: true, encoding: .utf8)
        print("\(lines.count - 1) pictures -> \(url.path)")
    }
}
