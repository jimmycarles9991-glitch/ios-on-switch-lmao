import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// A Depth16Unorm attachment whose draws' depth test runs in their shader on the driver (a fetching state with a
/// depth format: fetch_add_fragment), which renders the pass through a converted Depth32Float_Stencil8. 64x64, loaded
/// with a ramp: each band of 16 rows 1024 consecutive codes, the band's draw depth's code at texel 512 (the last for
/// 1.0). One draw a band at depth 1000/65535, 0.3, 0.7 and 1.0 under LessEqual with writes adds red 0.25; then a
/// full-screen draw under a state that tests stencil Equal 0 (no stencil attachment: Apple's tile and raw's conversion
/// hold 0) adds green 0.5; Store. The stored depth and colour bytes hash to an M4 Max's (this scene, measured; FNV-1a
/// 64): each draw's depth taken to its nearest 16-bit code, tested and stored as that code (0.7 is 45874 and passes
/// over a ramp texel of 45874), every other ramp code kept through the conversion. Per band b<k>_passed texels the
/// first draw reached and b<k>_code the code it stored (an M4 Max: 512/1000, 512/19661, 512/45874, 1/65535),
/// kept_changed the texels it did not reach whose code is not the ramp's (0), stencil_passed texels the stencil draw
/// reached (4096); every command buffer raw, failed_cbs=0.
struct Depth16ShaderTestsTest: HWTest {
    let id = "gpu.gm20b.depth16_shader_tests"
    let name = "A Depth16Unorm a fetching draw tests in its shader stores Metal's codes, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let side = 64, bandRows = 16
    static let depths: [Float] = [1000.0 / 65535, 0.3, 0.7, 1]
    static let metalDepth: UInt64 = 0xe2bd_5426_cfb4_2107
    static let metalColour: UInt64 = 0x673e_4bb1_fde1_3ae5

    struct Result {
        var depth: [UInt8] = []
        var colour: [UInt8] = []
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        do {
            let result = try await Self.drawn(gpu)
            let depth = YcbcrLoadTest.hash([result.depth]), colour = YcbcrLoadTest.hash([result.colour])
            metrics.merge(Self.fields(result))
            metrics.set("depth_hash", String(depth, radix: 16))
            metrics.set("colour_hash", String(colour, radix: 16))
            checks.append(("the stored depth codes Metal's", depth == Self.metalDepth))
            checks.append(("the stored colour Metal's (both tests pass where Metal's do)", colour == Self.metalColour))
        } catch {
            checks.append(("the pass: \(error)", false))
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func ramp() -> [UInt16] {
        let band = side * bandRows
        return depths.flatMap { z -> [UInt16] in
            let start = min(Int((Double(z) * 65535).rounded()) - band / 2, 65535 - (band - 1))
            return (0 ..< band).map { UInt16(start + $0) }
        }
    }

    static func fields(_ result: Result) -> Metrics {
        var metrics = Metrics()
        let band = side * bandRows
        let codes = (0 ..< side * side).map { result.depth.loaded(as: UInt16.self, at: $0) }
        let reached = (0 ..< side * side).map { result.colour[4 * $0] != 0 }
        for b in depths.indices {
            let passed = (b * band ..< (b + 1) * band).filter { reached[$0] }
            metrics.set("b\(b)_passed", passed.count)
            metrics.set("b\(b)_code", passed.last.map { Int(codes[$0]) } ?? -1)
        }
        let ramp = ramp()
        metrics.set("kept_changed", codes.indices.filter { !reached[$0] && codes[$0] != ramp[$0] }.count)
        metrics.set("stencil_passed", (0 ..< side * side).filter { result.colour[4 * $0 + 1] != 0 }.count)
        return metrics
    }

    static func drawn(_ gpu: GPU) async throws -> Result {
        let n = side
        let depth = try gpu.texture(.depth16Unorm, n, n, usage: [.renderTarget])
        let colour = try gpu.texture(.rgba8Unorm, n, n, usage: [.renderTarget])
        try await gpu.upload(
            ramp().flatMap { [UInt8(truncatingIfNeeded: $0), UInt8($0 >> 8)] }, to: depth, bytesPerRow: 2 * n
        )
        let state = try gpu.render(
            vertex: "fetch_depth_vertex", fragment: "fetch_add_fragment", format: .rgba8Unorm, depth: .depth16Unorm
        )
        let tested = MTLDepthStencilDescriptor()
        tested.depthCompareFunction = .lessEqual
        tested.isDepthWriteEnabled = true
        let stencil = MTLDepthStencilDescriptor()
        let face = MTLStencilDescriptor()
        face.stencilCompareFunction = .equal
        stencil.frontFaceStencil = face
        stencil.backFaceStencil = face
        guard let depthTest = gpu.device.makeDepthStencilState(descriptor: tested),
              let stencilTest = gpu.device.makeDepthStencilState(descriptor: stencil)
        else { throw TestFailure("no depth/stencil state") }
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = colour
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = depth
            pass.depthAttachment.loadAction = .load
            pass.depthAttachment.storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            func draw(_ tests: MTLDepthStencilState, z: Float, add: SIMD4<Float>, rows: Range<Int>) {
                var z = z
                var add = add
                encoder.setDepthStencilState(tests)
                encoder.setScissorRect(MTLScissorRect(x: 0, y: rows.lowerBound, width: n, height: rows.count))
                encoder.setVertexBytes(&z, length: 4, index: 0)
                encoder.setFragmentBytes(&add, length: 16, index: 0)
                encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            }
            for (band, z) in depths.enumerated() {
                draw(depthTest, z: z, add: SIMD4(0.25, 0, 0, 1), rows: band * bandRows ..< (band + 1) * bandRows)
            }
            draw(stencilTest, z: 0.5, add: SIMD4(0, 0.5, 0, 1), rows: 0 ..< n)
            encoder.endEncoding()
        }
        return Result(
            depth: try await gpu.read(depth, bytesPerPixel: 2), colour: try await gpu.read(colour, bytesPerPixel: 4)
        )
    }
}
