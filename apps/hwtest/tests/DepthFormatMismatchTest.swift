import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Render states whose declared depth or stencil format is not their pass's draw as Apple's GPU draws them (an M4 Max
/// with validation off, DepthProbeScene's depth_probe.mm): each state, declaring none, Depth16Unorm, Depth32Float,
/// Depth32Float_Stencil8 or Stencil8, draws red at z 0.25 and then green at z 0.75 (Less, depth write on) over black,
/// the pass's depth cleared to 1. The second draw fails the depth test: every pixel red and every depth 0.25, the
/// command buffer completed on the driver. gpu.gm20b.depth_format_mismatch has passes with a Depth32Float or
/// Depth32Float_Stencil8 attachment; gpu.gm20b.depth_format_mismatch_no_depth has passes with none, which Apple's GPU
/// tests in its tile and the driver in a hidden depth/stencil target (gpu.gm20b.hidden_depth). Per cell
/// <declared>_<pass>=<red pixels>/<pixels at depth 0.25, "-" with no attachment>/<path> (64/64/raw).
struct DepthFormatMismatchTest: HWTest {
    let id: String
    let name: String
    let passes: [MTLPixelFormat]
    let subsystem = Subsystem.metal

    static let attached = DepthFormatMismatchTest(
        id: "gpu.gm20b.depth_format_mismatch",
        name: "States declaring another depth or stencil format draw and depth-test in a depth pass as Metal does",
        passes: [.depth32Float, .depth32Float_stencil8]
    )
    static let unattached = DepthFormatMismatchTest(
        id: "gpu.gm20b.depth_format_mismatch_no_depth",
        name: "States declaring a depth or stencil format depth-test in a pass with no depth attachment as Metal does",
        passes: [.invalid]
    )

    static let declared: [MTLPixelFormat] = [.invalid, .depth16Unorm, .depth32Float, .depth32Float_stencil8, .stencil8]

    static func key(_ format: MTLPixelFormat) -> String {
        switch format {
        case .depth16Unorm: return "d16"
        case .depth32Float: return "d32"
        case .depth32Float_stencil8: return "d32s8"
        case .stencil8: return "s8"
        default: return "none"
        }
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let scene = try await DepthProbeScene(gpu)
        let test = MTLDepthStencilDescriptor()
        test.depthCompareFunction = .less
        test.isDepthWriteEnabled = true
        guard let depthState = gpu.device.makeDepthStencilState(descriptor: test) else {
            throw TestFailure("no depth state")
        }
        let n = DepthProbeScene.side
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for declared in Self.declared {
            let state: MTLRenderPipelineState
            do {
                state =
                    try declared == .stencil8
                        ? scene.state(stencil: declared)
                        : scene.state(
                            depth: declared,
                            stencil: declared == .depth32Float_stencil8 ? declared : .invalid
                        )
            } catch {
                checks.append(("\(Self.key(declared)): \(GPU.describe(error))", false))
                continue
            }
            for format in passes {
                do {
                    let colour = try scene.colour()
                    let depth: MTLTexture? =
                        try format == .invalid ? nil : gpu.texture(format, n, n, usage: [.renderTarget])
                    let path = try await gpu.run(tolerateError: true) { commands in
                        let pass = DepthProbeScene.pass(colour)
                        pass.depthAttachment.texture = depth
                        pass.depthAttachment.loadAction = .clear
                        pass.depthAttachment.clearDepth = 1
                        pass.depthAttachment.storeAction = .store
                        if format == .depth32Float_stencil8 {
                            pass.stencilAttachment.texture = depth
                            pass.stencilAttachment.loadAction = .clear
                            pass.stencilAttachment.storeAction = .store
                        }
                        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                        encoder.setRenderPipelineState(state)
                        encoder.setDepthStencilState(depthState)
                        DepthProbeScene.draw(encoder, z: 0.25, [1, 0, 0, 1])
                        DepthProbeScene.draw(encoder, z: 0.75, [0, 1, 0, 1])
                        encoder.endEncoding()
                    }.path
                    let notRed = try await scene.off(colour, DepthProbeScene.red)
                    let reds = n * n - notRed
                    var near = "-"
                    if let depth {
                        let options: MTLBlitOption = format == .depth32Float_stencil8 ? .depthFromDepthStencil : []
                        let bytes = try await gpu.read(depth, bytesPerPixel: 4, options: options)
                        near = "\((0 ..< n * n).filter { bytes.loaded(as: Float.self, at: $0) == 0.25 }.count)"
                    }
                    let cell = "\(Self.key(declared))_\(Self.key(format))"
                    metrics.set(cell, "\(reds)/\(near)/\(String(path.prefix(60)))")
                    let landed = reds == n * n && (depth == nil || near == "\(n * n)")
                    checks.append(("\(cell): every pixel red, depth 0.25, raw", landed && path == "gm20b"))
                } catch {
                    checks.append(("\(Self.key(declared))_\(Self.key(format)): \(GPU.describe(error))", false))
                }
            }
        }
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
