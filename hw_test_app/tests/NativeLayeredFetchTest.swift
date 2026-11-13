import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Framebuffer fetch in a layered pass: a fragment function reads [[color(0)]] in the layer its vertex function chose
/// ([[render_target_array_index]], instance i into the pass's layer i). On the raw path the fetch is the pixel interlock's
/// storage form, whose storage image is a 2D array of the pass's layers from the attachment's slice. Every part of each
/// texture is cleared to a colour of its own first; the quad in each layer the pass renders then holds that layer's colour
/// with its channels rotated, every other texel its clear colour. Passes of six layers, of two from slice 3, of three of a
/// mipmapped array's level 1 from slice 2, and of one layer (the layered state drawn into a 2D texture, and into a linear
/// one over a buffer, which the pass renders through a block-linear backing), each raw.
struct NativeLayeredFetchTest: NativeObjectTest {
    let id = "metal.native.layered_fetch"
    let name = "A fragment function fetches the colour of the layer its vertex function chose in a layered pass"
    var unavailable: String? { Layering.a13 ? nil : "21H461's A10X has no layered rendering" }

    static let source = """
        #include <metal_stdlib>
        using namespace metal;
        struct LayeredOut { float4 position [[position]]; uint layer [[render_target_array_index]]; };
        vertex LayeredOut layered_fetch_vertex(uint v [[vertex_id]], uint i [[instance_id]]) {
            LayeredOut o;
            o.position = float4(float(v & 1) - 0.5, float(v >> 1) - 0.5, 0, 1);
            o.layer = i;
            return o;
        }
        fragment float4 layered_fetch_fragment(float4 c [[color(0)]]) { return float4(c.g, c.b, c.r, c.a); }
        """

    struct Case {
        let name: String
        let type: MTLTextureType
        let layers: Int, levels: Int
        let level: Int, slice: Int, length: Int
        var linear = false
    }

    static let cases = [
        Case(name: "six", type: .type2DArray, layers: 6, levels: 1, level: 0, slice: 0, length: 6),
        Case(name: "two_from_3", type: .type2DArray, layers: 6, levels: 1, level: 0, slice: 3, length: 2),
        Case(name: "level_1", type: .type2DArray, layers: 6, levels: 2, level: 1, slice: 2, length: 3),
        Case(name: "one_layer", type: .type2D, layers: 1, levels: 1, level: 0, slice: 0, length: 1),
        Case(
            name: "one_layer_linear", type: .type2D, layers: 1, levels: 1, level: 0, slice: 0, length: 1, linear: true),
    ]

    /// A texture of case `c`: a linear one over a shared buffer (which the texture keeps), rows 128 bytes apart
    static func texture(_ device: MTLDevice, _ c: Case) throws -> MTLTexture {
        let d = NativeTargetPartsTest.descriptor(c.type, 32, 32, layers: c.layers, levels: c.levels)
        guard c.linear else {
            guard let t = device.makeTexture(descriptor: d) else { throw TestFailure("no texture for \(c.name)") }
            return t
        }
        d.storageMode = .shared
        guard let buffer = device.makeBuffer(length: 32 * 128, options: .storageModeShared),
            let t = buffer.makeTexture(descriptor: d, offset: 0, bytesPerRow: 128)
        else { throw TestFailure("no linear texture for \(c.name)") }
        return t
    }

    static func rotated(_ c: [UInt8]) -> [UInt8] { [c[1], c[2], c[0], c[3]] }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        let library = try await device.makeLibrary(source: Self.source, options: nil)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = library.makeFunction(name: "layered_fetch_vertex")
        descriptor.fragmentFunction = library.makeFunction(name: "layered_fetch_fragment")
        descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        descriptor.inputPrimitiveTopology = .triangle
        let pipeline = try await device.makeRenderPipelineState(descriptor: descriptor)
        var work = NativeWork(objects: [pipeline])
        for c in Self.cases {
            let t = try Self.texture(device, c)
            work.objects.append(t)
            let parts = TextureParts.parts(t)
            let timing = try await gpu.run { commands in
                for p in parts {
                    commands.makeRenderCommandEncoder(descriptor: TextureParts.pass(t, p, load: .clear))!.endEncoding()
                }
                let pass = TextureParts.pass(t, TexturePart(level: c.level, slice: c.slice), load: .load)
                pass.renderTargetArrayLength = c.length > 1 ? c.length : 0
                let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                e.setRenderPipelineState(pipeline)
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4, instanceCount: c.length)
                e.endEncoding()
            }
            work.paths.append(timing.path)
            var wrong = 0
            for p in parts {
                let got = try await TextureParts.read(gpu, t, p, paths: &work.paths)
                let (w, h, _) = TextureParts.size(t, p.level)
                let rendered = p.level == c.level && (c.slice..<c.slice + c.length).contains(p.slice)
                let clear = TextureParts.clearColor(p)
                for y in 0..<h {
                    for x in 0..<w {
                        let inside = rendered && (w / 4..<3 * w / 4).contains(x) && (h / 4..<3 * h / 4).contains(y)
                        if Array(got.pixel(x, y, width: w)) != (inside ? Self.rotated(clear) : clear) { wrong += 1 }
                    }
                }
            }
            metrics.set("wrong_\(c.name)", wrong)
            if wrong != 0 { work.failures.append("\(wrong) texels of \(c.name)") }
        }
        return work
    }
}
