import Foundation
import HWTestCore
import HWTestKit
import Metal

/// iOS 27's frameworks ship their binary archives as thin Apple GPU Mach-O binaries, which Metal opens on a GPU they hold
/// nothing for as empty archives. Boot 13's device answered nil (m2v reads only its own archives and metallibs), and
/// RenderBox logged a precondition failure for each in iconservicesagent, SpringBoard and TVWidgetExtension. A missing
/// file stays MTLBinaryArchiveError.invalidFile.
struct BinaryArchiveTest: HWTest {
    let id = "metal.archive.framework"
    let name = "Framework binary archives open as archives"
    let subsystem = Subsystem.metal

    static let frameworks = [
        "/System/Library/PrivateFrameworks/RenderBox.framework/archive.metallib",
        "/System/Library/PrivateFrameworks/IconRendering.framework/binary.metallib",
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        guard let device = MTLCreateSystemDefaultDevice() else {
            return .fail("MTLCreateSystemDefaultDevice returned nil")
        }
        var metrics = Metrics()
        var refused: [String] = []
        for path in Self.frameworks where FileManager.default.fileExists(atPath: path) {
            let descriptor = MTLBinaryArchiveDescriptor()
            descriptor.url = URL(fileURLWithPath: path)
            do {
                _ = try device.makeBinaryArchive(descriptor: descriptor)
            } catch {
                refused.append("\((path as NSString).lastPathComponent): \(error.localizedDescription)")
            }
        }
        metrics.set("refused", refused.isEmpty ? "-" : refused.joined(separator: "; "))
        let missing = MTLBinaryArchiveDescriptor()
        missing.url = URL(fileURLWithPath: "/System/Library/PrivateFrameworks/RenderBox.framework/missing.metallib")
        var missingCode = -1
        do {
            _ = try device.makeBinaryArchive(descriptor: missing)
        } catch {
            missingCode = (error as NSError).code
        }
        metrics.set("missing_code", missingCode)
        guard refused.isEmpty else { return .fail("a framework's archive did not open", metrics) }
        guard missingCode == MTLBinaryArchiveError.invalidFile.rawValue else {
            return .fail("a missing archive file did not answer invalidFile", metrics)
        }
        return .pass(metrics)
    }

    var timeout: TimeInterval { 10 }
}
