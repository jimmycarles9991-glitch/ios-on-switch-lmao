import Foundation
import HWTestCore
import Metal

/// Core Image opens its function libraries (metallib file type 1) with newLibraryWithURL:, and CoreUI, Vision, PencilKit
/// and WeatherMaps ship libraries of the same kind. From 24B119 (format 2.9) six of them list a HASH that is no digest of
/// their module, which Metal takes and m2v refused as a hash mismatch. Each one the OS ships must open.
struct CoreImageLibraryTest: HWTest {
    let id = "metal.library.core_image"
    let name = "Core Image's function libraries open"
    let subsystem = Subsystem.metal

    static let stdlib = "/System/Library/Frameworks/CoreImage.framework/ci_stdlib.metallib"

    /// Every file type 1 library of 21H461 and 24B119.
    static let libraries = [
        stdlib,
        "/System/Library/Frameworks/CoreImage.framework/ci_stdlib_h.metallib",
        "/System/Library/Frameworks/CoreImage.framework/ci_filters.metallib",
        "/System/Library/Frameworks/CoreImage.framework/ci_raw.metallib",
        "/System/Library/Frameworks/CoreImage.framework/CIPortraitBlurV2.metallib",
        "/System/Library/Frameworks/CoreImage.framework/CIPortraitBlurV3.metallib",
        "/System/Library/Frameworks/Vision.framework/ImageFilters.metallib",
        "/System/Library/Frameworks/PencilKit.framework/SixChannelComposite.ci.metallib",
        "/System/Library/PrivateFrameworks/CoreUI.framework/default.metallib",
        "/System/Library/PrivateFrameworks/WeatherMaps.framework/WeatherMapsSnapshotMetalLib.metallib",
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        guard let device = MTLCreateSystemDefaultDevice() else {
            return .fail("MTLCreateSystemDefaultDevice returned nil")
        }
        let present = Self.libraries.filter { FileManager.default.fileExists(atPath: $0) }
        var refused: [String] = []
        for path in present {
            do {
                _ = try device.makeLibrary(URL: URL(fileURLWithPath: path))
            } catch {
                refused.append("\((path as NSString).lastPathComponent): \(error.localizedDescription)")
            }
        }
        var metrics = Metrics()
        metrics.set("libraries", present.count)
        metrics.set("refused", refused.isEmpty ? "-" : refused.joined(separator: "; "))
        return .judged(
            [
                ("Core Image's ci_stdlib is there", present.contains(Self.stdlib)),
                ("every library opens", refused.isEmpty),
            ], metrics, proof: device.name)
    }

    var timeout: TimeInterval { 20 }
}
