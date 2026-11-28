import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUDecode
import HWTestKit
import Metal

/// PVRTC decoded on the GPU as it is uploaded (drivers/TegraGM20BMetal/decoded.h): each of the driver's
/// PVRTC vectors (DecodeVectors.pvrtc: whole levels of 160 to 167, sides 1 to 16, and every texel as an M4 Max reads
/// it) in a Shared ShaderRead|PixelFormatView texture by replaceRegion of the whole level (Morton order, bytesPerRow
/// 0), every texel read by a kernel. A linear format's codes, and an sRGB one's read through a view as its linear twin
/// (format - 1), are Apple's (`pvrtc_texels_off` 0: texels whose codes differ); an sRGB one read directly measures the
/// GPU's sRGB decode (`pvrtc_srgb_max_diff`, the largest distance from Apple's table of the codes; no value expected).
/// Red: a block decoded wrongly, the Morton order read as rows, or the RGB formats' alpha not one.
struct DecodePvrtcTest: HWTest {
    let id = "gpu.decode.pvrtc"
    let name = "PVRTC decoded on upload reads as an M4 Max reads it"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var off = 0, texels = 0, srgbMax: Float = 0
        var unmade: [String] = []
        for v in DecodeVectors.pvrtc {
            guard let texture = DecodeScene.texture(gpu, v.pixelFormat, v.side, v.side, storage: .shared) else {
                unmade.append("\(v.format)_\(v.side)")
                continue
            }
            v.bytes.withUnsafeBytes {
                texture.replace(
                    region: MTLRegionMake2D(0, 0, v.side, v.side), mipmapLevel: 0, withBytes: $0.baseAddress!,
                    bytesPerRow: 0
                )
            }
            let at = DecodeScene.everyTexel(v.side, v.side)
            guard v.srgb else {
                off += DecodeJudge.rgba8(try await DecodeScene.read(gpu, texture, at), v.want, srgb: false).off
                texels += at.count
                continue
            }
            let twin = MTLPixelFormat(rawValue: UInt(v.format - 1))!
            guard let linear = texture.makeTextureView(pixelFormat: twin) else {
                unmade.append("\(v.format)_\(v.side)_view")
                continue
            }
            off += DecodeJudge.rgba8(try await DecodeScene.read(gpu, linear, at), v.want, srgb: false).off
            texels += at.count
            let direct = try await DecodeScene.read(gpu, texture, at)
            srgbMax = max(srgbMax, DecodeJudge.rgba8(direct, v.want, srgb: true).maxDiff)
        }
        var metrics = Metrics()
        metrics.set("pvrtc_texels_off", off)
        metrics.set("pvrtc_texels", texels)
        metrics.set("pvrtc_srgb_max_diff", Double(srgbMax), digits: 6)
        metrics.set("unmade", unmade.isEmpty ? "-" : unmade.joined(separator: ","))
        return .judged(
            [
                ("\(unmade.count) textures or views not made", unmade.isEmpty),
                ("\(off) of \(texels) texels whose codes are not an M4 Max's", off == 0 && texels > 0),
            ], metrics, proof: gpu.proof
        ).onRawPath(gpu)
    }
}

