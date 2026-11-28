import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// What a Metal object made by the GM20B facade answers about itself in debug runtimes
/// (drivers/TegraGM20BMetal/TegraGM20BMetalObjectProbe.mm): its class. An object the facade did not make answers
/// nothing.
public enum NativeObject {
    public static func name(_ object: AnyObject) -> String? {
        let selector = NSSelectorFromString("switchMetalObject")
        guard let object = object as? NSObject, object.responds(to: selector),
              let answer = object.perform(selector)?.takeUnretainedValue() as? [String: Any]
        else { return nil }
        return answer["class"] as? String ?? "?"
    }
}

/// A workload's verdict: what went wrong, the objects it made through the device under test, and the paths of the
/// command buffers that used them.
public struct NativeWork {
    public var failures: [String] = []
    public var objects: [AnyObject] = []
    public var paths: [String] = []

    public init(failures: [String] = [], objects: [AnyObject] = [], paths: [String] = []) {
        self.failures = failures
        self.objects = objects
        self.paths = paths
    }
}

/// A Metal object the facade makes itself: on the facade, the workload gives the right bytes, every object it made
/// is the device's own, and the command buffers that used them stayed raw.
public protocol NativeObjectTest: HWTest {
    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork
    /// Why this build's identity has nothing of the feature to test (nil: it has)
    var unavailable: String? { get }
}

extension NativeObjectTest {
    public var subsystem: Subsystem { .metal }
    public var unavailable: String? { nil }

    public func run(_ context: TestContext) async throws -> Outcome {
        if let why = unavailable { return .skip(why) }
        let gpu = try GPU.make()
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else {
            return .skip("the default device is not the facade (\(GM20BFacade.kind(of: gpu.device)))")
        }
        guard !GM20BFacade.counters(of: gpu.device).isEmpty else {
            return .skip("a release runtime: objects do not answer switchMetalObject")
        }
        var metrics = Metrics()
        var work = try await exercise(gpu, &metrics)
        var names: [String] = []
        for object in work.objects {
            guard let name = NativeObject.name(object) else {
                let name = NSStringFromClass(type(of: object))
                names.append(name)
                work.failures.append("\(name) is not the facade's own")
                continue
            }
            names.append(name)
        }
        metrics.set("objects", names.isEmpty ? "-" : Set(names).sorted().joined(separator: ","))
        metrics.set("paths", work.paths.isEmpty ? "-" : work.paths.joined(separator: ","))
        if let path = work.paths
            .first(where: { $0 != "gm20b" }) { work.failures.append("a command buffer went \(path)") }
        return work.failures.isEmpty
            ? .pass(metrics, proof: gpu.proof)
            : .fail(work.failures.joined(separator: "; "), metrics, proof: gpu.proof)
    }
}
