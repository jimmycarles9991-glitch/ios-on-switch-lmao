import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// A cubic sampler (MTLSamplerMinMagFilterCubic) bound to the vertex stage on the driver: a vertex
/// function reads six coordinates of an 8x4 texture (its rows alike) through it at level 0, one point each, and a fragment
/// function reads the same six through the same sampler. Metal's red channel at those six, an M4 Max's (macOS 27.2.1,
/// either stage): 250, 41, 183, 115, 66, 125, where a linear filter reads 232, 58, 165, 115, 82, 119 (a texture of one row
/// reads linearly there, so the texture has four). Both stages within 2 codes of Metal's, every pass raw.
struct VertexCubicSamplerTest: HWTest {
    let id = "gpu.gm20b.vertex_cubic_sampler"
    let name = "A cubic sampler bound to the vertex stage filters as Metal's does, raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let reds: [UInt8] = [0, 255, 32, 220, 10, 200, 90, 0]
    static let rows = 4
    static let coordinates: [Float] = [0.20, 0.33, 0.47, 0.5, 0.61, 0.78]
    static let metal: [UInt8] = [250, 41, 183, 115, 66, 125]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let n = Self.coordinates.count
        let texture = try gpu.texture(.rgba8Unorm, Self.reds.count, Self.rows, usage: [.shaderRead])
        let texels = Self.reds.flatMap { r -> [UInt8] in [r, 255 - r, 0, 255] }
        try await gpu.upload(
            Array(Array(repeating: texels, count: Self.rows).joined()), to: texture,
            bytesPerRow: 4 * Self.reds.count
        )
        func sampler(_ filter: UInt) throws -> MTLSamplerState {
            let d = MTLSamplerDescriptor()
            d.minFilter = MTLSamplerMinMagFilter(rawValue: filter)!
            d.magFilter = MTLSamplerMinMagFilter(rawValue: filter)!
            d.sAddressMode = .clampToEdge
            d.tAddressMode = .clampToEdge
            guard let s = gpu.device.makeSamplerState(descriptor: d) else { throw TestFailure("no sampler \(filter)") }
            return s
        }
        let cubic = try sampler(2), linear = try sampler(1)
        let points = try gpu.render(vertex: "cubic_vertex", fragment: "cubic_point_fragment", format: .rgba8Unorm) {
            $0.inputPrimitiveTopology = .point
        }
        let row = try gpu.render(vertex: "quad_vertex", fragment: "cubic_row_fragment", format: .rgba8Unorm)
        var paths: [String] = []
        func draw(vertexStage: Bool, _ s: MTLSamplerState) async throws -> [UInt8] {
            let out = try gpu.texture(.rgba8Unorm, n, 1)
            let path = try await gpu.run { commands in
                let p = MTLRenderPassDescriptor()
                p.colorAttachments[0].texture = out
                p.colorAttachments[0].loadAction = .clear
                p.colorAttachments[0].storeAction = .store
                let e = commands.makeRenderCommandEncoder(descriptor: p)!
                var us = Self.coordinates
                if vertexStage {
                    e.setRenderPipelineState(points)
                    e.setVertexTexture(texture, index: 0)
                    e.setVertexSamplerState(s, index: 0)
                    e.setVertexBytes(&us, length: 4 * n, index: 0)
                    var count = UInt32(n)
                    e.setVertexBytes(&count, length: 4, index: 1)
                    e.drawPrimitives(type: .point, vertexStart: 0, vertexCount: n)
                } else {
                    e.setRenderPipelineState(row)
                    var corners = GPU.fullScreen
                    e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
                    e.setFragmentTexture(texture, index: 0)
                    e.setFragmentSamplerState(s, index: 0)
                    e.setFragmentBytes(&us, length: 4 * n, index: 0)
                    e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                e.endEncoding()
            }.path
            paths.append(path)
            let pixels = try await gpu.read(out, bytesPerPixel: 4)
            return (0 ..< n).map { pixels[4 * $0] }
        }
        let vertexCubic = try await draw(vertexStage: true, cubic)
        let fragmentCubic = try await draw(vertexStage: false, cubic)
        let vertexLinear = try await draw(vertexStage: true, linear)
        var metrics = Metrics()
        metrics.set("vertex_cubic", vertexCubic.map(String.init).joined(separator: ","))
        metrics.set("fragment_cubic", fragmentCubic.map(String.init).joined(separator: ","))
        metrics.set("vertex_linear", vertexLinear.map(String.init).joined(separator: ","))
        metrics.set("paths", paths.joined(separator: ","))
        var failures: [String] = []
        if paths.contains(where: { $0 != "gm20b" }) { failures.append("a pass went off the driver") }
        for (stage, values) in [("vertex", vertexCubic), ("fragment", fragmentCubic)]
            where zip(values, Self.metal).contains(where: { abs(Int($0) - Int($1)) > 2 }) {
            failures.append("the \(stage) stage's cubic samples are not Metal's")
        }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }
}