/// Metal's ASTC footprints of 4x2 and 8x4 texels, which GM20B's texture unit lacks, decoded on the GPU at upload: each
/// of the driver's vectors of 185, 191 (sRGB) and 203, 209 (linear) laid side by side in one row of a Private texture,
/// filled by a blit from a Shared buffer at offset 48 (not a multiple of 256), each vector's texel read by a kernel.
/// The linear forms are held as RGBA16Unorm (the owner's choice): their codes must be the driver's rule of Apple's
/// 12-bit k, the nearest 16-bit code of k / 4095 (`astc_narrow_linear_off` 0), which no 8-bit target would hold as
/// another code than Apple's (`astc_narrow_linear_8bit_off` 0; the largest distance from Apple's value in 16-bit codes
/// reported, at most 0.4999 on an M4 Max: harness/decodeoracle.mm). The sRGB forms read Apple's sRGB table of the
/// codes (`astc_narrow_srgb_codes_off` 0, by the nearest table code; the largest distance and, of the channels off,
/// those read undecoded and decoded twice reported: `astc_narrow_srgb_forms`, DecodeJudge.Tally.forms). Red: a block
/// decoded wrongly (endpoint mode, partition, dual plane, void extent, the error colour), or a blit offset or row
/// misread.
struct DecodeAstcNarrowTest: HWTest {
    let id = "gpu.decode.astc_narrow"
    let name = "ASTC 4x2 and 8x4 decoded on upload read as an M4 Max reads them, the linear forms in 16 bits"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var linear = DecodeJudge.Linear()
        var srgb = DecodeJudge.Tally()
        var linearTexels = 0, srgbTexels = 0
        var unmade: [Int] = []
        for format in [185, 191, 203, 209] {
            let set = DecodeVectors.set(format)
            guard let texture = try await DecodeScene.astcRow(gpu, set) else {
                unmade.append(format)
                continue
            }
            let points = set.points
            let read = try await DecodeScene.read(gpu, texture, points.map(\.at))
            if format == 185 || format == 191 {
                srgb.add(DecodeJudge.rgba8(read, points.map { DecodeJudge.word($0.texel.want) }, srgb: true))
                srgbTexels += read.count
            } else {
                let judged = DecodeJudge.unorm16(read, points.map(\.texel.want))
                linear.off += judged.off
                linear.eightBitOff += judged.eightBitOff
                linear.appleMaxCodes = max(linear.appleMaxCodes, judged.appleMaxCodes)
                linearTexels += read.count
            }
        }
        var metrics = Metrics()
        metrics.set("astc_narrow_linear_off", linear.off)
        metrics.set("astc_narrow_linear_apple_max_codes", linear.appleMaxCodes, digits: 4)
        metrics.set("astc_narrow_linear_8bit_off", linear.eightBitOff)
        metrics.set("astc_narrow_srgb_codes_off", srgb.off)
        metrics.set("astc_narrow_srgb_max_diff", Double(srgb.maxDiff), digits: 6)
        metrics.set("astc_narrow_srgb_forms", srgb.forms)
        metrics.set("astc_narrow_texels", linearTexels + srgbTexels)
        metrics.set("unmade", unmade.isEmpty ? "-" : unmade.map(String.init).joined(separator: ","))
        return .judged(
            [
                ("formats not made: \(unmade)", unmade.isEmpty),
                (
                    "\(linear.off) of \(linearTexels) linear texels off the driver's 16-bit rule",
                    linear.off == 0 && linearTexels > 0
                ),
                ("\(linear.eightBitOff) linear texels an 8-bit target holds otherwise", linear.eightBitOff == 0),
                ("\(srgb.off) of \(srgbTexels) sRGB texels off Apple's codes", srgb.off == 0 && srgbTexels > 0),
            ], metrics, proof: gpu.proof
        ).onRawPath(gpu)
    }
}

/// ASTC HDR decoded on the GPU at upload into RGBA16Float, on 24B119 alone (its identity, an A13, makes HDR textures;
/// 21H461's A10X makes none: `hdr_made` 0 there, every texture nil). For 221 (4x2), 222 (4x4), 227 (8x4) and 236
/// (12x12) the driver's vectors go by blit into level 1 of a two-level texture (the level sized to hold them in one
/// row) and into slice 2 of a three-slice array: each texel reads want's half bits as a float, a NaN where Apple read
/// one (`astc_hdr_texels_off` 0, `astc_hdr_nan_mismatch` 0), and the same points of level 0 and of slices 0 and 1,
/// never written, read a NaN in every channel (`astc_hdr_level_off`, `astc_hdr_slice_off` 0). Red: an HDR endpoint mode
/// or the void extent's half colour decoded wrongly, a NaN lost or made, or a decode landing in another level or slice.
struct DecodeAstcHdrTest: HWTest {
    let id = "gpu.decode.astc_hdr"
    let name = "ASTC HDR decoded on upload reads an M4 Max's halves, in its own level and slice (24B119)"
    let subsystem = Subsystem.metal

