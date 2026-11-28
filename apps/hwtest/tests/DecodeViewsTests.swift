import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUDecode
import HWTestKit
import HWTestShared
import Metal

/// Views of decoded textures across forms (drivers/TegraGM20BMetal/TegraGM20BMetalDecodedFamily.mm). A 166 PVRTC
/// texture's 167 view reads the sRGB decode of its codes, the one image (`decode_view_srgb_off` 0, by the nearest code
/// of Apple's table). A texture of the 203 vectors' blocks (and of 209's) with a 185 (191) view and, on 24B119, a 221
/// (227) view made after the upload; then the blocks rotated by one uploaded into the texture; then the first blocks
/// uploaded through the sRGB view: after each, the texture and every view read what a texture of its own format holding
/// the blocks last uploaded reads (`decode_form_view_off` 0; `decode_form_view_stale`, of those, the texels that read
/// the blocks before). An M4 Max reads its views so (decode-upload probes/views/aliasing.txt); here the reference is
/// the device's own decode of each format, which gpu.decode.astc_narrow and astc_hdr judge against Apple's. On 24B119 a
/// 222 texture's 204 and 186 views, which GM20B decodes itself, the same way (`decode_form_native_off` 0). A 204
/// texture's 222 view stays nil (the owner's choice: a texture GM20B decodes keeps no blocks;
/// `decode_native_hdr_view_made` 0). On 21H461 the 221 and 227 views are nil, as the identity makes no HDR texture.
/// Lines judged from the pulled log in this run's window: one gm20b:tex.view for the 222 view (`decode_view_lines_want`)
/// and, on 21H461, one gm20b:tex.decoded_form (`decode_form_lines_want`), each on the process's first run alone (once a
/// key a process).
struct DecodeViewsTest: HWTest {
    let id = "gpu.decode.views"
    let name = "Views of decoded textures across sRGB, linear and HDR forms read their own decode of the last blocks"
    let subsystem = Subsystem.metal

    /// Whether this process already asked for the views whose refusal lines are once a process
    nonisolated(unsafe) static var askedNativeHdr = false, askedHdrForm = false

    struct Forms {
        var off = 0, stale = 0, nativeOff = 0
        var texels = 0, nativeTexels = 0
        var unmade: [String] = []
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let start = Date()
        let hdr = context.ios >= 27
        var forms = Forms()
        var viewLinesWant = 0, formLinesWant = 0
        let srgb = try await Self.pvrtcView(gpu, &forms), srgbOff = srgb.off
        for (base, own) in [(203, [185, 221]), (209, [191, 227])] {
            try await Self.family(gpu, base, own.filter { hdr || $0 < 221 }, native: [], &forms)
        }
        if hdr { try await Self.family(gpu, 222, [], native: [204, 186], &forms) }
        for format in hdr ? [] : [221, 227] {
            let texture = try await DecodeScene.astcRow(gpu, DecodeVectors.set(format == 221 ? 203 : 209))
            if texture != nil && !Self.askedHdrForm {
                Self.askedHdrForm = true
                formLinesWant = 1
            }
            if texture?.makeTextureView(pixelFormat: MTLPixelFormat(rawValue: UInt(format))!) != nil {
                forms.unmade.append("\(format)_view_made")
            }
        }
        var nativeHdrMade = 0
        if let native = try await DecodeScene.astcRow(gpu, DecodeVectors.set(204)) {
            if !Self.askedNativeHdr {
                Self.askedNativeHdr = true
                viewLinesWant = 1
            }
            nativeHdrMade = native.makeTextureView(pixelFormat: MTLPixelFormat(rawValue: 222)!) == nil ? 0 : 1
        } else {
            forms.unmade.append("204")
        }
        var metrics = Metrics()
        metrics.set("decode_view_srgb_off", srgbOff)
        metrics.set("decode_view_srgb_forms", srgb.forms)
        metrics.set("decode_form_view_off", forms.off)
        metrics.set("decode_form_view_texels", forms.texels)
        metrics.set("decode_form_view_stale", forms.stale)
        metrics.set("decode_form_native_off", forms.nativeOff)
        metrics.set("decode_form_native_texels", forms.nativeTexels)
        metrics.set("decode_native_hdr_view_made", nativeHdrMade)
        metrics.set("unmade", forms.unmade.isEmpty ? "-" : forms.unmade.joined(separator: ","))
        let viewLines = ProcessLog.lines(
            ProcessLog.refusal, "gm20b", containing: "tex.view: no view of format 222,", since: start
        )
        let formLines = ProcessLog.lines(ProcessLog.refusal, "gm20b", containing: "tex.decoded_form", since: start)
        metrics.set("decode_view_lines_want", viewLinesWant)
        metrics.set("decode_view_lines", viewLines?.count ?? -1)
        metrics.set("decode_form_lines_want", formLinesWant)
        metrics.set("decode_form_lines", formLines?.count ?? -1)
        return .judged(
            [
                ("textures or views not as this identity makes them: \(forms.unmade)", forms.unmade.isEmpty),
                ("\(srgbOff) texels of the 167 view off the sRGB decode of the codes", srgbOff == 0),
                (
                    "\(forms.off) of \(forms.texels) view or texture texels off their own decode of the last blocks",
                    forms.off == 0 && forms.texels > 0
                ),
                (
                    "\(forms.nativeOff) of \(forms.nativeTexels) native-form view texels off a native texture's",
                    forms.nativeOff == 0 && (forms.nativeTexels > 0 || !hdr)
                ),
                ("a 204 texture's 222 view made", nativeHdrMade == 0),
            ], metrics, proof: gpu.proof
        ).onRawPath(gpu)
    }

