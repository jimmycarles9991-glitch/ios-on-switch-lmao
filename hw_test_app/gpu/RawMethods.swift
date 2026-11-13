import Foundation
import HWTestCore
import Metal

/// GM20B methods Metal cannot reach (drivers/TegraGM20BMetal/raw/runtime/RawMetalProbe.mm, debug builds of the
/// facade): while armed, every draw of one instance this process encodes emits `before` after its state and `after`
/// once it ended. Pairs are (3D-class method, value).
public enum RawMethods {
    static let selector = NSSelectorFromString("switchRawProbeBefore:after:")

    /// nil once armed, else why not.
    public static func arm(_ device: MTLDevice, before: [(UInt32, UInt32)], after: [(UInt32, UInt32)]) -> String? {
        guard let object = device as? NSObject, object.responds(to: selector) else {
            return "the device has no raw probe channel"
        }
        let refusal = object.perform(selector, with: data(before), with: data(after))
        return refusal?.takeUnretainedValue() as? String
    }

    /// The draws that emitted the methods since they were armed.
    public static func disarm(_ device: MTLDevice) -> Int {
        guard let object = device as? NSObject, object.responds(to: selector) else { return 0 }
        let emitted = object.perform(selector, with: nil, with: nil)?.takeUnretainedValue() as? String
        return emitted.flatMap { Int($0) } ?? 0
    }

    static func data(_ pairs: [(UInt32, UInt32)]) -> NSData {
        let words = pairs.flatMap { [$0.0, $0.1] }
        return words.withUnsafeBytes { NSData(bytes: $0.baseAddress, length: $0.count) }
    }

    /// One command buffer drawn with the methods armed: its path on the facade, and the draws that emitted them
    /// (0: the methods never reached the GPU, whatever the picture shows).
    public static func draw(
        _ gpu: GPU, before: [(UInt32, UInt32)], after: [(UInt32, UInt32)], _ encode: (MTLCommandBuffer) throws -> Void
    ) async throws -> (path: String, emitted: Int, gpuMilliseconds: Double) {
        if let refusal = arm(gpu.device, before: before, after: after) { throw TestFailure(refusal) }
        do {
            let timing = try await gpu.run(encode)
            return (timing.path, disarm(gpu.device), timing.gpuMilliseconds)
        } catch {
            _ = disarm(gpu.device)
            throw error
        }
    }
}
