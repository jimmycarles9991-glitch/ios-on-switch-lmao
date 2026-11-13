import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Rule storage.compressed (drivers/TegraGM20BMetal/raw/runtime/raw_storage_form.h storageReadsCompressed): whether the
/// surface unit's loads and stores (SULD/SUST) read and write a compressed C32_2CRA target as its texels. A 256x256
/// private BGRA8 target, compressed (raw_private_comp.h), cleared to a ZBC value, then four fetching source-over draws
/// (FixedBlendRule.metal rule_fetch_fragment, which reads [[color(0)]]) three ways: the storage form viewing the compressed
/// target (switchRawProbeStorageCompressed:), today's fallback to the barrier form, and the same draws on plain memory
/// (switchRawProbePlainPrivate:). PASS: the storage form ran on compressed memory (no compressed fallback counted, the
/// target compressed) and its bytes are the plain run's; the barrier form's are too. FAIL with `storage_bytes_differ`: the
/// unit reads raw bytes, so the rule stays false and compressed targets keep the barrier form.
struct StorageCompressedTest: HWTest {
    let id = "gpu.raw.storage_compressed"
    let name = "Surface loads and stores through a compressed target: the storage form against plain memory"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 90 }

    static let storageSelector = NSSelectorFromString("switchRawProbeStorageCompressed:")
    static let plainSelector = NSSelectorFromString("switchRawProbePlainPrivate:")
    static let factsSelector = NSSelectorFromString("switchRawProbeFacts:")
    static let side = 256

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        guard let facade = gpu.device as? NSObject, facade.responds(to: Self.storageSelector) else {
            return .skip("a release facade: no storage-compression switch")
        }
        let state = try gpu.render(fragment: "rule_fetch_fragment", format: .bgra8Unorm)
        var metrics = Metrics()
        var runs: [String: [UInt8]] = [:]
        for arm in ["plain", "barrier", "storage"] {
            _ = facade.perform(Self.plainSelector, with: NSNumber(value: arm == "plain"))
            _ = facade.perform(Self.storageSelector, with: NSNumber(value: arm == "storage"))
            defer {
                _ = facade.perform(Self.plainSelector, with: NSNumber(value: false))
                _ = facade.perform(Self.storageSelector, with: NSNumber(value: false))
            }
            let target = try await self.target(gpu, facade, plain: arm == "plain")
            let before = RawFacade.counters(of: gpu.device)["fetch_fallback.compressed"] ?? 0
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
                for layer in 0..<4 {
                    var which = SIMD2<UInt32>(UInt32(layer), 1)
                    e.setFragmentBytes(&which, length: 8, index: 0)
                    e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                e.endEncoding()
            }.path
            let fallbacks = (RawFacade.counters(of: gpu.device)["fetch_fallback.compressed"] ?? 0) - before
            runs[arm] = try await gpu.read(target, bytesPerPixel: 4)
            metrics.set("\(arm)_path", path)
            metrics.set("\(arm)_compressed", comptags(facade, target))
            metrics.set("\(arm)_compressed_fallbacks", fallbacks)
        }
        var wrong: [String] = []
        for arm in ["storage", "barrier"] {
            let differ = zip(runs[arm] ?? [], runs["plain"] ?? []).filter { $0 != $1 }.count
            metrics.set("\(arm)_bytes_differ", differ)
            if differ > 0 { wrong.append("\(arm)_bytes_differ") }
        }
        if metrics["storage_compressed"] != .integer(1) { wrong.append("storage_not_compressed") }
        if metrics["storage_compressed_fallbacks"] != .integer(0) { wrong.append("storage_fell_back") }
        if metrics["plain_compressed"] == .integer(1) { wrong.append("reference_compressed") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    func comptags(_ facade: NSObject, _ texture: MTLTexture) -> Int {
        let line = facade.perform(Self.factsSelector, with: texture)?.takeUnretainedValue() as? String ?? ""
        return line.contains("comptags=1") ? 1 : 0
    }

    /// A private target; compressed memory of a size is made off the render thread after its first ask, so the
    /// compressed arms make it again until one binds it (or 20 tries pass)
    func target(_ gpu: GPU, _ facade: NSObject, plain: Bool) async throws -> MTLTexture {
        var made = try gpu.texture(.bgra8Unorm, Self.side, Self.side, usage: [.renderTarget, .shaderRead])
        for _ in 0..<20 where !plain && comptags(facade, made) == 0 {
            try await Task.sleep(nanoseconds: 50_000_000)
            made = try gpu.texture(.bgra8Unorm, Self.side, Self.side, usage: [.renderTarget, .shaderRead])
        }
        return made
    }
}
