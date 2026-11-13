import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Visibility results on the raw path (drivers/TegraGM20BMetal/raw/runtime/raw_visibility.h): Counting offsets are the
/// 3D engine's sample counter, reported per mode set and copied to the offset by the copy engine at the encoder's end.
/// Each arm's values are the samples M4 counts (m2v's visibility.rs rules): draws at one offset add up, an offset set
/// with no draw reads 0, a draw under Disabled counts nowhere, an offset never set keeps its bytes, depth-rejected
/// samples and 4x MSAA samples count as samples. Boolean offsets read 1 or 0, the count's halves each made at most 1 by
/// the copy engine. The same 32 full-target draws under Counting and under Boolean give each mode's GPU time. Every
/// arm's command buffer must stay raw.
struct VisibilityTest: HWTest {
    let id = "gpu.raw.visibility"
    let name = "Visibility results: Counting on the raw path, exact per offset, MSAA and depth"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let size = 256
    static let sentinel: UInt64 = 0xabab_abab_abab_abab
    static let timedDraws = 32

    struct Arm {
        let name: String
        let samples: Int
        let depth: Bool
        let expected: [UInt64?]  // per 8-byte offset; nil: never set, keeps the sentinel
        let encode: (MTLRenderCommandEncoder, MTLRenderPipelineState, MTLDepthStencilState?) -> Void
    }

    static func quad(_ x0: Int, _ y0: Int, _ x1: Int, _ y1: Int, z: Float = 0.5) -> [SIMD4<Float>] {
        GPU.pixelQuad(x0, y0, x1, y1, width: size, height: size, z: z)
    }

    static let red = SIMD4<Float>(1, 0, 0, 1)

    static let arms: [Arm] = [
        Arm(name: "counting", samples: 1, depth: false, expected: [20_000, 0, 2_048, nil]) { e, _, _ in
            e.setVisibilityResultMode(.counting, offset: 0)
            GPU.draw(e, corners: quad(0, 0, 100, 100), color: red)
            GPU.draw(e, corners: quad(50, 50, 150, 150), color: red)
            e.setVisibilityResultMode(.counting, offset: 8)
            e.setVisibilityResultMode(.counting, offset: 16)
            GPU.draw(e, corners: quad(0, 0, 64, 32), color: red)
            e.setVisibilityResultMode(.disabled, offset: 0)
            GPU.draw(e, corners: quad(0, 0, 200, 200), color: red)
        },
        Arm(name: "msaa4", samples: 4, depth: false, expected: [400 * 4, nil, nil, nil]) { e, _, _ in
            e.setVisibilityResultMode(.counting, offset: 0)
            GPU.draw(e, corners: quad(10, 10, 30, 30), color: red)
        },
        Arm(name: "depth", samples: 1, depth: true, expected: [10_000, 5_000, nil, nil]) { e, _, state in
            if let state { e.setDepthStencilState(state) }
            e.setVisibilityResultMode(.counting, offset: 0)
            GPU.draw(e, corners: quad(0, 0, 100, 100, z: 0.25), color: red)
            e.setVisibilityResultMode(.counting, offset: 8)
            GPU.draw(e, corners: quad(50, 0, 150, 100, z: 0.75), color: red)
        },
        Arm(name: "boolean", samples: 1, depth: false, expected: [1, 0, nil, nil]) { e, _, _ in
            e.setVisibilityResultMode(.boolean, offset: 0)
            GPU.draw(e, corners: quad(0, 0, 10, 10), color: red)
            e.setVisibilityResultMode(.boolean, offset: 8)
            e.setVisibilityResultMode(.disabled, offset: 0)
        },
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        var metrics = Metrics()
        var wrong: [String] = []
        let results = try gpu.buffer(32)
        for arm in Self.arms {
            let ran = try await measure(gpu, arm, results)
            metrics.set("\(arm.name)_path", ran.path)
            metrics.set("\(arm.name)_values", ran.values)
            wrong += ran.wrong
            if ran.path != "raw" { wrong.append("\(arm.name)_path") }
        }
        let (rawMs, otherMs, otherPath) = try await timed(gpu, results)
        metrics.set("counting_gpu_ms", rawMs, digits: 3)
        metrics.set("boolean_gpu_ms", otherMs, digits: 3)
        metrics.set("boolean_timed_path", otherPath)
        if otherPath != "raw" { wrong.append("boolean_timed_path") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    /// One arm's command buffer: its path, the four offsets as read ("-" for the sentinel) and the offsets it got wrong
    func measure(
        _ gpu: GPU, _ arm: Arm, _ results: MTLBuffer
    ) async throws
        -> (path: String, values: String, wrong: [String])
    {
        let words = results.contents().bindMemory(to: UInt64.self, capacity: 4)
        for i in 0..<4 { words[i] = Self.sentinel }
        let target = try gpu.texture(.bgra8Unorm, Self.size, Self.size, samples: arm.samples)
        let depth =
            arm.depth
            ? try gpu.texture(.depth32Float, Self.size, Self.size, usage: [.renderTarget], storage: .memoryless)
            : nil
        let state = try gpu.render(
            fragment: "solid_fragment", format: .bgra8Unorm, samples: arm.samples,
            depth: arm.depth ? .depth32Float : .invalid)
        let less: MTLDepthStencilState? =
            arm.depth
            ? {
                let d = MTLDepthStencilDescriptor()
                d.depthCompareFunction = .less
                d.isDepthWriteEnabled = true
                return gpu.device.makeDepthStencilState(descriptor: d)
            }() : nil
        let path = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = arm.samples > 1 ? .dontCare : .store
            if let depth {
                pass.depthAttachment.texture = depth
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.clearDepth = 1
                pass.depthAttachment.storeAction = .dontCare
            }
            pass.visibilityResultBuffer = results
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            arm.encode(encoder, state, less)
            encoder.endEncoding()
        }.path
        var got: [String] = []
        var wrong: [String] = []
        for (i, want) in arm.expected.enumerated() {
            let value = words[i]
            got.append(value == Self.sentinel ? "-" : String(value))
            if value != (want ?? Self.sentinel) { wrong.append("\(arm.name)_\(8 * i)") }
        }
        return (path, got.joined(separator: ","), wrong)
    }

    /// GPU milliseconds of 32 full-target draws under Counting, then the same under Boolean, and the Boolean one's path
    func timed(_ gpu: GPU, _ results: MTLBuffer) async throws -> (Double, Double, String) {
        let target = try gpu.texture(.bgra8Unorm, 1280, 720)
        let state = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        func pass(_ mode: MTLVisibilityResultMode) -> (MTLCommandBuffer) -> Void {
            { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                pass.visibilityResultBuffer = results
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(state)
                encoder.setVisibilityResultMode(mode, offset: 0)
                for _ in 0..<Self.timedDraws { GPU.draw(encoder, corners: GPU.fullScreen, color: Self.red) }
                encoder.endEncoding()
            }
        }
        var counting = 0.0, boolean = 0.0
        var path = ""
        for _ in 0..<3 {
            counting += try await gpu.run(pass(.counting)).gpuMilliseconds
            let other = try await gpu.run(pass(.boolean))
            boolean += other.gpuMilliseconds
            path = other.path
        }
        return (counting / 3, boolean / 3, path)
    }
}
