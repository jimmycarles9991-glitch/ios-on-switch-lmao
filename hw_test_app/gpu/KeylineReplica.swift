import Foundation
import HWTestCore
import Metal

/// The state the scroll edge keyline draws with (PBGRABsovXmw_A3Xhfc: Core Animation's solid fill blending source-over
/// into BGRA8 beside an RGBA16Float and an R32Float attachment it never writes, its fragment made for framebuffer
/// fetch), drawn offscreen as the render server draws it (ui.pocket_keyline): the colour target cleared to a known
/// backdrop by one pass, then loaded by the fill's pass, whose other two attachments start at known values. The marks
/// are the keyline's translucent white as a whole-pixel row, as a row with fractional edges (a half point at 1.59
/// pixels a point) and as a column, and a tinted row, so a channel swap shows. Every attachment is read back: the
/// fill writes the colour target alone. With the other two attachments private, then memoryless as Core Animation
/// makes them, which the raw path's storage form can view.
public enum KeylineReplica {
    public static let state = """
        R ok=1 label=com.apple.coreanimation.PBGRABsovXmw_A3Xhfc samples=1 topo=0 ds=0/0 c0=80:15:1:1:5:0:1:5:0 c1=115:0:0:1:0:0:1:0:0 c2=55:0:0:1:0:0:1:0:0 a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1 | v fixed_vert_lph_spc fc_generic:53:00 fc_perspective:53:00 fc_texcoord_count0:49:00 fc_texcoord_count1:49:00 fc_vertex_layout:49:00 | f fixed_frag_lph_cpf fc_attachment_count:49:03 fc_blend_function:49:3f fc_clamp_color:53:00 fc_color_inputs0:49:00 fc_color_inputs1:49:00 fc_color_opaque0:53:00 fc_color_opaque1:53:00 fc_color_opcodes0:52:00000000 fc_color_opcodes1:52:00000000 fc_color_opcodes_ext0:50:0000 fc_color_opcodes_ext1:50:0000 fc_coord_function0:49:00 fc_coord_function1:49:00 fc_debug_xr:53:00 fc_dest_count:49:00 fc_dest_index:49:00 fc_destination_function:49:00 fc_extended_range:53:00 fc_filter0:49:00 fc_filter1:49:00 fc_framebuffer_fetch:53:01 fc_generic:53:00 fc_has_asg_77_weights:53:00 fc_has_attachment_0:53:00 fc_has_attachment_1:53:00 fc_has_attachment_2:53:00 fc_has_color_cube0:53:00 fc_has_color_cube1:53:00 fc_has_color_params:53:00 fc_has_color_trc0:53:00 fc_has_color_trc1:53:00 fc_has_edr_factor:53:00 fc_has_gamma_luts:53:00 fc_has_hdr_scale:53:00 fc_has_image_0A:53:00 fc_has_image_0B:53:00 fc_has_image_0C:53:00 fc_has_image_1A:53:00 fc_has_image_1B:53:00 fc_has_image_1C:53:00 fc_has_lod_bias:53:00 fc_has_lut_tex:53:00 fc_has_noise_scale:53:00 fc_has_noise_tex:53:00 fc_has_premultiply:53:00 fc_has_ycbcr_matrix:53:00 fc_image_count:49:00 fc_image_function0:49:00 fc_image_function1:49:00 fc_memoryless_index0:49:00 fc_memoryless_index1:49:00 fc_noise_width:49:00 fc_perspective:53:00 fc_plane_count0:49:00 fc_plane_count1:49:00 fc_texcoord_count0:49:00 fc_texcoord_count1:49:00 fc_texture_function:49:00 fc_unbounded_range:53:00 fc_vertex_layout:49:00 |
        """

    public static let width = 64
    public static let height = 16
    /// R, G, B of the colour target before the fill (the pocket's dark grey under the device's keyline)
    public static let backdrop: [Int] = [20, 20, 21]
    static let other: [Float] = [0.25, 0.5, 0.75, 1]
    static let third: Float = 0.375

    /// A rectangle in pixels and its straight colour; Core Animation's vertex carries it premultiplied
    public struct Mark: Sendable {
        public let name: String
        public let x0: Float, y0: Float, x1: Float, y1: Float
        public let rgb: [Float]
        public let alpha: Float
        /// The pixel whose value the test reports
        public let probe: (x: Int, y: Int)

