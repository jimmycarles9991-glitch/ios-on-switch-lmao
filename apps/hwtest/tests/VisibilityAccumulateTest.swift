import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// Accumulating visibility results (MTLVisibilityResultTypeAccumulate, iOS 26) on the driver
/// (drivers/TegraGM20BMetal/visibility.h): Counting adds to an offset's low word modulo 2^32 and keeps
/// its high word, through the counter's report reducing into the offset; Boolean ORs 1 into the low word where a sample
/// passed, by a copy-engine release that render enable gates on the count. An offset set with no draw keeps its bytes.
/// Each offset starts from a preset; the values are an M4 Max's (macOS 27.2.1). Two passes in a row add up. Every
/// command buffer must stay raw.
struct VisibilityAccumulateTest: HWTest {
    let id = "gpu.gm20b.visibility_accumulate"
    let name = "Visibility results accumulated across passes: Counting adds, Boolean ORs 1, on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let size = 128
    static let red = SIMD4<Float>(1, 0, 0, 1)

    struct Arm {
        let name: String
        let preset: [UInt64]
        let expected: [UInt64]
        let passes: Int
        let encode: (MTLRenderCommandEncoder) -> Void
    }

    static func quad(_ x0: Int, _ y0: Int, _ x1: Int, _ y1: Int) -> [SIMD4<Float>] {
        GPU.pixelQuad(x0, y0, x1, y1, width: size, height: size)
    }

    static let arms: [Arm] = [
        Arm(
            name: "counting", preset: [1000, 7, 0xabab, 0xffff_ffff_ffff_fff0, 0xffff_ffff, 0x1_0000_0000],
            expected: [11_000, 7, 0xabab, 0xffff_ffff_0000_0054, 0, 0x1_0000_0010], passes: 1
        ) { e in
            e.setVisibilityResultMode(.counting, offset: 0)
            GPU.draw(e, corners: quad(0, 0, 100, 100), color: red)
            e.setVisibilityResultMode(.counting, offset: 8)
            e.setVisibilityResultMode(.counting, offset: 24)
            GPU.draw(e, corners: quad(0, 0, 10, 10), color: red)
            e.setVisibilityResultMode(.counting, offset: 32)
            GPU.draw(e, corners: quad(0, 0, 1, 1), color: red)
            e.setVisibilityResultMode(.counting, offset: 40)
            GPU.draw(e, corners: quad(0, 0, 4, 4), color: red)
            e.setVisibilityResultMode(.disabled, offset: 0)
            GPU.draw(e, corners: quad(0, 0, 50, 50), color: red)
        },
        Arm(
            name: "boolean", preset: [5, 2, 0x1_0000_0000, 0, 3, 0],
            expected: [5, 3, 0x1_0000_0001, 1, 3, 0], passes: 1
        ) { e in
            e.setVisibilityResultMode(.boolean, offset: 0)
            GPU.draw(e, corners: quad(0, 0, 10, 10), color: red)
            e.setVisibilityResultMode(.boolean, offset: 8)
            GPU.draw(e, corners: quad(0, 0, 10, 10), color: red)
            e.setVisibilityResultMode(.boolean, offset: 16)
            GPU.draw(e, corners: quad(0, 0, 1, 1), color: red)
            e.setVisibilityResultMode(.boolean, offset: 24)
            GPU.draw(e, corners: quad(0, 0, 64, 64), color: red)
            e.setVisibilityResultMode(.boolean, offset: 32)
            e.setVisibilityResultMode(.boolean, offset: 40)
            e.setVisibilityResultMode(.disabled, offset: 0)
        },
        Arm(name: "twice", preset: [0, 0, 0, 0, 0, 0], expected: [20_000, 1, 0, 0, 0, 0], passes: 2) { e in
            e.setVisibilityResultMode(.counting, offset: 0)
            GPU.draw(e, corners: quad(0, 0, 100, 100), color: red)
            e.setVisibilityResultMode(.boolean, offset: 8)
            GPU.draw(e, corners: quad(0, 0, 100, 100), color: red)
            e.setVisibilityResultMode(.boolean, offset: 16)
        },
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        guard #available(iOS 26, *) else { return .skip("MTLVisibilityResultTypeAccumulate is iOS 26's") }
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let results = try gpu.buffer(48)
        let target = try gpu.texture(.bgra8Unorm, Self.size, Self.size)
        let state = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        var metrics = Metrics()
        var wrong: [String] = []
        for arm in Self.arms {
            let words = results.contents().bindMemory(to: UInt64.self, capacity: 6)
            for (i, v) in arm.preset.enumerated() { words[i] = v }
            var paths: [String] = []
            for _ in 0 ..< arm.passes {
                paths.append(
                    try await gpu.run { commands in
                        let pass = MTLRenderPassDescriptor()
                        pass.colorAttachments[0].texture = target
                        pass.colorAttachments[0].loadAction = .clear
                        pass.colorAttachments[0].storeAction = .store
                        pass.visibilityResultBuffer = results
                        pass.visibilityResultType = .accumulate
                        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                        encoder.setRenderPipelineState(state)
                        arm.encode(encoder)
                        encoder.endEncoding()
                    }.path
                )
            }
            let got = (0 ..< 6).map { words[$0] }
            metrics.set("\(arm.name)_values", got.map { String($0, radix: 16) }.joined(separator: ","))
            metrics.set("\(arm.name)_paths", paths.joined(separator: ","))
            for (i, want) in arm.expected.enumerated() where got[i] != want { wrong.append("\(arm.name)_\(8 * i)") }
            if paths.contains(where: { $0 != "gm20b" }) { wrong.append("\(arm.name)_path") }
        }
        return wrong.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(wrong.joined(separator: ","), metrics)
    }
}
