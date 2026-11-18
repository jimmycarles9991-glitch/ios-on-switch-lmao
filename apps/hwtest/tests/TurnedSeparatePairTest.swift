import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A Depth32Float or Depth16Unorm texture and a Stencil8 texture as the depth and stencil attachments of passes into a
/// turned drawable (rotation SPI, rotations 1 to 4): the first pass writes stencil 0xA5 at depth 0.25 in one rectangle and
/// 0x5A at 0.5 behind it and stores both, the second loads them and draws red where the stencil is 0xA5, green where 0x5A,
/// then blue alone past depth 0.375. Each texture is moved into a stand-in of the plane by turn copies (raw_turned_pass.h);
/// before, the pass went to m2v (pass.ds: a turned separate pair). wrong_r<turn>_<cell> counts frame pixels off Metal's
/// store of the same passes into a plain texture (0 to pass), path_r<turn>_<cell> the turned run's path (raw to pass).
struct TurnedSeparatePairTest: HWTest {
    let id = "gpu.raw.turned_separate_pair"
    let name = "A separate depth and stencil pair beside a turned drawable, every rotation, on the raw path"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        let states = try RotatedStates(gpu)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for turn in 1...4 {
            let cells = RotatedCells(gpu: gpu, states: states, turn: turn, width: 96, height: 64, aligned: true)
            guard let plane = try cells.makePlane() else {
                return .fail("the device makes no texture with the rotation SPI", metrics, proof: gpu.proof)
            }
            for cell in RotatedCell.pairCells {
                let key = "r\(turn)_\(cell.rawValue)"
                let result = try await cells.run(cell, into: plane)
                metrics.set("wrong_" + key, result.wrong)
                metrics.set("path_" + key, result.path)
                checks.append(("\(key): Metal's store, on the raw path", result.wrong == 0 && result.path == "raw"))
            }
        }
        return .judged(checks, metrics, proof: gpu.proof)
    }
}

extension RotatedCells {
    /// keptStencil's two passes over a depth texture of `depthFormat` and a Stencil8 texture of the frame
    func keptPair(_ depthFormat: MTLPixelFormat) -> Pass {
        { commands, target in
            guard let depth = try? gpu.texture(depthFormat, width, height, usage: .renderTarget),
                let stencil = try? gpu.texture(.stencil8, width, height, usage: .renderTarget),
                let tested = try? gpu.render(
                    vertex: "rotated_vertex", fragment: "rotated_fragment", format: .bgra8Unorm, depth: depthFormat,
                    stencil: .stencil8),
                let blue = try? gpu.render(
                    vertex: "rotated_vertex", fragment: "rotated_fragment", format: .bgra8Unorm, depth: depthFormat,
                    stencil: .stencil8, configure: { $0.colorAttachments[0].writeMask = .blue }),
                let writes = pairState(.lessEqual, writes: true, stencil: .always, replace: true),
                let equal = pairState(.always, writes: false, stencil: .equal, replace: false),
                let beyond = pairState(.lessEqual, writes: false, stencil: .always, replace: false)
            else { return }
            let at = { (r: [SIMD4<Float>], z: Float) in r.map { SIMD4<Float>($0.x, $0.y, z, 1) } }
            let whole = rect(0, 0, width, height)
            for (n, load) in [MTLLoadAction.clear, .load].enumerated() {
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
                pass.colorAttachments[0].storeAction = .store
                pass.depthAttachment.texture = depth
                pass.depthAttachment.loadAction = load
                pass.depthAttachment.clearDepth = 1
                pass.depthAttachment.storeAction = n == 0 ? .store : .dontCare
                pass.stencilAttachment.texture = stencil
                pass.stencilAttachment.loadAction = load
                pass.stencilAttachment.clearStencil = 0
                pass.stencilAttachment.storeAction = n == 0 ? .store : .dontCare
                let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                if n == 0 {
                    e.setDepthStencilState(writes)
                    e.setStencilReferenceValue(0xA5)
                    shapes(e, at(rect(10, 5, 50, 45), 0.25), [1, 1, 0, 1], tested)
                    e.setStencilReferenceValue(0x5A)
                    shapes(e, at(rect(20, 10, 80, 55), 0.5), [0, 1, 1, 1], tested)
                } else {
                    e.setDepthStencilState(equal)
                    e.setStencilReferenceValue(0xA5)
                    shapes(e, at(whole, 0.9), [1, 0, 0, 1], tested)
                    e.setStencilReferenceValue(0x5A)
                    shapes(e, at(whole, 0.9), [0, 1, 0, 1], tested)
                    e.setDepthStencilState(beyond)
                    shapes(e, at(whole, 0.375), [0, 0, 1, 1], blue)
                }
                e.endEncoding()
            }
        }
    }

    private func pairState(
        _ depth: MTLCompareFunction, writes: Bool, stencil: MTLCompareFunction, replace: Bool
    ) -> MTLDepthStencilState? {
        let face = MTLStencilDescriptor()
        face.stencilCompareFunction = stencil
        face.depthStencilPassOperation = replace ? .replace : .keep
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = depth
        d.isDepthWriteEnabled = writes
        d.frontFaceStencil = face
        d.backFaceStencil = face
        return gpu.device.makeDepthStencilState(descriptor: d)
    }
}
