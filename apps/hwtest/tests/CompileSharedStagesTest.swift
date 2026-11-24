import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A second state over the same stages takes them from the shader compiler's first compile. Boot 2's compiler compiled
/// the (vertex, fragment) pair again for 310 of 532 render states whose keys differed only in colour format, blend and
/// sample count, among them DrawBands.metal's 5-7 s fragment three times. Here two states share a fragment salted with
/// the run's own value, so neither is in a pack or store before the run, and differ only in their colour attachment's
/// format, which the fragment's code does not read (BGRA8 and RGBA8 make the same program): the second state is
/// compiled in at most a quarter of the first's time. What it still costs is its key (decoding and specializing its
/// functions, about half) and its record: on the host 4.7-11.5 ms against 55-118 ms.
struct CompileSharedStagesTest: HWTest {
    let id = "gpu.compile.shared_stages"
    let name = "A second state over the same stages takes them from the first state's compile"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    static let shareOfFirst = 0.25
    /// Below this the first compile is too short for its quarter to be measured
    static let firstAtLeastMs = 100.0

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else {
            return .skip("the default device is not the facade")
        }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        let salt = UInt32.random(in: 1 ... UInt32.max)
        let first = try CompileAnswerLatencyTest.Account(of: pipeline(gpu, salt: salt, format: .bgra8Unorm))
        let second = try CompileAnswerLatencyTest.Account(of: pipeline(gpu, salt: salt, format: .rgba8Unorm))
        let ms = { (account: CompileAnswerLatencyTest.Account) in Double(account.us["compile_us"] ?? -1) / 1000 }
        metrics.set("first_how", first.how)
        metrics.set("second_how", second.how)
        metrics.set("first_compile_ms", ms(first), digits: 1)
        metrics.set("second_compile_ms", ms(second), digits: 1)
        metrics.set("second_wait_ms", Double(second.us["wait_us"] ?? -1) / 1000, digits: 1)

        checks.append(("both states were compiled for this run", first.how == "compiled" && second.how == "compiled"))
        let longEnough = ms(first) >= Self.firstAtLeastMs
        checks.append(("the first compile took \(Int(Self.firstAtLeastMs)) ms or more", longEnough))
        checks.append((
            "the second compile took at most a quarter of the first's",
            ms(second) >= 0 && ms(second) <= ms(first) * Self.shareOfFirst
        ))
        return .judged(checks, metrics, proof: "salt \(salt)").onRawPath(gpu)
    }

    func pipeline(_ gpu: GPU, salt: UInt32, format: MTLPixelFormat) throws -> MTLRenderPipelineState {
        var value = salt
        let constants = MTLFunctionConstantValues()
        constants.setConstantValue(&value, type: .uint, index: 0)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = gpu.library.makeFunction(name: "quad_vertex")
        descriptor.fragmentFunction = try gpu.library.makeFunction(
            name: "shared_stages_fragment",
            constantValues: constants
        )
        let color: MTLRenderPipelineColorAttachmentDescriptor = descriptor.colorAttachments[0]
        color.pixelFormat = format
        color.isBlendingEnabled = true
        color.sourceRGBBlendFactor = .sourceAlpha
        color.destinationRGBBlendFactor = .oneMinusSourceAlpha
        color.sourceAlphaBlendFactor = .sourceAlpha
        color.destinationAlphaBlendFactor = .oneMinusSourceAlpha
        return try gpu.device.makeRenderPipelineState(descriptor: descriptor)
    }
}
