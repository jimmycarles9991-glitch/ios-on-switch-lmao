import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A stitched library of the app library's own visible functions (shaders/Stitching.metal) is made by the system shader
/// compiler's Metal front end from the base device's DAG text and each function's bitcode as the device read it
/// (drivers/TegraGM20BMetal/TegraGM20BMetalCompiledLibraries.mm), never from m2v twins: the stitched function is
/// the device's own. Core Image stitches its kernels this way (iconservicesagent, Maps, mediaanalysisd on boot 21).
/// The case fails if the library is not made or any command buffer failed (failed_cbs=).
/// It also stitches stitch_scaled_add specialized with each gain, as MPS stitches its DAG functions, links the result
/// into stitch_consumer and checks every word it writes (specialized_words_off) and that each run stayed raw.
struct StitchedLibraryTest: HWTest {
    let id = "gpu.gm20b.stitched_library"
    let name = "A stitched library, made by the shader compiler"
    let subsystem = Subsystem.metal

    static let words = 32

    /// Gains 1, 2 and 1 again; the second named as MPS names its functions (MTLFunctionDescriptor.specializedName). Each
    /// run writes out[i] = i * gain + 3 over a buffer filled with NaN first.
    static func specializedOutputs(_ gpu: GPU) async throws -> (wrong: Int, last: [Float]) {
        guard let consumer = gpu.library.makeFunction(name: "stitch_consumer") else {
            throw TestFailure("no stitch_consumer in the app library")
        }
        let data = try gpu.buffer(words * MemoryLayout<Float>.stride)
        var wrong = 0
        var last: [Float] = []
        for (gain, specializedName) in [(UInt32(1), ""), (2, "stitch_scaled_add_2"), (1, "")] {
            let values = MTLFunctionConstantValues()
            var value = gain
            values.setConstantValue(&value, type: .uint, index: 3)
            let made = MTLFunctionDescriptor()
            made.name = "stitch_scaled_add"
            made.specializedName = specializedName.isEmpty ? nil : specializedName
            made.constantValues = values
            let function = try await gpu.library.makeFunction(descriptor: made)
            let a = MTLFunctionStitchingInputNode(argumentIndex: 0)
            let b = MTLFunctionStitchingInputNode(argumentIndex: 1)
            let output = MTLFunctionStitchingFunctionNode(
                name: function.name, arguments: [a, b], controlDependencies: []
            )
            let stitching = MTLStitchedLibraryDescriptor()
            stitching.functionGraphs = [
                MTLFunctionStitchingGraph(functionName: "stitch_scaled", nodes: [], outputNode: output, attributes: []),
            ]
            stitching.functions = [function]
            let library = try await gpu.device.makeLibrary(stitchedDescriptor: stitching)
            guard let stitched = library.makeFunction(name: "stitch_scaled") else {
                throw TestFailure("no stitch_scaled in \(library.functionNames)")
            }
            let linked = MTLLinkedFunctions()
            linked.privateFunctions = [stitched]
            let pipeline = MTLComputePipelineDescriptor()
            pipeline.computeFunction = consumer
            pipeline.linkedFunctions = linked
            let state = try gpu.device.makeComputePipelineState(descriptor: pipeline, options: [], reflection: nil)
            memset(data.contents(), 0xff, words * MemoryLayout<Float>.stride)
            _ = try await gpu.run { commands in
                guard let encoder = commands.makeComputeCommandEncoder() else {
                    throw TestFailure("makeComputeCommandEncoder returned nil")
                }
                encoder.setComputePipelineState(state)
                encoder.setBuffer(data, offset: 0, index: 0)
                encoder.dispatchThreads(
                    MTLSize(width: words, height: 1, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: words, height: 1, depth: 1)
                )
                encoder.endEncoding()
            }
            let out = data.contents().bindMemory(to: Float.self, capacity: words)
            wrong += (0 ..< words).filter { out[$0] != Float($0) * Float(gain) + 3 }.count
            last.append(out[words - 1])
        }
        return (wrong, last)
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        guard let add = gpu.library.makeFunction(name: "stitch_add"),
              let mul = gpu.library.makeFunction(name: "stitch_mul")
        else { throw TestFailure("no stitch_add or stitch_mul in the app library") }
        let a = MTLFunctionStitchingInputNode(argumentIndex: 0)
        let b = MTLFunctionStitchingInputNode(argumentIndex: 1)
        let sum = MTLFunctionStitchingFunctionNode(name: "stitch_add", arguments: [a, b], controlDependencies: [])
        let product = MTLFunctionStitchingFunctionNode(
            name: "stitch_mul", arguments: [sum, b], controlDependencies: []
        )
        let descriptor = MTLStitchedLibraryDescriptor()
        descriptor.functionGraphs = [
            MTLFunctionStitchingGraph(functionName: "stitch_madd", nodes: [sum], outputNode: product, attributes: []),
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
        var specialized: (wrong: Int, last: [Float]) = (Self.words * 3, [])
        var specializedFailure = "-"
        do {
            specialized = try await Self.specializedOutputs(gpu)
        } catch {
            specializedFailure = String(error.localizedDescription.prefix(160))
        }

        var metrics = Metrics()
        metrics.set("made_ms", madeMs, digits: 1)
        metrics.set("functions", made?.functionNames.joined(separator: ",") ?? "-")
        metrics.set("function_type", Int(stitched?.functionType.rawValue ?? 0))
        metrics.set("library", made.map { String(describing: type(of: $0 as AnyObject)) } ?? "-")
        metrics.set("failure", failure)
        metrics.set("specialized_words_off", specialized.wrong)
        metrics.set("specialized_last", specialized.last.map { String($0) }.joined(separator: ","))
        metrics.set("specialized_failure", specializedFailure)
        let failed = FailedCounts.judged(
            before: before, after: GM20BFacade.counters(of: gpu.device), own: gpu.paths, &metrics
        )
        var wrong: [String] = []
        if made == nil { wrong.append("library") }
        if stitched?.functionType != .visible { wrong.append("function") }
        if specialized.wrong != 0 { wrong.append("specialized_words") }
        if !failed.1 { wrong.append(failed.0) }
        let outcome: Outcome = wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
        return outcome.onRawPath(gpu)
    }
}
