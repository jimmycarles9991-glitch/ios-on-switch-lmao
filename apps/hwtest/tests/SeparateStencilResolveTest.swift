import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A 4x Depth32Float beside a 4x Stencil8, both memoryless or both private, each sample drawn alone
/// (masked_texel_values' sample mask): sample s replaces the stencil with s + 1 at depths 0.5/0.25/0.75/0.625 over the
/// left half and 0.875/0.625/0.375/0.125 over the right. MultisampleResolve resolves the depth into a one-sample
/// Depth32Float (Sample0, Min, Max) and the stencil into a one-sample Stencil8 (Sample0, or DepthResolvedSample beside
/// Min and Max), the Stencil8 read back by a blit into a buffer. The stencil is the picked sample's: sample0 1/1, min
/// 2/4, max 3/1 (left/right), and the depth the picked sample's, as Metal's resolve filters define them and an M4 Max
/// stores every texel of this scene. Per case <storage>_<filter>_stencil (left/right), _stencil_off and _depth_off
/// texels off; every command buffer raw, failed_cbs=0. Stencil resolve is Apple5's: SKIP below it.
struct SeparateStencilResolveTest: HWTest {
    let id = "gpu.gm20b.separate_stencil_resolve"
    let name = "A multisampled Stencil8 beside a Depth32Float resolves the sample its filter picks, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let n = 16
    static let depths: [[Float]] = [[0.5, 0.25, 0.75, 0.625], [0.875, 0.625, 0.375, 0.125]]
    static let storages: [(key: String, mode: MTLStorageMode)] = [("memoryless", .memoryless), ("private", .private)]
    static let filters = [
        Filter(key: "sample0", depth: .sample0, stencil: .sample0),
        Filter(key: "min", depth: .min, stencil: .depthResolvedSample),
        Filter(key: "max", depth: .max, stencil: .depthResolvedSample),
    ]

    struct Filter {
        let key: String
        let depth: MTLMultisampleDepthResolveFilter
        let stencil: MTLMultisampleStencilResolveFilter
    }

    struct Result {
        var stencil: [UInt8] = []
        var depth: [Float] = []
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        guard gpu.device.supportsFamily(.apple5) else { return .skip("the device resolves no stencil (Apple5)") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for storage in Self.storages {
            for filter in Self.filters {
                let key = "\(storage.key)_\(filter.key)"
                do {
                    let result = try await Self.resolved(gpu, storage.mode, filter.depth, filter.stencil)
                    let off = Self.off(result, filter.depth)
                    let row = Self.n * Self.n / 2
                    metrics.set(key + "_stencil", "\(result.stencil[row + 2])/\(result.stencil[row + Self.n - 2])")
                    metrics.set(key + "_stencil_off", off.stencil)
                    metrics.set(key + "_depth_off", off.depth)
                    checks.append(("\(key): the picked sample's stencil and depth", off.stencil + off.depth == 0))
                } catch {
                    checks.append(("\(key): \(error)", false))
                }
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// The sample `filter` picks over `half` (0 left, 1 right)
    static func picked(_ filter: MTLMultisampleDepthResolveFilter, half: Int) -> Int {
        let z = depths[half]
        switch filter {
        case .min: return z.indices.min { z[$0] < z[$1] }!
        case .max: return z.indices.max { z[$0] < z[$1] }!
        default: return 0
        }
    }

    static func off(_ result: Result, _ filter: MTLMultisampleDepthResolveFilter) -> (stencil: Int, depth: Int) {
        var off = (stencil: 0, depth: 0)
        for at in 0 ..< n * n {
            let half = at % n < n / 2 ? 0 : 1
            let sample = picked(filter, half: half)
            off.stencil += Int(result.stencil[at]) == sample + 1 ? 0 : 1
            off.depth += result.depth[at] == depths[half][sample] ? 0 : 1
        }
        return off
    }

    static func resolved(
        _ gpu: GPU, _ storage: MTLStorageMode, _ depthFilter: MTLMultisampleDepthResolveFilter,
        _ stencilFilter: MTLMultisampleStencilResolveFilter
    ) async throws -> Result {
        let colour = try gpu.texture(.rgba8Unorm, n, n, usage: [.renderTarget], samples: 4)
        let depth = try gpu.texture(.depth32Float, n, n, usage: [.renderTarget], samples: 4, storage: storage)
        let stencil = try gpu.texture(.stencil8, n, n, usage: [.renderTarget], samples: 4, storage: storage)
        let depthOut = try gpu.texture(.depth32Float, n, n, usage: [.renderTarget])
        let stencilOut = try gpu.texture(.stencil8, n, n, usage: [.renderTarget])
        let state = try gpu.render(
            fragment: "masked_texel_values", format: .rgba8Unorm, samples: 4, depth: .depth32Float, stencil: .stencil8
        )
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .always
        d.isDepthWriteEnabled = true
        let face = MTLStencilDescriptor()
        face.depthStencilPassOperation = .replace
        d.frontFaceStencil = face
        d.backFaceStencil = face
        guard let write = gpu.device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no depth state") }
        let values = try gpu.buffer([SIMD4<Float>](repeating: SIMD4(1, 0, 0, 1), count: n * n))
        try await gpu.run { commands in
            let p = MTLRenderPassDescriptor()
            p.colorAttachments[0].texture = colour
            p.colorAttachments[0].loadAction = .clear
            p.colorAttachments[0].storeAction = .dontCare
            p.depthAttachment.texture = depth
            p.depthAttachment.loadAction = .clear
            p.depthAttachment.clearDepth = 1
            p.depthAttachment.storeAction = .multisampleResolve
            p.depthAttachment.resolveTexture = depthOut
            p.depthAttachment.depthResolveFilter = depthFilter
            p.stencilAttachment.texture = stencil
            p.stencilAttachment.loadAction = .clear
            p.stencilAttachment.storeAction = .multisampleResolve
            p.stencilAttachment.resolveTexture = stencilOut
            p.stencilAttachment.stencilResolveFilter = stencilFilter
            let e = commands.makeRenderCommandEncoder(descriptor: p)!
            e.setRenderPipelineState(state)
            e.setDepthStencilState(write)
            e.setFragmentBuffer(values, offset: 0, index: 0)
            for sample in 0 ..< 4 {
                var at = SIMD2<UInt32>(UInt32(n), UInt32(sample))
                e.setFragmentBytes(&at, length: 8, index: 1)
                e.setStencilReferenceValue(UInt32(sample + 1))
                for half in 0 ..< 2 {
                    var corners = GPU.pixelQuad(
                        half * n / 2, 0, (half + 1) * n / 2, n, width: n, height: n, z: depths[half][sample]
                    )
                    e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
            }
            e.endEncoding()
        }
        let depths = try await gpu.read(depthOut, bytesPerPixel: 4)
        return Result(
            stencil: try await gpu.read(stencilOut, bytesPerPixel: 1),
            depth: (0 ..< n * n).map { depths.loaded(as: Float.self, at: $0) }
        )
    }
}