    /// The 167 view of a 166 texture of the driver's 16x16 vector: texels off the sRGB decode of its codes, and how
    /// their channels missed (decode_view_srgb_forms: DecodeJudge.Tally.forms)
    static func pvrtcView(_ gpu: GPU, _ forms: inout Forms) async throws -> DecodeJudge.Tally {
        guard let v = DecodeVectors.level(166, side: 16),
              let texture = DecodeScene.texture(gpu, v.pixelFormat, 16, 16, storage: .shared),
              let view = texture.makeTextureView(pixelFormat: MTLPixelFormat(rawValue: 167)!)
        else {
            forms.unmade.append("166_or_167")
            return DecodeJudge.Tally()
        }
        v.bytes.withUnsafeBytes {
            texture.replace(
                region: MTLRegionMake2D(0, 0, 16, 16), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: 0
            )
        }
        let read = try await DecodeScene.read(gpu, view, DecodeScene.everyTexel(16, 16))
        return DecodeJudge.rgba8(read, v.want, srgb: true)
    }

    /// A texture of `base`'s vectors with views of the formats `own` (decoded forms) and `native` (forms GM20B
    /// decodes): after the upload, after the blocks rotated by one went into the texture, and after the first blocks
    /// went in through the first view, each member against a texture of its own format holding the blocks last uploaded
    static func family(_ gpu: GPU, _ base: Int, _ own: [Int], native: [Int], _ forms: inout Forms) async throws {
        let set = DecodeVectors.set(base)
        guard let texture = try await DecodeScene.astcRow(gpu, set) else {
            forms.unmade.append("\(base)")
            return
        }
        var views: [(format: Int, view: MTLTexture)] = []
        for format in own + native {
            guard let view = texture.makeTextureView(pixelFormat: MTLPixelFormat(rawValue: UInt(format))!) else {
                forms.unmade.append("\(base)_\(format)")
                continue
            }
            views.append((format, view))
        }
        let at = set.points.map(\.at)
        var last = 0, before = 0
        for step in 0 ..< 3 {
            if step == 1 {
                try await DecodeScene.upload(
                    gpu, set.bytes(rotated: 1), rowBytes: set.blocks.count * 16, into: texture, width: set.width,
                    height: set.blockH
                )
                (before, last) = (0, 1)
            } else if step == 2, let through = views.first {
                try await DecodeScene.upload(
                    gpu, set.bytes(), rowBytes: set.blocks.count * 16, into: through.view, width: set.width,
                    height: set.blockH
                )
                (before, last) = (1, 0)
            }
            for (format, member) in [(base, texture)] + views.map({ ($0.format, $0.view) }) {
                let read = try await DecodeScene.read(gpu, member, at)
                let want = try await Self.reference(gpu, set, format, rotated: last, at)
                let off = DecodeScene.differing(read, want)
                if native.contains(format) {
                    forms.nativeOff += off
                    forms.nativeTexels += read.count
                } else {
                    forms.off += off
                    forms.texels += read.count
                }
                guard off > 0, step > 0 else { continue }
                let old = try await Self.reference(gpu, set, format, rotated: before, at)
                forms.stale += zip(read, zip(want, old)).filter { r, pair in
                    DecodeScene.differing([r], [pair.0]) > 0 && DecodeScene.differing([r], [pair.1]) == 0
                }.count
            }
        }
    }

    /// A texture of `format` holding `set`'s blocks rotated by `rotated`, read at `at`
    static func reference(
        _ gpu: GPU, _ set: DecodeVectors.Astc, _ format: Int, rotated: Int, _ at: [SIMD2<UInt32>]
    ) async throws -> [SIMD4<Float>] {
        let pixelFormat = MTLPixelFormat(rawValue: UInt(format))!
        guard let texture = try await DecodeScene.astcRow(gpu, set, format: pixelFormat, rotated: rotated) else {
            throw TestFailure("no reference texture of format \(format)")
        }
        return try await DecodeScene.read(gpu, texture, at)
    }
}
