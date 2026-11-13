import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Rules resolve.twod and resolve.exact (drivers/TegraGM20BMetal/raw/runtime/raw_resolve_rule.h): a 720x1280 BGRA8 4x
/// pass, every sample shaded apart, resolved into a private texture four ways, each three times: the route the rules
/// pick, the complement route forced (switchRawProbeResolveExact:), the complement route on a store-and-resolve whose
/// samples a second pass loads and resolves by the pack's pass, and the pack's resolve pass (switchRawProbeResolveDraw:,
/// Metal's exact rule); then the read arm (ResolveReadArm.swift), whose target a blit copies out in the same command buffer.
/// PASS: all on the raw path, the picked route (a target first read in a later command buffer) the 2D engine's within one
/// code of the pass, the complement route taken and within one code, exact at 2x, the kept samples resolving as the
/// pass's, and the read arm through the complement byte for byte Metal's rule computed from the samples
/// (`<n>x_read_route=exact <n>x_read_differ=0`). `2x_exact_differ=0` with
/// `2x_kept_differ=0` flips twodResolveComplementProven, `4x_exact_differ=0` twodResolveSingleRounding4x.
struct ResolveTwodTest: HWTest {
    let id = "gpu.raw.resolve_twod"
    let name = "MSAA resolves by the 2D engine, directly and through the complement, against the resolve pass"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    static let drawSelector = NSSelectorFromString("switchRawProbeResolveDraw:")
    static let exactSelector = NSSelectorFromString("switchRawProbeResolveExact:")
    static let (width, height) = (720, 1280)

    struct Resolved {
        var bytes: [UInt8] = []
        var gpuMs = Double.infinity
        var path = ""
        var twod = 0, exact = 0
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        guard let facade = gpu.device as? NSObject, facade.responds(to: Self.drawSelector),
            facade.responds(to: Self.exactSelector)
        else {
            return .skip("a release facade: no switches to send resolves through the pass or the complement")
        }
        var metrics = Metrics()
        var wrong: [String] = []
        for samples in [4, 2] where gpu.device.supportsTextureSampleCount(samples) {
            let state = try gpu.render(fragment: "resolve_twod_fragment", format: .bgra8Unorm, samples: samples)
            let picked = try await resolved(gpu, state, samples: samples)
            let exact = try await switched(facade, Self.exactSelector) {
                try await resolved(gpu, state, samples: samples)
            }
            let kept = try await keptSamples(gpu, facade, state, samples: samples)
            let pass = try await switched(facade, Self.drawSelector) {
                try await resolved(gpu, state, samples: samples)
            }
            let read = try await readInCommandBuffer(gpu, state, samples: samples)
            let reference = Self.reference(samples: samples)
            let tag = "\(samples)x"
            let readDiffer = zip(read.bytes, reference).filter { $0 != $1 }.count
            let passReferenceDiffer = zip(pass.bytes, reference).filter { $0 != $1 }.count
            metrics.set("\(tag)_read_route", read.exact > 0 ? "exact" : read.twod > 0 ? "twod" : "pass")
            metrics.set("\(tag)_read_differ", readDiffer)
            metrics.set("\(tag)_read_gpu_ms", read.gpuMs, digits: 3)
            metrics.set("\(tag)_pass_reference_differ", passReferenceDiffer)
            if read.exact == 0 { wrong.append("\(tag)_read_not_exact") }
            if readDiffer > 0 { wrong.append("\(tag)_read_differ") }
            if read.path != "raw" { wrong.append("\(tag)_read_path") }
            let (beyond, oneOff) = Self.compare(picked.bytes, pass.bytes)
            let (exactBeyond, exactOff) = Self.compare(exact.bytes, pass.bytes)
            let exactDiffer = exactOff.reduce(0, +) + exactBeyond
            let keptDiffer = zip(kept, pass.bytes).filter { $0 != $1 }.count
            let route = picked.exact > 0 ? "exact" : picked.twod > 0 ? "twod" : "pass"
            metrics.set("\(tag)_route", route)
            metrics.set("\(tag)_beyond_one", beyond)
            metrics.set("\(tag)_one_off_bgra", oneOff.map(String.init).joined(separator: "/"))
            metrics.set("\(tag)_exact_differ", exactDiffer)
            metrics.set("\(tag)_exact_one_off_bgra", exactOff.map(String.init).joined(separator: "/"))
            metrics.set("\(tag)_exact_taken", exact.exact)
            metrics.set("\(tag)_kept_differ", keptDiffer)
            metrics.set("\(tag)_twod_gpu_ms", picked.gpuMs, digits: 3)
            metrics.set("\(tag)_exact_gpu_ms", exact.gpuMs, digits: 3)
            metrics.set("\(tag)_pass_gpu_ms", pass.gpuMs, digits: 3)
            metrics.set("\(tag)_paths", "\(picked.path)/\(exact.path)/\(pass.path)")
            if beyond > 0 { wrong.append("\(tag)_beyond_one") }
            if picked.twod == 0 { wrong.append("\(tag)_not_taken") }
            if exact.exact == 0 { wrong.append("\(tag)_exact_not_taken") }
            if exactBeyond > 0 { wrong.append("\(tag)_exact_beyond_one") }
            if samples == 2 && exactDiffer > 0 { wrong.append("2x_exact_differ") }
            if keptDiffer > 0 { wrong.append("\(tag)_kept_differ") }
            if [picked.path, exact.path, pass.path].contains(where: { $0 != "raw" }) { wrong.append("\(tag)_paths") }
        }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    static func compare(_ a: [UInt8], _ b: [UInt8]) -> (beyond: Int, oneOff: [Int]) {
        var oneOff = [0, 0, 0, 0], beyond = 0
        for i in 0..<min(a.count, b.count) where a[i] != b[i] {
            if abs(Int(a[i]) - Int(b[i])) > 1 { beyond += 1 } else { oneOff[i % 4] += 1 }
        }
        return (beyond, oneOff)
    }

