import Foundation
import HWTestCore
import HWTestGPU
import Metal
import XCTest

/// gpu.ca.sweep's reference (core/CAGoldens.swift): every Core Animation state of the raw pack's corpus drawn on this
/// Mac's GPU with the IPSW's own QuartzCore library, written to hw_test_app/goldens/ca_<build>.txt in the checkout
/// under `bazel run` (the test's outputs otherwise). States of the other iOS version's QuartzCore are left out.
final class CAGoldensWriter: XCTestCase {
    func testWriteTheReference() async throws {
        let environment = ProcessInfo.processInfo.environment
        let runfiles = (environment["TEST_SRCDIR"] ?? ".") + "/"
        let build = try XCTUnwrap(environment["CA_BUILD"])
        let states = try String(contentsOfFile: runfiles + XCTUnwrap(environment["CA_STATES"]), encoding: .utf8)
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let quartzCore = try device.makeLibrary(
            URL: URL(fileURLWithPath: runfiles + XCTUnwrap(environment["CA_LIBRARY"])))
        let gpu = GPU(
            device: device, queue: try XCTUnwrap(device.makeCommandQueue()),
            library: try await device.makeLibrary(source: "kernel void unused() {}", options: nil))
        var lines = [
            "# QuartzCore \(build) on \(device.name), \(CAStateSweep.width)x\(CAStateSweep.height), 8x8 block means"
        ]
        var written = Set<String>()
        for line in CAStateSweep.states(states, everyState: true) {
            guard written.insert(CAStateSweep.key(line)).inserted,
                let drawn = try? await CAStateSweep.draw(line, library: quartzCore, gpu: gpu)
            else { continue }
            let colors = line.colors.sorted { $0.index < $1.index }
            let signatures = zip(colors, drawn.attachments).map { color, bytes in
                (
                    index: color.index,
                    signature: Signature(
                        bytes: bytes, metalFormat: color.format, width: CAStateSweep.width,
                        height: CAStateSweep.height)
                )
            }
            lines.append(CAGoldens.line(key: CAStateSweep.key(line), label: line.shortLabel, signatures))
        }
        XCTAssertGreaterThan(lines.count, 1, "no state of the corpus drew with this QuartzCore")
        let folder =
            environment["BUILD_WORKSPACE_DIRECTORY"].map { $0 + "/hw_test_app/goldens" }
            ?? environment["TEST_UNDECLARED_OUTPUTS_DIR"] ?? NSTemporaryDirectory()
        let url = URL(fileURLWithPath: folder).appendingPathComponent("ca_\(build).txt")
        try (lines.joined(separator: "\n") + "\n").write(to: url, atomically: true, encoding: .utf8)
        print("\(lines.count - 1) states -> \(url.path)")
    }
}
