import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// A pass with no depth or stencil attachment under a depth/stencil state that tests: Apple's GPU tests in its tile,
/// cleared to depth 1 and stencil 0, and keeps the pass's writes to it; the driver gives such a pass a hidden
/// depth/stencil target. The cases of an M4 Max probe (DepthProbeScene's hidden_probe.mm), each a red full-screen
/// triangle under one state and then a green one under another over black, every pixel the colour Metal leaves:
/// red, black, red, red, red, black, green. Per case <case>=<pixels off that colour>/<path>.
struct HiddenDepthTest: HWTest {
    let id = "gpu.gm20b.hidden_depth"
    let name = "A pass with no depth or stencil attachment depth- and stencil-tests as Metal's tile does, on raw"
    let subsystem = Subsystem.metal

    struct Draw {
        var depth: MTLCompareFunction = .never
        var write = false
        var stencil: MTLCompareFunction = .always
        var operation: MTLStencilOperation = .keep
        var reference: UInt32 = 0
        var z: Float = 0.5
    }

    static let cases: [(key: String, first: Draw, second: Draw, leaves: [UInt8])] = [
        ("less_0999", Draw(depth: .less, z: 0.999), Draw(), DepthProbeScene.red),
        ("less_1", Draw(depth: .less, z: 1), Draw(), DepthProbeScene.black),
        ("less_equal_1", Draw(depth: .lessEqual, z: 1), Draw(), DepthProbeScene.red),
        ("written_then_less", Draw(depth: .always, write: true, z: 0.25), Draw(depth: .less), DepthProbeScene.red),
        ("stencil_equal_0", Draw(depth: .always, stencil: .equal), Draw(), DepthProbeScene.red),
        ("stencil_equal_1", Draw(depth: .always, stencil: .equal, reference: 1), Draw(), DepthProbeScene.black),
        (
            "incremented_then_equal_1", Draw(depth: .always, operation: .incrementClamp),
            Draw(depth: .always, stencil: .equal, reference: 1), DepthProbeScene.green
        ),
    ]

    func depthStencil(_ device: MTLDevice, _ d: Draw) throws -> MTLDepthStencilState {
        let descriptor = MTLDepthStencilDescriptor()
        descriptor.depthCompareFunction = d.depth
        descriptor.isDepthWriteEnabled = d.write
        let stencil = MTLStencilDescriptor()
        stencil.stencilCompareFunction = d.stencil
        stencil.depthStencilPassOperation = d.operation
        descriptor.frontFaceStencil = stencil
        descriptor.backFaceStencil = stencil
        guard let state = device.makeDepthStencilState(descriptor: descriptor) else {
            throw TestFailure("no depth/stencil state")
        }
        return state
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let scene = try await DepthProbeScene(gpu)
        let state = try scene.state()
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for c in Self.cases {
            do {
                let colour = try scene.colour()
                let states = [try depthStencil(gpu.device, c.first), try depthStencil(gpu.device, c.second)]
                let path = try await gpu.run(tolerateError: true) { commands in
                    let encoder = commands.makeRenderCommandEncoder(descriptor: DepthProbeScene.pass(colour))!
                    encoder.setRenderPipelineState(state)
                    let draws: [(Draw, SIMD4<Float>)] = [(c.first, [1, 0, 0, 1]), (c.second, [0, 1, 0, 1])]
                    for (tested, (d, rgba)) in zip(states, draws) {
                        encoder.setDepthStencilState(tested)
                        encoder.setStencilReferenceValue(d.reference)
                        DepthProbeScene.draw(encoder, z: d.z, rgba)
                    }
                    encoder.endEncoding()
                }.path
                let off = try await scene.off(colour, c.leaves)
                metrics.set(c.key, "\(off)/\(String(path.prefix(60)))")
                checks.append(("\(c.key): every pixel the colour Metal leaves, raw", off == 0 && path == "gm20b"))
            } catch {
                checks.append(("\(c.key): \(GPU.describe(error))", false))
            }
        }
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