    func switched<T>(_ facade: NSObject, _ selector: Selector, _ body: () async throws -> T) async rethrows -> T {
        _ = facade.perform(selector, with: NSNumber(value: true))
        defer { _ = facade.perform(selector, with: NSNumber(value: false)) }
        return try await body()
    }

    func pass(_ ms: MTLTexture, into: MTLTexture, load: MTLLoadAction, store: MTLStoreAction) -> MTLRenderPassDescriptor
    {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = ms
        pass.colorAttachments[0].resolveTexture = into
        pass.colorAttachments[0].loadAction = load
        pass.colorAttachments[0].storeAction = store
        return pass
    }

    func draw(_ commands: MTLCommandBuffer, _ pass: MTLRenderPassDescriptor, _ state: MTLRenderPipelineState) {
        let e = commands.makeRenderCommandEncoder(descriptor: pass)!
        e.setRenderPipelineState(state)
        var corners = GPU.fullScreen
        e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
        e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        e.endEncoding()
    }

    /// The resolve texture's bytes, the fastest of three GPU times of the pass and its resolve, the path, and the 2D
    /// engine's resolve counters moved over the three
    func resolved(_ gpu: GPU, _ state: MTLRenderPipelineState, samples: Int) async throws -> Resolved {
        let ms = try gpu.texture(.bgra8Unorm, Self.width, Self.height, usage: [.renderTarget], samples: samples)
        let into = try gpu.texture(.bgra8Unorm, Self.width, Self.height, usage: [.renderTarget, .shaderRead])
        let before = RawFacade.counters(of: gpu.device)
        var out = Resolved()
        for _ in 0..<3 {
            let timing = try await gpu.run { commands in
                draw(commands, pass(ms, into: into, load: .clear, store: .multisampleResolve), state)
            }
            out.gpuMs = min(out.gpuMs, timing.gpuMilliseconds)
            out.path = timing.path
        }
        let after = RawFacade.counters(of: gpu.device)
        out.twod = (after["resolve_twod"] ?? 0) - (before["resolve_twod"] ?? 0)
        out.exact = (after["resolve_exact"] ?? 0) - (before["resolve_exact"] ?? 0)
        out.bytes = try await gpu.read(into, bytesPerPixel: 4)
        return out
    }

    /// A store-and-resolve through the complement, then its samples loaded by a pass that draws nothing and resolved by
    /// the pack's pass: the complement put back, these are the pass's own bytes
    func keptSamples(
        _ gpu: GPU, _ facade: NSObject, _ state: MTLRenderPipelineState, samples: Int
    ) async throws
        -> [UInt8]
    {
        let ms = try gpu.texture(.bgra8Unorm, Self.width, Self.height, usage: [.renderTarget], samples: samples)
        let first = try gpu.texture(.bgra8Unorm, Self.width, Self.height, usage: [.renderTarget, .shaderRead])
        let again = try gpu.texture(.bgra8Unorm, Self.width, Self.height, usage: [.renderTarget, .shaderRead])
        try await switched(facade, Self.exactSelector) {
            _ = try await gpu.run { commands in
                draw(commands, pass(ms, into: first, load: .clear, store: .storeAndMultisampleResolve), state)
            }
        }
        try await switched(facade, Self.drawSelector) {
            _ = try await gpu.run { commands in
                commands.makeRenderCommandEncoder(
                    descriptor: pass(ms, into: again, load: .load, store: .multisampleResolve))!
                    .endEncoding()
            }
        }
        return try await gpu.read(again, bytesPerPixel: 4)
    }
}
