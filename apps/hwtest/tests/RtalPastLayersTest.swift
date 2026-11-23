import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// renderTargetArrayLength past the texture's layers: a 16x16 BGRA8Unorm array of two slices, every byte 0x11, a pass
/// at slice 1 with renderTargetArrayLength 2 cleared to blue, an instanced draw (layered_vertex) of red into layer 0
/// and green into layer 1 over the whole target. The command buffer completes, slice 1 is red and slice 0 keeps 0x11
/// everywhere: the draw into the layer past the texture lands nowhere, which checks that the GM20B drops a layer index
/// past RT_ARRAY_MODE rather than redirecting it (the spec's; an M4 Max, this scene: slice 0 0x11, slice 1 red).
/// layers= each slice's contents, slice 0 first: p 0x11, r red, g green, b blue, x anything else; slice<n>_off texels
/// not as expected, completed=, error= the command buffer's when it failed; every command buffer raw, failed_cbs=0.
struct RtalPastLayersTest: HWTest {
    let id = "gpu.gm20b.rtal_past_layers"
    let name = "A layered pass past the texture's layers draws its own layers and drops the one past them, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let side = 16
    static let texels: [String: [UInt8]] = [
        "p": [0x11, 0x11, 0x11, 0x11], "r": [0, 0, 255, 255], "g": [0, 255, 0, 255], "b": [255, 0, 0, 255],
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        do {
            let (path, slices) = try await Self.drawn(gpu)
            let completed = !path.hasPrefix("error:")
            metrics.set("layers", slices.map(Self.contents).joined(separator: "/"))
            for (slice, want) in zip(slices.indices, ["p", "r"]) {
                metrics.set("slice\(slice)_off", Self.off(slices[slice], Self.texels[want]!))
            }
            metrics.set("completed", completed)
            if !completed { metrics.set("error", String(path.dropFirst("error:".count).prefix(120))) }
            checks.append(("the command buffer completed", completed))
            checks.append(("slice 0 keeps its bytes", Self.contents(slices[0]) == "p"))
            checks.append(("slice 1 holds layer 0's draw", Self.contents(slices[1]) == "r"))
        } catch {
            checks.append(("the pass: \(GPU.describe(error))", false))
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func off(_ bytes: [UInt8], _ texel: [UInt8]) -> Int {
        stride(from: 0, to: bytes.count, by: 4).filter { Array(bytes[$0 ..< $0 + 4]) != texel }.count
    }

    static func contents(_ bytes: [UInt8]) -> String {
        texels.first { off(bytes, $0.value) == 0 }?.key ?? "x"
    }

    static func drawn(_ gpu: GPU) async throws -> (path: String, slices: [[UInt8]]) {
        let d = MTLTextureDescriptor()
        d.textureType = .type2DArray
        d.pixelFormat = .bgra8Unorm
        (d.width, d.height, d.arrayLength) = (side, side, 2)
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = .private
        guard let array = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no BGRA8 array") }
        for slice in 0 ..< 2 {
            try await gpu.upload(
                [UInt8](repeating: 0x11, count: side * side * 4), to: array, bytesPerRow: side * 4, slice: slice
            )
        }
        let state = try gpu.render(vertex: "layered_vertex", fragment: "flat_color_fragment", format: .bgra8Unorm) {
            $0.inputPrimitiveTopology = .triangle
        }
        let timing = try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = array
            pass.colorAttachments[0].slice = 1
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 1, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            pass.renderTargetArrayLength = 2
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            var colours: [SIMD4<Float>] = [SIMD4(1, 0, 0, 1), SIMD4(0, 1, 0, 1)]
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setVertexBytes(&colours, length: MemoryLayout<SIMD4<Float>>.stride * 2, index: 1)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4, instanceCount: 2)
            encoder.endEncoding()
        }
        var slices: [[UInt8]] = []
        for slice in 0 ..< 2 { slices.append(try await gpu.read(array, bytesPerPixel: 4, slice: slice)) }
        return (timing.path, slices)
    }
}