        func covers(_ x: Int, _ y: Int) -> Bool {
            let (cx, cy) = (Float(x) + 0.5, Float(y) + 0.5)
            return cx >= x0 && cx < x1 && cy >= y0 && cy < y1
        }
    }

    public static let marks = [
        Mark(name: "row", x0: 4, y0: 7, x1: 60, y1: 8, rgb: [1, 1, 1], alpha: 0.1, probe: (32, 7)),
        Mark(name: "frac", x0: 4, y0: 3.2, x1: 60, y1: 4, rgb: [1, 1, 1], alpha: 0.1, probe: (32, 3)),
        Mark(name: "tint", x0: 4, y0: 11, x1: 60, y1: 12, rgb: [1, 0.5, 0.25], alpha: 0.1, probe: (32, 11)),
        Mark(name: "column", x0: 62, y0: 1, x1: 63, y1: 15, rgb: [1, 1, 1], alpha: 0.1, probe: (62, 8)),
    ]

    public struct Drawn: Sendable {
        public let path: String
        /// How the raw path drew the fill (the facade's counters): storage, barrier.<why the storage form fell back>,
        /// split or whole; "-" off the facade
        public let form: String
        /// The colour target, BGRA bytes, and the other two attachments' bytes where they were kept
        public let color: [UInt8]
        public let other: [UInt8]?
        public let third: [UInt8]?
    }

