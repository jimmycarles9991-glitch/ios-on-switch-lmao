import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Draws that fetch the colour they write (fetch_order_fragment: each full-screen triangle blends its step into what the
/// one before it left), in the shapes whose raster order the raw path keeps differently: three instances in the middle of
/// three render encoders of one command buffer, a strip of six vertices whose four triangles cover each other, and twelve
/// overlapping triangles in one draw over a depth attachment and into four and two samples. Each pass must match the
/// result of its triangles applied in order, and on the facade the fetch counters must show how the raw path took
/// them: inside the pixel interlock (fetch_interlocked), or refused as recorded or at encoding (its encoder then on
/// m2v), or ordered on the GPU a triangle a draw (sequential) or on the CPU's proof (whole). A pass that moves none of
/// them did not fetch its own colour.
struct CAFetchRouteScene: GPUScene {
    let id = "gpu.ca.fetch_routes"
    let name = "Draws fetching their own colour: instanced, folded strip, depth and multisample routes and pixels"

    static let size = 64
    static let clear: [Float] = [0.125, 0.25, 0.5, 1]

    enum Pass: String, CaseIterable {
        case instanced, strip, depth, msaa, msaa2
    }

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        for pass in Pass.allCases {
            do {
                try await draw(pass, gpu, &result)
            } catch {
                result.expect(false, "\(pass.rawValue): \(GPU.describe(error))")
            }
        }
        return result
    }

    /// The steps the pass's triangles apply, in raster order (fetch_vertex: a triangle's step is its first vertex / 3 plus
    /// its instance)
    static func steps(_ pass: Pass) -> [Int] {
        switch pass {
        case .instanced: return [0, 0, 1, 2, 0]
        case .strip: return [0, 0, 0, 1]
        case .depth, .msaa, .msaa2: return Array(0..<12)
        }
    }

    func draw(_ pass: Pass, _ gpu: GPU, _ result: inout SceneResult) async throws {
        let size = Self.size
        let samples = pass == .msaa ? 4 : pass == .msaa2 ? 2 : 1
        let depthFormat: MTLPixelFormat = pass == .depth ? .depth32Float : .invalid
        let pipeline = try gpu.render(
            vertex: "fetch_vertex", fragment: "fetch_order_fragment", format: .bgra8Unorm, samples: samples,
            depth: depthFormat)
        let resolved = try gpu.texture(.bgra8Unorm, size, size, usage: [.renderTarget, .shaderRead])
        let target =
            try samples > 1 ? gpu.texture(.bgra8Unorm, size, size, usage: [.renderTarget], samples: samples) : resolved
        let depth: MTLTexture? =
            try pass == .depth ? gpu.texture(.depth32Float, size, size, usage: [.renderTarget]) : nil
        let depthState = depth.flatMap { _ -> MTLDepthStencilState? in
            let descriptor = MTLDepthStencilDescriptor()
            descriptor.depthCompareFunction = .lessEqual
            descriptor.isDepthWriteEnabled = true
            return gpu.device.makeDepthStencilState(descriptor: descriptor)
        }
        let before = RawFacade.counters(of: gpu.device)
        let timing = try await gpu.run(tolerateError: true) { commands in
            func encoder(first: Bool, last: Bool) -> MTLRenderCommandEncoder {
                let descriptor = MTLRenderPassDescriptor()
                descriptor.colorAttachments[0].texture = target
                descriptor.colorAttachments[0].loadAction = first ? .clear : .load
                descriptor.colorAttachments[0].clearColor = MTLClearColor(
                    red: Double(Self.clear[0]), green: Double(Self.clear[1]), blue: Double(Self.clear[2]), alpha: 1)
                descriptor.colorAttachments[0].storeAction = samples > 1 && last ? .multisampleResolve : .store
                if samples > 1 && last { descriptor.colorAttachments[0].resolveTexture = resolved }
                if let depth {
                    descriptor.depthAttachment.texture = depth
                    descriptor.depthAttachment.loadAction = .clear
                    descriptor.depthAttachment.clearDepth = 1
                    descriptor.depthAttachment.storeAction = .dontCare
                }
                let made = commands.makeRenderCommandEncoder(descriptor: descriptor)!
                made.setRenderPipelineState(pipeline)
                if let depthState { made.setDepthStencilState(depthState) }
                var discard: UInt32 = 0
                made.setFragmentBytes(&discard, length: 4, index: 0)
                return made
            }
            switch pass {
            case .instanced:
                for (index, instances) in [1, 3, 1].enumerated() {
                    let e = encoder(first: index == 0, last: index == 2)
                    e.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3, instanceCount: instances)
                    e.endEncoding()
                }
            case .strip:
                let e = encoder(first: true, last: true)
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 6)
                e.endEncoding()
            case .depth, .msaa, .msaa2:
                let e = encoder(first: true, last: true)
                e.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 36)
                e.endEncoding()
            }
        }
        let after = RawFacade.counters(of: gpu.device)
        let name = pass.rawValue
        let moved = { (key: String) in (after[key] ?? 0) - (before[key] ?? 0) }
        result.metrics.set("path_" + name, timing.path)
        result.metrics.set("fetch_" + name, RawFacade.fetchRoutes(before: before, after: after))
        result.expect(!timing.path.hasPrefix("error:"), "\(name): \(timing.path)")
        if RawFacade.kind(of: gpu.device) == "raw" {
            let interlocked = moved("fetch_interlocked")
            let refused = after.keys.filter { $0.hasPrefix("fetch_record.") || $0.hasPrefix("fetch_encode.") }
                .reduce(0) { $0 + moved($1) }
            let ordered = moved("fetch_sequential")
            // a pass the raw path refuses as it begins (m2v:pass:...) records no draw, so no fetch counter can move
            result.expect(
                timing.path.hasPrefix("m2v:pass:") || interlocked + refused + ordered > 0,
                "\(name): no fetch counter moved: the pipeline did not fetch the colour it writes")
            if interlocked == 0 && refused > 0 {
                result.expect(!timing.path.hasPrefix("raw"), "\(name): refused, yet drawn raw: \(timing.path)")
            }
            // twelve overlapping triangles over a depth attachment: one draw each behind the fetch barrier, no vertex read
            if pass == .depth {
                result.expect(
                    timing.path == "raw" && interlocked + moved("fetch_sequential") > 0,
                    "depth: not ordered on the raw path: \(timing.path) \(RawFacade.fetchRoutes(before: before, after: after))"
                )
            }
            if pass == .msaa || pass == .msaa2 {
                result.expect(timing.path == "raw", "\(name): not drawn on the raw path: \(timing.path)")
            }
            if pass == .instanced && moved("fetch_record.instances") > 0 {
                result.expect(
                    timing.path.hasPrefix("split:2+1:"),
                    "the instanced draw's encoder went to m2v, and its neighbours with it: \(timing.path)")
            }
        }
        let picture = try await gpu.read(resolved, bytesPerPixel: 4)
        let expected = Patterns.repeated(Self.expected(Self.steps(pass)), count: size * size)
        result.compare(
            name, bytes: picture, expected: expected, format: .bgra8Unorm, width: size, height: size,
            tolerance: 3.0 / 255)
    }

    /// fetch_order_fragment's steps over the clear colour, each stored as BGRA8 before the next reads it
    static func expected(_ steps: [Int]) -> [UInt8] {
        let format = TexelFormat.bgra8Unorm
        func stored(_ rgba: [Float]) -> [Float] {
            format.decode(format.encode(rgba)[...])
        }
        var value = stored(clear)
        for step in steps {
            let next = [Float((step * 3 + 1) % 7), Float((step * 5 + 2) % 7), Float((step + 3) % 7)]
            value = stored((0..<3).map { value[$0] * 0.5 + next[$0] / 16 } + [1])
        }
        return format.encode(value)
    }
}
