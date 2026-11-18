import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// LIMITS 3D-D6: SET_FILL_VIA_TRIANGLE (0x113c), which NVK's context init sets DISABLED. One triangle, the first of an
/// axis-aligned quad's strip [8, 40) x [8, 24) of a 64x64 target, per arm: disabled draws the triangle alone (the
/// reference: half the quad); bbox (2) should fill the whole quad, the claim; all (1) is recorded. Then 64 full 720p quads
/// as two-triangle strips with the mode off against 64 single triangles under bbox: the rasteriser's share of a quad draw.
enum X1FillBoxProbe {
    static let setFillViaTriangle: UInt32 = 0x113c
    static let size = 64
    static let colour = SIMD4<Float>(0.2, 0.4, 0.6, 1)

    static func draw(
        _ state: MTLRenderPipelineState, _ target: MTLTexture, corners: [SIMD4<Float>], vertices: Int,
        count: Int
    ) -> (MTLCommandBuffer) -> Void {
        { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = corners
            var colour = colour
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
            encoder.setFragmentBytes(&colour, length: 16, index: 0)
            for _ in 0..<count { encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: vertices) }
            encoder.endEncoding()
        }
    }

    /// Pixels of the target at the draw's colour, and whether every one of them lies in the quad
    static func covered(_ bytes: [UInt8]) -> (count: Int, inside: Bool) {
        var count = 0
        var inside = true
        for y in 0..<size {
            for x in 0..<size where Array(bytes.pixel(x, y, width: size)) != [0, 0, 0, 255] {
                count += 1
                inside = inside && x >= 8 && x < 40 && y >= 8 && y < 24
            }
        }
        return (count, inside)
    }

    static let probe = X1Probe(
        id: "gpu.x1.fill_bbox", name: "Fill via triangle: one triangle covers its bounding box", probe: "3D-D6",
        risky: true
    ) { gpu in
        var wrong: [String: Int] = [:]
        var metrics = Metrics()
        var paths: [String] = []
        var emitted = 0
        let quad = GPU.pixelQuad(8, 8, 40, 24, width: size, height: size)
        let state = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        for (name, mode) in [("disabled", UInt32(0)), ("bbox", 2), ("all", 1)] {
            let target = try gpu.texture(.bgra8Unorm, size, size)
            do {
                let drawn = try await RawMethods.draw(
                    gpu, before: [(setFillViaTriangle, mode)], after: [(setFillViaTriangle, 0)],
                    draw(state, target, corners: quad, vertices: 3, count: 1))
                paths.append(drawn.path)
                emitted += drawn.emitted
            } catch {
                metrics.set("\(name)_refused", String(describing: error).prefix(80).description)
                continue
            }
            let (count, inside) = covered(try await gpu.read(target, bytesPerPixel: 4))
            metrics.set("\(name)_pixels", count)
            metrics.set("\(name)_inside", inside ? 1 : 0)
            if name == "bbox" { wrong["bbox_missing_pixels"] = abs(32 * 16 - count) + (inside ? 0 : 1) }
        }
        let big = try gpu.texture(.bgra8Unorm, 1280, 720)
        var stripMs = 0.0, boxMs = 0.0
        for _ in 0..<3 {
            stripMs += try await gpu.run(draw(state, big, corners: GPU.fullScreen, vertices: 4, count: 64))
                .gpuMilliseconds
            if let box = try? await RawMethods.draw(
                gpu, before: [(setFillViaTriangle, 2)], after: [(setFillViaTriangle, 0)],
                draw(state, big, corners: GPU.fullScreen, vertices: 3, count: 64))
            {
                boxMs += box.gpuMilliseconds
                emitted += box.emitted
            }
        }
        metrics.set("strip_gpu_ms", stripMs / 3, digits: 3)
        metrics.set("bbox_gpu_ms", boxMs / 3, digits: 3)
        return ProbeFinding.counted(wrong, paths: paths, metrics: metrics).emitting(emitted)
    }
}
