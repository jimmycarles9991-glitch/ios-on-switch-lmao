import Foundation
import HWTestCore

/// L959: window B's composite (drivers/NativeMobileDisplay/SwitchPlaneCompositeDisplay.h) on what Core Animation
/// detaches. The overlay sets VIC composes are the ones DC A's memory admission refuses, which takes two 4K videos; here
/// TegraVIC's kernel-test selector tells the display to take every set as refused (debug kernels), display.planes plays
/// its clip, and the report says how many frames VIC composited meanwhile. The clip must still play on window B with
/// nothing dropped (display.planes' own checks). The composite is VIC's colour conversion and filtering, not DC A's, so
/// no CRC is compared. iOS 27's app runs it: the selector call is its X1VICKernelTest.
public struct PlanesCompositeTest: HWTest {
    public let id = "display.planes_composite"
    public let name = "Video on window B as VIC's composite, the memory admission refusing it"
    public let subsystem = Subsystem.compositor
    public var timeout: TimeInterval { 120 }
    let kernelTest: @Sendable (UInt32, Int) -> Result<[UInt32], TestFailure>

    static let arm: UInt32 = 0x5643_4641  // 'VCFA'
    static let report: UInt32 = 0x5643_4652  // 'VCFR'
    static let replyBytes = 16

    public init(kernelTest: @escaping @Sendable (UInt32, Int) -> Result<[UInt32], TestFailure>) {
        self.kernelTest = kernelTest
    }

    public func run(_ context: TestContext) async throws -> Outcome {
        switch kernelTest(Self.arm, Self.replyBytes) {
        case .failure(let failure): return .skip("arming the composite: \(failure)")
        case .success(let reply) where reply[3] != 0:
            return .skip("arming the composite: the display answered \(Int32(bitPattern: reply[3]))")
        case .success: break
        }
        let played = try await PlanesTest().run(context)
        let composited: Int
        switch kernelTest(Self.report, Self.replyBytes) {
        case .failure(let failure): return .fail("reporting the composite: \(failure)", played.metrics)
        case .success(let reply): composited = Int(reply[2])
        }
        var metrics = played.metrics
        metrics.set("composited", composited)
        guard played.verdict != .skip else { return .skip(played.detail, metrics) }
        return .judged(
            [
                ("display.planes held: \(played.detail.isEmpty ? "pass" : played.detail)", played.verdict == .pass),
                ("VIC composited the video (composited > 0)", composited > 0),
            ], metrics)
    }
}
