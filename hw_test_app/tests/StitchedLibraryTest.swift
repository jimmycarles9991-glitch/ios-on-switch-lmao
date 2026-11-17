import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A stitched library of the app library's own visible functions (shaders/Stitching.metal) is made by the system shader
/// compiler's Metal front end from the base device's DAG text and each function's bitcode as the raw device read it
/// (drivers/TegraGM20BMetal/raw/runtime/RawMetalCompiledLibraries.mm), never from m2v twins: the stitched function is
/// the raw device's own. Core Image stitches its kernels this way (iconservicesagent, Maps, mediaanalysisd on boot 21).
/// The case fails if the library is not made or anything asked for the m2v device (m2v_needs_asked).
struct StitchedLibraryTest: HWTest {
    let id = "gpu.raw.stitched_library"
    let name = "A stitched library, made by the shader compiler"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let before = OwnAnswersTest.needs(gpu.device)
        guard let add = gpu.library.makeFunction(name: "stitch_add"),
            let mul = gpu.library.makeFunction(name: "stitch_mul")
        else { throw TestFailure("no stitch_add or stitch_mul in the app library") }
        let a = MTLFunctionStitchingInputNode(argumentIndex: 0)
        let b = MTLFunctionStitchingInputNode(argumentIndex: 1)
        let sum = MTLFunctionStitchingFunctionNode(name: "stitch_add", arguments: [a, b], controlDependencies: [])
        let product = MTLFunctionStitchingFunctionNode(
            name: "stitch_mul", arguments: [sum, b], controlDependencies: [])
        let descriptor = MTLStitchedLibraryDescriptor()
        descriptor.functionGraphs = [
            MTLFunctionStitchingGraph(functionName: "stitch_madd", nodes: [sum], outputNode: product, attributes: [])
        ]
        descriptor.functions = [add, mul]

        let started = Date()
        var made: MTLLibrary?
        var failure = "-"
        do {
            made = try await gpu.device.makeLibrary(stitchedDescriptor: descriptor)
        } catch {
            failure = String(error.localizedDescription.prefix(160))
        }
        let madeMs = Date().timeIntervalSince(started) * 1000
        let stitched = made?.makeFunction(name: "stitch_madd")
        let asked = OwnAnswersTest.needs(gpu.device).filter { $0.value > (before[$0.key] ?? 0) }.keys.sorted()

        var metrics = Metrics()
        metrics.set("made_ms", madeMs, digits: 1)
        metrics.set("functions", made?.functionNames.joined(separator: ",") ?? "-")
        metrics.set("function_type", Int(stitched?.functionType.rawValue ?? 0))
        metrics.set("library", made.map { String(describing: type(of: $0 as AnyObject)) } ?? "-")
        metrics.set("failure", failure)
        metrics.set("m2v_needs_asked", asked.isEmpty ? "-" : asked.joined(separator: ","))
        var wrong: [String] = []
        if made == nil { wrong.append("library") }
        if stitched?.functionType != .visible { wrong.append("function") }
        if !asked.isEmpty { wrong.append("m2v") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
