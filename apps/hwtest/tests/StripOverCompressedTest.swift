import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestShared
import Metal

/// RenderBox's icon and widget quads on the driver (boot 21 iconservicesagent: enc.geometry a=4 b=0x100000004 for
/// filter_color and uber, then the command buffer failed): a state that fetches the colour it writes, drawn as an indexed
/// strip of four over a private target on compressed memory, its corners set as vertex bytes before the encoder's first
/// draw. Rule storage.compressed (storage_form.h storageReadsCompressed): the storage form's surface loads and stores
/// reach the target through its compressible kind, so the L2 decompresses for them as for any other client, and the
/// storage form draws inside the pixel interlock over the compressed memory itself: no barrier form taken, no move to
/// plain memory, for the indexed strip and for the control's vertex strip alike. FixedBlendRule.metal rule_fetch_fragment
/// blends source-over its destination four times. A target that never compresses (ShaderWrite) is the reference: a
/// storage access that read or wrote compressed bytes as plain ones differs from it. PASS: every arm raw, bytes alike,
/// no compressed fallback in either compressed arm, and (debug builds, which answer a texture's facts) both compressed
/// targets compressed before and after.
struct StripOverCompressedTest: HWTest {
    let id = "gpu.gm20b.strip_over_compressed"
    let name = "A fetching strip over a compressed private target draws its storage form over the compressed memory"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 90 }

    static let factsSelector = NSSelectorFromString("switchMetalProbeFacts:")
    // A shape no earlier test renders: a storage form that needed one plain makes every later target of it plain
    static let width = 232, height = 88

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let facade = gpu.device as? NSObject
        let debug = facade?.responds(to: Self.factsSelector) == true
        let state = try gpu.render(fragment: "rule_fetch_fragment", format: .bgra8Unorm)
        let indices: [UInt16] = [0, 1, 2, 3]
        guard
            let strip = gpu.device.makeBuffer(
                bytes: indices, length: MemoryLayout<UInt16>.stride * indices.count, options: .storageModeShared
            )
        else { throw TestFailure("no index buffer") }
        var metrics = Metrics()
        var wrong: [String] = []
        var bytes: [String: [UInt8]] = [:]
        for arm in ["control", "indexed", "reference"] {
            let target: MTLTexture
            if arm == "reference" {
                target = try gpu.texture(
                    .bgra8Unorm, Self.width, Self.height, usage: [.renderTarget, .shaderRead, .shaderWrite]
                )
            } else {
                target = try await CompressionSettled.remade(gpu.device) {
                    try gpu.texture(.bgra8Unorm, Self.width, Self.height)
                }
            }
            metrics.set("\(arm)_compressed_before", comptags(facade, target))
            let before = GM20BFacade.counters(of: gpu.device)["fetch_fallback.compressed"] ?? 0
            let path = try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
                pass.colorAttachments[0].storeAction = .store
                let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                e.setRenderPipelineState(state)
                var corners = GPU.fullScreen
                e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                for layer in 0 ..< 4 {
                    var which = SIMD2<UInt32>(UInt32(layer), 1)
                    e.setFragmentBytes(&which, length: 8, index: 0)
                    if arm == "control" {
                        e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    } else {
                        e.drawIndexedPrimitives(
                            type: .triangleStrip, indexCount: 4, indexType: .uint16, indexBuffer: strip,
                            indexBufferOffset: 0
                        )
                    }
                }
                e.endEncoding()
            }.path
            let fallbacks = (GM20BFacade.counters(of: gpu.device)["fetch_fallback.compressed"] ?? 0) - before
            metrics.set("\(arm)_path", path)
            metrics.set("\(arm)_compressed_after", comptags(facade, target))
            metrics.set("\(arm)_compressed_fallbacks", fallbacks)
            if path != "gm20b" { wrong.append("\(arm)_path") }
            bytes[arm] = try await gpu.read(target, bytesPerPixel: 4)
        }
        for arm in ["control", "indexed"] {
            let differ = zip(bytes[arm] ?? [], bytes["reference"] ?? []).filter { $0 != $1 }.count
            metrics.set("\(arm)_bytes_differ", differ)
            if differ > 0 { wrong.append("\(arm)_bytes_differ") }
        }
        for arm in ["control", "indexed"] where metrics["\(arm)_compressed_fallbacks"] != .integer(0) {
            wrong.append("\(arm)_fell_back")
        }
        if debug {
            for arm in ["control", "indexed"] {
                if metrics["\(arm)_compressed_before"] != .integer(1) { wrong.append("\(arm)_target_not_compressed") }
                if metrics["\(arm)_compressed_after"] != .integer(1) { wrong.append("\(arm)_target_moved_plain") }
            }
        }
        return wrong.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(wrong.joined(separator: ","), metrics)
    }

    func comptags(_ facade: NSObject?, _ texture: MTLTexture) -> Int {
        guard let facade, facade.responds(to: Self.factsSelector) else { return 0 }
        let line = facade.perform(Self.factsSelector, with: texture)?.takeUnretainedValue() as? String ?? ""
        return line.contains("comptags=1") ? 1 : 0
    }
}
