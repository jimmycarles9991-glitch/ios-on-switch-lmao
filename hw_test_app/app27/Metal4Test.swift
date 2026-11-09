import Foundation
import HWTestCore
import HWTestKit
import Metal

/// Metal 4 where the device offers it. J181's A13 has no Metal 4 family, so under the J181 identity the device answers
/// metal4=0 and the test passes with mtl4=0; every family answer is logged, so a wrong yes shows. Where the family is
/// there, an MTL4 queue, allocator and one empty command buffer must complete: the queue signals a shared event after
/// it, and the test's timeout fails a signal that never comes. The MTL4 entry points are probed with
/// respondsToSelector: first, because the raw facade forwards a selector it lacks to its m2v twin and one neither
/// answers raises, which would end the suite.
struct Metal4Test: HWTest {
    let id = "metal4.device"
    let name = "Metal 4 family, MTL4 queue and command buffer"
    let subsystem = Subsystem.metal

    static let selectors = [
        "newMTL4CommandQueue", "newCommandAllocator", "newCommandBuffer", "newCompilerWithDescriptor:error:",
        "newArgumentTableWithDescriptor:error:",
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        guard let device = MTLCreateSystemDefaultDevice() else {
            return .fail("MTLCreateSystemDefaultDevice returned nil")
        }
        var metrics = Metrics()
        metrics.set("apple", Self.families(device, base: 1000, count: 11))
        metrics.set("common", Self.families(device, base: 3000, count: 3))
        metrics.set("metal3", device.supportsFamily(.metal3))
        let metal4 = device.supportsFamily(.metal4)
        metrics.set("metal4", metal4)
        let missing = Self.selectors.filter { !(device as AnyObject).responds(to: NSSelectorFromString($0)) }
        metrics.set("mtl4_missing", missing.isEmpty ? "-" : missing.joined(separator: ","))
        guard metal4 else {
            metrics.set("mtl4", false)
            return .pass(metrics, detail: "no Metal 4 family")
        }
        guard missing.isEmpty else {
            return .fail("the Metal 4 family without \(missing.joined(separator: ", "))", metrics)
        }
        guard let queue = device.makeMTL4CommandQueue(), let allocator = device.makeCommandAllocator(),
            let commands = device.makeCommandBuffer(), let event = device.makeSharedEvent()
        else { return .fail("an MTL4 queue, allocator, command buffer or event was nil", metrics) }
        commands.beginCommandBuffer(allocator: allocator)
        commands.endCommandBuffer()
        let start = CFAbsoluteTimeGetCurrent()
        queue.commit([commands])
        queue.signalEvent(event, value: 1)
        await event.valueSignaled(1)
        metrics.set("mtl4", true)
        metrics.set("signal_ms", (CFAbsoluteTimeGetCurrent() - start) * 1000, digits: 2)
        return .pass(metrics)
    }

    var timeout: TimeInterval { 10 }

    /// The families base+1 ... base+count the device supports, as "1,2,3" ("-" for none). By raw value: iOS 27
    /// deprecates the Common names.
    static func families(_ device: MTLDevice, base: Int, count: Int) -> String {
        let supported = (1...count).filter { MTLGPUFamily(rawValue: base + $0).map(device.supportsFamily) ?? false }
        return supported.isEmpty ? "-" : supported.map(String.init).joined(separator: ",")
    }
}