    static let formats = [221, 222, 227, 236]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let identityMakesHdr = context.ios >= 27
        var made = 0, texels = 0, levelOff = 0, sliceOff = 0
        var halves = DecodeJudge.Halves()
        for format in Self.formats {
            let set = DecodeVectors.set(format)
            let chain = DecodeScene.texture(gpu, set.pixelFormat, set.width * 2, set.blockH * 2, levels: 2)
            let array = DecodeScene.texture(gpu, set.pixelFormat, set.width, set.blockH, layers: 3)
            made += (chain == nil ? 0 : 1) + (array == nil ? 0 : 1)
            guard let chain, let array else { continue }
            let rowBytes = set.blocks.count * 16
            try await DecodeScene.upload(
                gpu, set.bytes(), offset: 48, rowBytes: rowBytes, into: chain, width: set.width, height: set.blockH,
                level: 1
            )
            try await DecodeScene.upload(
                gpu, set.bytes(), offset: 48, rowBytes: rowBytes, into: array, width: set.width, height: set.blockH,
                slice: 2
            )
            let points = set.points, at = points.map(\.at), wants = points.map(\.texel.want)
            let levelRead = try await DecodeScene.read(gpu, chain, at, level: 1)
            let sliceRead = try await DecodeScene.read(gpu, array, at, slice: 2)
            for read in [levelRead, sliceRead] {
                let judged = DecodeJudge.halves(read, wants)
                halves.off += judged.off
                halves.nanMismatch += judged.nanMismatch
                texels += read.count
            }
            levelOff += DecodeJudge.notAllNaN(try await DecodeScene.read(gpu, chain, at, level: 0))
            for slice in 0 ..< 2 {
                sliceOff += DecodeJudge.notAllNaN(try await DecodeScene.read(gpu, array, at, slice: slice))
            }
        }
        let want = identityMakesHdr ? 2 * Self.formats.count : 0
        var metrics = Metrics()
        metrics.set("hdr_made", made)
        metrics.set("astc_hdr_texels_off", halves.off)
        metrics.set("astc_hdr_texels", texels)
        metrics.set("astc_hdr_nan_mismatch", halves.nanMismatch)
        metrics.set("astc_hdr_level_off", levelOff)
        metrics.set("astc_hdr_slice_off", sliceOff)
        return .judged(
            [
                ("\(made) HDR textures made, \(want) on this identity", made == want),
                (
                    "\(halves.off) of \(texels) texels off Apple's halves",
                    halves.off == 0 && (texels > 0 || !identityMakesHdr)
                ),
                ("\(halves.nanMismatch) channels' NaN not as Apple's", halves.nanMismatch == 0),
                ("\(levelOff) never-written level 0 texels not NaN", levelOff == 0),
                ("\(sliceOff) never-written slice texels not NaN", sliceOff == 0),
            ], metrics, proof: gpu.proof
        ).onRawPath(gpu)
    }
}

/// A measurement, not of a change: the ASTC 4x4 forms GM20B's texture unit decodes itself, 204 (linear) and 186 (sRGB),
/// uploaded and read as gpu.decode.astc_narrow's are, against an M4 Max's reads of the same blocks (the driver's
/// vectors): 204 reads k / 4095 there (`native_astc_linear_off`: texels whose float is not that; the largest distance
/// reported), 186 its sRGB table (`native_astc_srgb_codes_off`, by the nearest table code). Judged only that the
/// textures are made and their work stays raw; the counts are the native decode's exactness, never measured before.
struct DecodeNativeAstcPrecisionTest: HWTest {
    let id = "gpu.decode.native_astc_precision"
    let name = "GM20B's own ASTC 4x4 decode against an M4 Max's (a measurement)"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var metrics = Metrics()
        var made = true
        let linear = DecodeVectors.set(204), srgb = DecodeVectors.set(186)
        if let texture = try await DecodeScene.astcRow(gpu, linear) {
            let points = linear.points
            let read = try await DecodeScene.read(gpu, texture, points.map(\.at))
            var off = 0
            var largest: Float = 0
            for (texel, point) in zip(read, points) {
                let want = point.texel.want.map { Float($0) / 4095 }
                if (0 ..< 4).contains(where: { texel[$0].bitPattern != want[$0].bitPattern }) { off += 1 }
                for c in 0 ..< 4 { largest = max(largest, abs(texel[c] - want[c])) }
            }
            metrics.set("native_astc_texels", read.count)
            metrics.set("native_astc_linear_off", off)
            metrics.set("native_astc_linear_max_diff", Double(largest), digits: 6)
        } else {
            made = false
        }
        if let texture = try await DecodeScene.astcRow(gpu, srgb) {
            let points = srgb.points
            let read = try await DecodeScene.read(gpu, texture, points.map(\.at))
            let tally = DecodeJudge.rgba8(read, points.map { DecodeJudge.word($0.texel.want) }, srgb: true)
            metrics.set("native_astc_srgb_codes_off", tally.off)
            metrics.set("native_astc_srgb_max_diff", Double(tally.maxDiff), digits: 6)
        } else {
            made = false
        }
        return .judged([("both ASTC 4x4 textures made", made)], metrics, proof: gpu.proof).onRawPath(gpu)
    }
}
