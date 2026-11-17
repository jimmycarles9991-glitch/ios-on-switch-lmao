import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A library from Metal source is made by the system shader compiler's Metal front end and read as the raw device's own
/// (drivers/TegraGM20BMetal/raw/runtime/RawMetalCompiledLibraries.mm, compiler/compiler_codegen.mm), never by m2v: its
/// kernel's state and dispatch run on the raw path, and source the front end refuses fails as Metal's compile failure
/// with the front end's log. The case fails if any of it asked for the m2v device (m2v_needs_asked).
struct SourceLibraryTest: HWTest {
    let id = "gpu.raw.source_library"
    let name = "A library from source, made by the shader compiler"
    let subsystem = Subsystem.metal

    static let source = """
        #include <metal_stdlib>
        using namespace metal;
        kernel void source_library_cube(device uint *data [[buffer(0)]], uint id [[thread_position_in_grid]]) {
            data[id] = id * id * id + 3;
        }
        """
    static let broken = "kernel void source_library_broken( {}"
    static let count = 1024

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let before = OwnAnswersTest.needs(gpu.device)

        let started = Date()
        let library = try await gpu.device.makeLibrary(source: Self.source, options: nil)
        let compileMs = Date().timeIntervalSince(started) * 1000
        guard let function = library.makeFunction(name: "source_library_cube") else {
            throw TestFailure("no source_library_cube in \(library.functionNames)")
        }
        let state = try await gpu.device.makeComputePipelineState(function: function)
        let data = try gpu.buffer(Self.count * 4)
        let path = try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(state)
            encoder.setBuffer(data, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: Self.count, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1))
            encoder.endEncoding()
        }.path
        let values = data.contents().bindMemory(to: UInt32.self, capacity: Self.count)
        let off = (0..<Self.count).filter { values[$0] != UInt32($0 * $0 * $0 + 3) }.count

        var refusedCode = -1
        var refusedLog = ""
        do {
            _ = try await gpu.device.makeLibrary(source: Self.broken, options: nil)
        } catch let error as NSError {
            refusedCode = error.code
            refusedLog = String(error.localizedDescription.prefix(120))
        }
        let asked = OwnAnswersTest.needs(gpu.device).filter { $0.value > (before[$0.key] ?? 0) }.keys.sorted()

        var metrics = Metrics()
        metrics.set("compile_ms", compileMs, digits: 1)
        metrics.set("functions", library.functionNames.joined(separator: ","))
        metrics.set("words_off", off)
        metrics.set("path", path)
        metrics.set("refused_code", refusedCode)
        metrics.set("refused_log", refusedLog.isEmpty ? "-" : refusedLog)
        metrics.set("m2v_needs_asked", asked.isEmpty ? "-" : asked.joined(separator: ","))
        var wrong: [String] = []
        if off > 0 { wrong.append("words") }
        if path != "raw" { wrong.append("path") }
        let compileFailure = MTLLibraryError.Code.compileFailure.rawValue
        if refusedCode != compileFailure || refusedLog.isEmpty { wrong.append("refusal") }
        if !asked.isEmpty { wrong.append("m2v") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
