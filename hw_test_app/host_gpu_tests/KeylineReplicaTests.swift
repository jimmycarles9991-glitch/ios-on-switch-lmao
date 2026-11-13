import Foundation
import HWTestCore
import HWTestGPU
import Metal
import XCTest

/// The keyline replica on this Mac's QuartzCore and GPU: Apple's draw of the state must leave every attachment as
/// KeylineReplica.expected says, so a wrong texel on the device is the device's.
final class KeylineReplicaTests: XCTestCase {
    func testAppleDrawsTheKeylineStateAsTheReplicaExpects() async throws {
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let queue = try XCTUnwrap(device.makeCommandQueue())
        let library = try await device.makeLibrary(source: "", options: nil)
        let gpu = GPU(device: device, queue: queue, library: library)
        for memoryless in [false, true] {
            let drawn: KeylineReplica.Drawn
            do {
                drawn = try await KeylineReplica.draw(gpu: gpu, memoryless: memoryless)
            } catch {
                throw XCTSkip("this Mac's QuartzCore does not make the keyline state: \(error)")
            }
            let (wrong, texels) = KeylineReplica.judge(drawn)
            let row = KeylineReplica.marks[0].probe
            print(
                "keyline replica memoryless=\(memoryless) row=\(KeylineReplica.rgba(drawn, row.x, row.y)) "
                    + "want=\(KeylineReplica.expected(row.x, row.y)) others=\(KeylineReplica.others(drawn, row.x, row.y))"
            )
            XCTAssertEqual(wrong, "", "memoryless=\(memoryless): \(texels) texels off")
        }
    }
}
