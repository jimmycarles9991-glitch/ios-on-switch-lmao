import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A texture buffer as a colour target: a 256-texel RGBA8Unorm texture buffer of RenderTarget and ShaderRead usage over
/// a shared buffer of 0x11 bytes, a pass cleared red: every word 0xff0000ff. A 20000-texel one, wider than a render
/// target's 16384: its command buffer fails MTLCommandBufferErrorInternal and no word changes. Tolerance 0. An M4 Max
/// (m2v-passes3 probe/p3probe.mm texture_buffer and unspecialized_views, out3.txt and out4.txt: status 5 error 1; this
/// scene on an M4 Max: 0 words off, wide_status 1, 0 words changed). row_off words not red, wide_status the wide
/// command buffer's error code (0 completed), wide_changed its words changed; every other command buffer raw, and the
/// wide one the only failure, the facade's pass.fmt (failed_why=).
struct TextureBufferTargetTest: HWTest {
    let id = "gpu.gm20b.texture_buffer_target"
    let name = "A pass clears a texture buffer as a pitch row and fails one past 16384 texels, as Apple's GPU, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let row = 256, wide = 20000
    static let red: UInt32 = 0xff00_00ff

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        do {
            let (code, words) = try await Self.cleared(gpu, texels: Self.row, usage: [.renderTarget, .shaderRead])
            let off = words.filter { $0 != Self.red }.count
            metrics.set("row_off", off)
            checks.append(("row: every word red", code == 0 && off == 0))
        } catch {
            checks.append(("row: \(GPU.describe(error))", false))
        }
        do {
            let (code, words) = try await Self.cleared(gpu, texels: Self.wide, usage: [.renderTarget])
            let changed = words.filter { $0 != 0x1111_1111 }.count
            metrics.set("wide_status", code)
            metrics.set("wide_changed", changed)
            checks.append(("wide: the command buffer fails Internal", code == MTLCommandBufferError.internal.rawValue))
            checks.append(("wide: no word changed", changed == 0))
        } catch {
            checks.append(("wide: \(GPU.describe(error))", false))
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(
            FailedCounts.judged(before: before, after: counted, expecting: ["pass.fmt": 1], own: gpu.paths, &metrics)
        )
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func errorCode(_ gpu: GPU, _ encode: (MTLCommandBuffer) throws -> Void) throws -> Int {
        let buffer = try gpu.commit(encode)
        _ = try? gpu.finish([buffer])
        return buffer.status == .error ? (buffer.error as NSError?)?.code ?? -1 : 0
    }

    static func cleared(_ gpu: GPU, texels: Int, usage: MTLTextureUsage) async throws -> (code: Int, words: [UInt32]) {
        let buffer = try gpu.buffer([UInt8](repeating: 0x11, count: texels * 4))
        let d = MTLTextureDescriptor.textureBufferDescriptor(
            with: .rgba8Unorm, width: texels, resourceOptions: .storageModeShared, usage: usage
        )
        guard let texture = buffer.makeTexture(descriptor: d, offset: 0, bytesPerRow: texels * 4) else {
            throw TestFailure("no texture buffer of \(texels)")
        }
        let code = try errorCode(gpu) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 1, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        let words = UnsafeBufferPointer(
            start: buffer.contents().bindMemory(to: UInt32.self, capacity: texels), count: texels
        )
        return (code, Array(words))
    }
}