    public static func draw(gpu: GPU, memoryless: Bool) async throws -> Drawn {
        guard let line = PipelineLine(state) else { throw TestFailure("the replica's state line does not parse") }
        let before = RawFacade.counters(of: gpu.device)
        let library = try gpu.device.makeLibrary(URL: CAState.library)
        let pipeline = try CAState.pipeline(line, library: library, device: gpu.device)
        let target = try gpu.texture(.bgra8Unorm, width, height)
        let storage: MTLStorageMode = memoryless ? .memoryless : .private
        let usage: MTLTextureUsage = memoryless ? [.renderTarget] : [.renderTarget, .shaderRead]
        let second = try gpu.texture(.rgba16Float, width, height, usage: usage, storage: storage)
        let thirdTarget = try gpu.texture(.r32Float, width, height, usage: usage, storage: storage)
        let timing = try await gpu.run(tolerateError: true) { commands in
            let clear = MTLRenderPassDescriptor()
            clear.colorAttachments[0].texture = target
            clear.colorAttachments[0].loadAction = .clear
            clear.colorAttachments[0].clearColor = MTLClearColor(
                red: Double(backdrop[0]) / 255, green: Double(backdrop[1]) / 255, blue: Double(backdrop[2]) / 255,
                alpha: 1)
            clear.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: clear)!.endEncoding()
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .store
            for (index, texture, value) in [(1, second, other), (2, thirdTarget, [third, 0, 0, 0])] {
                pass.colorAttachments[index].texture = texture
                pass.colorAttachments[index].loadAction = .clear
                pass.colorAttachments[index].clearColor = MTLClearColor(
                    red: Double(value[0]), green: Double(value[1]), blue: Double(value[2]), alpha: Double(value[3]))
                pass.colorAttachments[index].storeAction = memoryless ? .dontCare : .store
            }
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            for (index, mark) in marks.enumerated() {
                let vertices = rectangle(mark)
                if index == 0 { CAState.bind(encoder, vertices: vertices, source: nil) }
                var bytes = vertices
                encoder.setVertexBytes(&bytes, length: MemoryLayout<CAState.Vertex>.stride * bytes.count, index: 1)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: bytes.count)
            }
            encoder.endEncoding()
        }
        if timing.path.hasPrefix("error:") { throw TestFailure("the replica's draw: \(timing.path)") }
        let after = RawFacade.counters(of: gpu.device)
        let color = try await gpu.read(target, bytesPerPixel: 4)
        var kept: ([UInt8], [UInt8])?
        if !memoryless {
            kept = (try await gpu.read(second, bytesPerPixel: 8), try await gpu.read(thirdTarget, bytesPerPixel: 4))
        }
        return Drawn(
            path: timing.path, form: form(before: before, after: after), color: color, other: kept?.0, third: kept?.1)
    }

    /// R, G, B, A of the colour target at a pixel: the backdrop with every mark that covers the pixel's centre blended
    /// over it in order, each stored as unorm8 before the next
    public static func expected(_ x: Int, _ y: Int) -> [Int] {
        var value = backdrop.map { Double($0) / 255 } + [1]
        for mark in marks where mark.covers(x, y) {
            let a = KeylineClaim.half(Double(mark.alpha))
            let source = mark.rgb.map { KeylineClaim.half(Double($0 * mark.alpha)) } + [a]
            value = (0..<4).map { ((source[$0] + value[$0] * (1 - a)) * 255).rounded() / 255 }
        }
        return value.map { Int(($0 * 255).rounded()) }
    }

    public static func rgba(_ drawn: Drawn, _ x: Int, _ y: Int) -> [Int] {
        let at = (y * width + x) * 4
        return [2, 1, 0, 3].map { Int(drawn.color[at + $0]) }
    }

    /// The first attachment value off what the fill should leave ("c0.g@32,7": a colour channel more than a code off;
    /// "c1@x,y", "c2@x,y": an attachment the fill must not write, changed), "" for none, and how many texels are off
    public static func judge(_ drawn: Drawn) -> (wrong: String, texels: Int) {
        var first = ""
        var count = 0
        let otherBits = other.flatMap { withUnsafeBytes(of: Float16($0).bitPattern.littleEndian) { Array($0) } }
        let thirdBits = withUnsafeBytes(of: third.bitPattern.littleEndian) { Array($0) }
        for y in 0..<height {
            for x in 0..<width {
                let (got, want) = (rgba(drawn, x, y), expected(x, y))
                var wrong = (0..<4).first { abs(got[$0] - want[$0]) > 1 }.map { "c0." + ["r", "g", "b", "a"][$0] }
                let at = y * width + x
                if wrong == nil, let kept = drawn.other, Array(kept[(at * 8)..<(at * 8 + 8)]) != otherBits {
                    wrong = "c1"
                }
                if wrong == nil, let kept = drawn.third, Array(kept[(at * 4)..<(at * 4 + 4)]) != thirdBits {
                    wrong = "c2"
                }
                guard let wrong else { continue }
                count += 1
                if first.isEmpty { first = "\(wrong)@\(x),\(y)" }
            }
        }
        return (first, count)
    }

    /// The other two attachments at a pixel as their values ("0.25,0.5,0.75,1/0.375"), "-" where they were not kept
    public static func others(_ drawn: Drawn, _ x: Int, _ y: Int) -> String {
        guard let second = drawn.other, let third = drawn.third else { return "-" }
        let at = y * width + x
        let halves = (0..<4).map { c in
            let bits = UInt16(second[at * 8 + c * 2]) | UInt16(second[at * 8 + c * 2 + 1]) << 8
            return String(format: "%g", Float(Float16(bitPattern: bits)))
        }
        let word = (0..<4).reduce(UInt32(0)) { $0 | UInt32(third[at * 4 + $1]) << (8 * $1) }
        return halves.joined(separator: ",") + "/" + String(format: "%g", Float(bitPattern: word))
    }

    static func rectangle(_ mark: Mark) -> [CAState.Vertex] {
        let x = { (p: Float) in p / Float(width) * 2 - 1 }
        let y = { (p: Float) in 1 - p / Float(height) * 2 }
        let a = mark.alpha
        let color = SIMD4<Float16>(
            Float16(mark.rgb[0] * a), Float16(mark.rgb[1] * a), Float16(mark.rgb[2] * a), Float16(a))
        return CAState.rectangle(x0: x(mark.x0), y0: y(mark.y1), x1: x(mark.x1), y1: y(mark.y0), color: color)
    }

    public static func form(before: [String: Int], after: [String: Int]) -> String {
        guard !after.isEmpty else { return "-" }
        let moved = { (key: String) in (after[key] ?? 0) - (before[key] ?? 0) }
        let fallbacks = after.keys.filter { $0.hasPrefix("fetch_fallback.") && moved($0) > 0 }
            .map { String($0.dropFirst("fetch_fallback.".count)) }.sorted()
        if moved("interlock_draws") > 0 { return "storage" }
        if !fallbacks.isEmpty { return "barrier." + fallbacks.joined(separator: "+") }
        return moved("split_prologues") > 0 ? "split" : "whole"
    }
}
