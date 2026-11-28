import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// A 4x Depth16Unorm beside a 4x Stencil8 resolved into a one-sample Depth16Unorm (sample 0). The driver renders the
/// pair through a Depth32Float_Stencil8 of its own, so the resolve must read that, not the Depth16Unorm, which the pass
/// never writes before its end. The depth is cleared to 1 and a quad at 0.25 drawn over the left half, resolved into a
/// Depth16Unorm the test cleared to 0.75 first, once with MultisampleResolve and once with StoreAndMultisampleResolve;
/// after the second a pass loads the kept 4x depth and resolves it again into another such texture. Left: 0.25's code
/// (16383 or 16384, the two roundings of 16383.75); right: 65535; 49151 (0.75's code) where nothing was resolved.
/// Fields per resolve (resolve, store_resolve, reload): <r>_left and <r>_right one row's middle codes, <r>_wrong the
/// codes off, <r>_kept the codes still 0.75's; reload_differs the codes the reload's resolve does not share with
/// store_resolve's; paths=.
struct Depth16MultisampleResolveTest: HWTest {
    let id = "gpu.gm20b.depth16_ms_resolve"
    let name = "A multisampled Depth16Unorm and Stencil8 pair resolves the depth its pass drew, on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let n = 16, samples = 4
    static let left: Set<UInt16> = [16383, 16384]
    static let right: UInt16 = 65535, untouched: UInt16 = 49151

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let (n, samples) = (Self.n, Self.samples)
        let colour = try gpu.texture(.rgba8Unorm, n, n, samples: samples)
        let depth = try gpu.texture(.depth16Unorm, n, n, usage: [.renderTarget], samples: samples)
        let stencil = try gpu.texture(.stencil8, n, n, usage: [.renderTarget], samples: samples)
        let state = try gpu.render(
            fragment: "solid_fragment", format: .rgba8Unorm, samples: samples, depth: .depth16Unorm, stencil: .stencil8
        )
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .always
        d.isDepthWriteEnabled = true
        guard let write = gpu.device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no depth state") }
        var paths: [String] = []
        func resolved() async throws -> MTLTexture {
            let out = try gpu.texture(.depth16Unorm, n, n, usage: [.renderTarget])
            paths.append(
                try await gpu.run { commands in
                    let p = MTLRenderPassDescriptor()
                    p.depthAttachment.texture = out
                    p.depthAttachment.loadAction = .clear
                    p.depthAttachment.clearDepth = 0.75
                    p.depthAttachment.storeAction = .store
                    commands.makeRenderCommandEncoder(descriptor: p)!.endEncoding()
                }.path
            )
            return out
        }
        func pass(_ commands: MTLCommandBuffer, load: Bool, store: MTLStoreAction, into out: MTLTexture) {
            let p = MTLRenderPassDescriptor()
            p.colorAttachments[0].texture = colour
            p.colorAttachments[0].loadAction = .clear
            p.colorAttachments[0].storeAction = .dontCare
            p.depthAttachment.texture = depth
            p.depthAttachment.loadAction = load ? .load : .clear
            p.depthAttachment.clearDepth = 1
            p.depthAttachment.storeAction = store
            p.depthAttachment.resolveTexture = out
            p.depthAttachment.depthResolveFilter = .sample0
            p.stencilAttachment.texture = stencil
            p.stencilAttachment.loadAction = .clear
            p.stencilAttachment.storeAction = .dontCare
            let e = commands.makeRenderCommandEncoder(descriptor: p)!
            if !load {
                e.setRenderPipelineState(state)
                e.setDepthStencilState(write)
                var corners: [SIMD4<Float>] = [[-1, -1, 0.25, 1], [0, -1, 0.25, 1], [-1, 1, 0.25, 1], [0, 1, 0.25, 1]]
                e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                var red = SIMD4<Float>(1, 0, 0, 1)
                e.setFragmentBytes(&red, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            e.endEncoding()
        }
        func codes(_ texture: MTLTexture) async throws -> [UInt16] {
            let bytes = try await gpu.read(texture, bytesPerPixel: 2)
            return (0 ..< n * n).map { UInt16(bytes[2 * $0]) | UInt16(bytes[2 * $0 + 1]) << 8 }
        }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        var read: [String: [UInt16]] = [:]
        // reload loads the 4x depth store_resolve kept
        let resolves: [(String, Bool, MTLStoreAction)] = [
            ("resolve", false, .multisampleResolve), ("store_resolve", false, .storeAndMultisampleResolve),
            ("reload", true, .multisampleResolve),
        ]
        for (key, load, store) in resolves {
            let out = try await resolved()
            paths.append(try await gpu.run(tolerateError: true) { pass($0, load: load, store: store, into: out) }.path)
            let got = try await codes(out)
            read[key] = got
            var wrong = 0, kept = 0
            for (at, code) in got.enumerated() {
                wrong += (at % n < n / 2 ? Self.left.contains(code) : code == Self.right) ? 0 : 1
                kept += code == Self.untouched ? 1 : 0
            }
            metrics.set("\(key)_left", Int(got[n * (n / 2) + n / 4]))
            metrics.set("\(key)_right", Int(got[n * (n / 2) + 3 * n / 4]))
            metrics.set("\(key)_wrong", wrong)
            metrics.set("\(key)_kept", kept)
            checks.append(("\(key): 0.25's code left, 1's right, none still 0.75's", wrong == 0 && kept == 0))
        }
        let reloadDiffers = zip(read["store_resolve"] ?? [], read["reload"] ?? []).filter { $0 != $1 }.count
        metrics.set("reload_differs", reloadDiffers)
        let raw = paths.allSatisfy { $0 == "gm20b" }
        metrics.set("paths", raw ? "gm20b" : String(paths.joined(separator: ",").prefix(120)))
        checks.append(("the kept depth resolves as the pass that drew it did", reloadDiffers == 0))
        checks.append(("every pass on the driver", raw))
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
