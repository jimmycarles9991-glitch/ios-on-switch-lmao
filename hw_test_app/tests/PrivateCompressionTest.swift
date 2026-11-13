import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Private render targets on compressed memory (drivers/TegraGM20BMetal/raw/runtime/raw_private_comp.h): a glass-like
/// chain (a 512x512 BGRA8 pattern, a 256x256 RGBA16Float and a 128x128 BGRA8 2:1 reduction of it), each pass cleared to a
/// ZBC value first, drawn on compressed memory and again with compression held off (switchRawProbePlainPrivate:): every
/// byte the same. Mid-chain one command buffer that samples the first target also writes a Boolean visibility result,
/// and stays raw (moves counts the targets that left compressed memory meanwhile). Then 16 clear-only passes of the
/// 512x512 target, compressed and plain, give the GPU time a ZBC clear saves.
struct PrivateCompressionTest: HWTest {
    let id = "gpu.raw.private_compression"
    let name = "Private targets on compressed memory: same bytes, ZBC clears, a Boolean visibility pass kept raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 90 }

    static let plainSelector = NSSelectorFromString("switchRawProbePlainPrivate:")
    static let factsSelector = NSSelectorFromString("switchRawProbeFacts:")

    struct Chain {
        var bytes: [[UInt8]] = []
        var compressed: [Int] = []
        var booleanPath = ""
        var clearMs = 0.0
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        guard let facade = gpu.device as? NSObject, facade.responds(to: Self.plainSelector) else {
            return .skip("a release facade: no switch to hold compression off for the reference")
        }
        let before = RawFacade.counters(of: gpu.device), zbcBefore = RawFacade.compression(of: gpu.device)
        let compressed = try await chain(gpu, facade, plain: false)
        let after = RawFacade.counters(of: gpu.device), zbcAfter = RawFacade.compression(of: gpu.device)
        let plain = try await chain(gpu, facade, plain: true)
        var metrics = Metrics()
        var wrong: [String] = []
        let names = ["pattern", "half", "quarter", "after_boolean"]
        for (i, name) in names.enumerated() {
            let differing = zip(compressed.bytes[i], plain.bytes[i]).filter { $0 != $1 }.count
            metrics.set("\(name)_bytes_differ", differing)
            if differing != 0 { wrong.append(name) }
        }
        metrics.set("compressed_targets", compressed.compressed.map(String.init).joined(separator: ","))
        metrics.set("plain_targets", plain.compressed.map(String.init).joined(separator: ","))
        let moves = (after["private_moves"] ?? 0) - (before["private_moves"] ?? 0)
        metrics.set("made", (after["private_compressed"] ?? 0) - (before["private_compressed"] ?? 0))
        metrics.set("moves", moves)
        metrics.set("boolean_path", compressed.booleanPath)
        metrics.set("zbc_hits", (zbcAfter["zbc_hits"] ?? 0) - (zbcBefore["zbc_hits"] ?? 0))
        metrics.set("zbc_misses", (zbcAfter["zbc_misses"] ?? 0) - (zbcBefore["zbc_misses"] ?? 0))
        metrics.set("clear16_compressed_gpu_ms", compressed.clearMs, digits: 3)
        metrics.set("clear16_plain_gpu_ms", plain.clearMs, digits: 3)
        if compressed.compressed.first != 1 { wrong.append("not_compressed") }
        if plain.compressed.contains(1) { wrong.append("reference_compressed") }
        if compressed.booleanPath != "raw" { wrong.append("boolean_path") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    func comptags(_ facade: NSObject, _ texture: MTLTexture) -> Int {
        let line = facade.perform(Self.factsSelector, with: texture)?.takeUnretainedValue() as? String ?? ""
        return line.contains("comptags=1") ? 1 : 0
    }

    /// A private target of the chain; compressed memory of a size is made off the render thread after its first ask, so
    /// the compressed run makes it again until one binds it (or 20 tries pass)
    func target(
        _ gpu: GPU, _ facade: NSObject, _ format: MTLPixelFormat, _ size: Int, plain: Bool
    ) async throws
        -> MTLTexture
    {
        var made = try gpu.texture(format, size, size)
        for _ in 0..<20 where !plain && comptags(facade, made) == 0 {
            try await Task.sleep(nanoseconds: 50_000_000)
            made = try gpu.texture(format, size, size)
        }
        return made
    }

    func chain(_ gpu: GPU, _ facade: NSObject, plain: Bool) async throws -> Chain {
        _ = facade.perform(Self.plainSelector, with: NSNumber(value: plain))
        defer { _ = facade.perform(Self.plainSelector, with: NSNumber(value: false)) }
        let first = try await target(gpu, facade, .bgra8Unorm, 512, plain: plain)
        let half = try await target(gpu, facade, .rgba16Float, 256, plain: plain)
        let quarter = try await target(gpu, facade, .bgra8Unorm, 128, plain: plain)
        let again = try await target(gpu, facade, .bgra8Unorm, 256, plain: plain)
        var out = Chain()
        out.compressed = [first, half, quarter, again].map { comptags(facade, $0) }
        let pattern = try gpu.render(fragment: "comp_pattern_fragment", format: .bgra8Unorm)
        let reduceHalf = try gpu.render(fragment: "comp_reduce_fragment", format: .rgba16Float)
        let reduce = try gpu.render(fragment: "comp_reduce_fragment", format: .bgra8Unorm)
        func pass(
            _ target: MTLTexture, _ state: MTLRenderPipelineState, source: MTLTexture?, clear: MTLClearColor
        )
            -> (MTLCommandBuffer) -> Void
        {
            { commands in
                let p = MTLRenderPassDescriptor()
                p.colorAttachments[0].texture = target
                p.colorAttachments[0].loadAction = .clear
                p.colorAttachments[0].clearColor = clear
                p.colorAttachments[0].storeAction = .store
                let e = commands.makeRenderCommandEncoder(descriptor: p)!
                e.setRenderPipelineState(state)
                var corners = GPU.fullScreen
                e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                if let source { e.setFragmentTexture(source, index: 0) }
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                e.endEncoding()
            }
        }
        let black = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1), zero = MTLClearColor()
        try await gpu.run(pass(first, pattern, source: nil, clear: black))
        try await gpu.run(pass(half, reduceHalf, source: first, clear: zero))
        let results = try gpu.buffer(16)
        let scratch = try gpu.texture(.bgra8Unorm, 256, 256)
        out.booleanPath = try await gpu.run { commands in
            let p = MTLRenderPassDescriptor()
            p.colorAttachments[0].texture = scratch
            p.colorAttachments[0].loadAction = .clear
            p.colorAttachments[0].storeAction = .store
            p.visibilityResultBuffer = results
            let e = commands.makeRenderCommandEncoder(descriptor: p)!
            e.setVisibilityResultMode(.boolean, offset: 0)
            e.setRenderPipelineState(reduce)
            var corners = GPU.fullScreen
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            e.setFragmentTexture(first, index: 0)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            e.endEncoding()
        }.path
        try await gpu.run(pass(quarter, reduce, source: half, clear: zero))
        try await gpu.run(pass(again, reduce, source: first, clear: zero))
        for texture in [first, half, quarter, again] {
            out.bytes.append(try await gpu.read(texture, bytesPerPixel: texture.pixelFormat == .rgba16Float ? 8 : 4))
        }
        let clears = try await target(gpu, facade, .bgra8Unorm, 512, plain: plain)
        let clearOnly: (MTLCommandBuffer) -> Void = { commands in
            for _ in 0..<16 {
                let p = MTLRenderPassDescriptor()
                p.colorAttachments[0].texture = clears
                p.colorAttachments[0].loadAction = .clear
                p.colorAttachments[0].clearColor = black
                p.colorAttachments[0].storeAction = .store
                commands.makeRenderCommandEncoder(descriptor: p)!.endEncoding()
            }
        }
        for _ in 0..<3 { out.clearMs += try await gpu.run(clearOnly).gpuMilliseconds / 3 }
        return out
    }
}
