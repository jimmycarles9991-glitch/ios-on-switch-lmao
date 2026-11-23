import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Metal's exact roundings in compute (ExactRoundModel, which an M4 Max follows on every value here). packs: MSL's
/// pack_float_to_unorm4x8, snorm4x8, unorm2x16, snorm2x16 and unorm10a2 of each code boundary (k + 0.5) / top and 1
/// and 2 floats either side, of each code's value and of specials (8- and 10-bit every k, 16-bit every 61st or 31st),
/// each against the exact product rounded to nearest even (exact_round_values, exact_round_wrong: words off). xr:
/// BGR10_XR (554), BGRA10_XR (552) and RGB10A8_2P_XR10 (550 over a 'b3a8' surface), 32x32, texel i holding code i in
/// every colour channel (552's alpha too, 550's alpha byte i & 255): read by kernels as floats and as halves, each
/// (code - 384) / 510 nearest, 552's alpha clamped to [0, 1] as an M4 Max reads it, 550's alpha byte / 255
/// (xr_decode_wrong, xr_decode_half_wrong: channels off), then those values written by
/// kernels as floats and as halves into a texture of the format, read back as codes (xr_encode_wrong,
/// xr_encode_half_wrong: channels off). All 0; every command buffer on the driver.
struct ExactRoundTest: HWTest {
    let id = "gpu.compute.exact_round"
    let name = "MSL float packs and extended-range codes read and written in compute round as Metal's, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    struct XrFormat {
        let name: String
        let pixel: MTLPixelFormat
        let bytes: Int
        /// The alpha channel's code of texel i, and whether it is an extended-range code (else a byte)
        let alpha: ((Int) -> Int)?
        let alphaXR: Bool
    }

    static let side = 32
    static let formats = [
        XrFormat(name: "bgr10xr", pixel: .bgr10_xr, bytes: 4, alpha: nil, alphaXR: false),
        XrFormat(name: "bgra10xr", pixel: .bgra10_xr, bytes: 8, alpha: { $0 }, alphaXR: true),
        XrFormat(name: "xr550", pixel: XRTwoPlaneTest.format, bytes: 4, alpha: { $0 & 255 }, alphaXR: false),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        do {
            let wrong = try await Self.packs(gpu)
            metrics.set("exact_round_values", ExactRoundModel.values.count)
            metrics.set("exact_round_wrong", wrong)
            checks.append(("every pack's word Metal's", wrong == 0))
        } catch {
            checks.append(("packs: \(GPU.describe(error))", false))
        }
        var xr = [0, 0, 0, 0]
        for format in Self.formats {
            do {
                let counts = try await Self.roundTrip(gpu, format)
                xr = zip(xr, counts).map { $0 + $1 }
            } catch {
                checks.append(("\(format.name): \(GPU.describe(error))", false))
            }
        }
        for (key, count) in zip(
            ["xr_decode_wrong", "xr_decode_half_wrong", "xr_encode_wrong", "xr_encode_half_wrong"], xr
        )
        {
            metrics.set(key, count)
            checks.append(("\(key) 0", count == 0))
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func dispatch(
        _ gpu: GPU, _ kernel: String, width: Int, height: Int, _ bind: @escaping (MTLComputeCommandEncoder) -> Void
    )
        async throws
    {
        let state = try gpu.compute(kernel)
        try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(state)
            bind(encoder)
            encoder.dispatchThreads(
                MTLSize(width: width, height: height, depth: 1),
                threadsPerThreadgroup: MTLSize(width: height == 1 ? 64 : 8, height: height == 1 ? 1 : 8, depth: 1)
            )
            encoder.endEncoding()
        }
    }

    /// Pack words off the model
    static func packs(_ gpu: GPU) async throws -> Int {
        let values = ExactRoundModel.values
        let count = (values.count + 63) / 64 * 64
        let input = try gpu.buffer(values + [Float](repeating: 0, count: count - values.count))
        let out = try gpu.buffer(count * 20)
        try await dispatch(gpu, "exact_round_packs", width: count, height: 1) {
            $0.setBuffer(input, offset: 0, index: 0)
            $0.setBuffer(out, offset: 0, index: 1)
        }
        let words = out.contents().bindMemory(to: UInt32.self, capacity: count * 5)
        return values.enumerated().map { i, v in
            zip(ExactRoundModel.words(v), (0 ..< 5).map { words[i * 5 + $0] }).filter { $0 != $1 }.count
        }.reduce(0, +)
    }

    /// Texel i's codes, red first (alpha last where the format has one)
    static func codes(_ format: XrFormat, _ i: Int) -> [Int] { [i, i, i] + (format.alpha.map { [$0(i)] } ?? []) }

    static func texel(_ format: XrFormat, _ c: [Int]) -> [UInt8] {
        if format.bytes == 8 {
            return [c[2], c[1], c[0], c[3]].flatMap { [UInt8(($0 << 6) & 0xff), UInt8($0 >> 2)] }
        }
        let word = UInt32(c[0]) << 20 | UInt32(c[1]) << 10 | UInt32(c[2])
        return (0 ..< 4).map { UInt8(truncatingIfNeeded: word >> (8 * $0)) }
    }

    static func stored(_ format: XrFormat, _ bytes: [UInt8], _ i: Int) -> [Int] {
        if format.bytes == 8 {
            let word = { (k: Int) in (Int(bytes[i * 8 + 2 * k]) | Int(bytes[i * 8 + 2 * k + 1]) << 8) >> 6 }
            return [word(2), word(1), word(0), word(3)]
        }
        let w = bytes.loaded(as: UInt32.self, at: i)
        return [Int(w >> 20 & 0x3ff), Int(w >> 10 & 0x3ff), Int(w & 0x3ff)]
    }

    /// Texel i's codes as Metal reads and writes them: an extended-range alpha clamped to [0, 1], codes 384..894
    static func held(_ format: XrFormat, _ i: Int) -> [Int] {
        codes(format, i).enumerated().map { c, code in c == 3 && format.alphaXR ? min(max(code, 384), 894) : code }
    }

    /// The value of channel c of texel i, and its half
    static func value(_ format: XrFormat, _ i: Int, _ c: Int) -> (Float, UInt16) {
        let code = held(format, i)[c]
        if c == 3 && !format.alphaXR {
            return (Float(Double(code) / 255), Float16(Double(code) / 255).bitPattern)
        }
        return (ExactRoundModel.xr(code), ExactRoundModel.xrHalf(code))
    }

    /// A texture of the format, usage ShaderRead|ShaderWrite (never one an Apple GPU keeps compressed), with texel i
    /// holding codes((i + shift) % 1024); its codes read back
    static func made(
        _ gpu: GPU, _ format: XrFormat, shift: Int = 0
    ) async throws -> (MTLTexture, () async throws -> [[Int]]) {
        let count = side * side
        if format.pixel != XRTwoPlaneTest.format {
            let texture = try gpu.texture(format.pixel, side, side, usage: [.shaderRead, .shaderWrite])
            let bytes = (0 ..< count).flatMap { texel(format, codes(format, ($0 + shift) % count)) }
            try await gpu.upload(bytes, to: texture, bytesPerRow: side * format.bytes)
            return (
                texture,
                {
                    let read = try await gpu.read(texture, bytesPerPixel: format.bytes)
                    return (0 ..< count).map { stored(format, read, $0) }
                }
            )
        }
        let (buffer, texture) = try XRTwoPlaneTest.surfaceTexture(
            gpu, width: side, height: side, usage: [.shaderRead, .shaderWrite]
        )
        CVPixelBufferLockBaseAddress(buffer, [])
        let word = CVPixelBufferGetBaseAddressOfPlane(buffer, 0)!,
            alpha = CVPixelBufferGetBaseAddressOfPlane(buffer, 1)!
        let (wordRow, alphaRow) = (
            CVPixelBufferGetBytesPerRowOfPlane(buffer, 0), CVPixelBufferGetBytesPerRowOfPlane(buffer, 1)
        )
        for i in 0 ..< count {
            let c = codes(format, (i + shift) % count)
            word.storeBytes(
                of: UInt32(c[0] << 20 | c[1] << 10 | c[2]), toByteOffset: (i / side) * wordRow + (i % side) * 4,
                as: UInt32.self
            )
            alpha.storeBytes(of: UInt8(c[3]), toByteOffset: (i / side) * alphaRow + i % side, as: UInt8.self)
        }
        CVPixelBufferUnlockBaseAddress(buffer, [])
        return (
            texture,
            {
                XRTwoPlaneTest.planes(buffer, width: side, height: side).map {
                    [Int($0 >> 20 & 0x3ff), Int($0 >> 10 & 0x3ff), Int($0 & 0x3ff), Int($0 >> 32 & 0xff)]
                }
            }
        )
    }

    /// Channels off Metal's: read as floats, read as halves, written as floats, written as halves
    static func roundTrip(_ gpu: GPU, _ format: XrFormat) async throws -> [Int] {
        let count = side * side, channels = format.alpha == nil ? 3 : 4
        let (source, _) = try await made(gpu, format)
        let floats = try await gpu.texels(source)
        let halves = try gpu.buffer(count * 8)
        try await dispatch(gpu, "x1_texture_codes_half", width: side, height: side) {
            $0.setTexture(source, index: 0)
            $0.setBuffer(halves, offset: 0, index: 0)
        }
        let halfBits = halves.contents().bindMemory(to: UInt16.self, capacity: count * 4)
        var counts = [0, 0, 0, 0]
        for i in 0 ..< count {
            for c in 0 ..< channels {
                let (f, h) = value(format, i, c)
                counts[0] += floats[i][c].bitPattern == f.bitPattern ? 0 : 1
                counts[1] += halfBits[i * 4 + c] == h ? 0 : 1
            }
        }
        let values = try gpu.buffer(
            (0 ..< count).map { i in SIMD4<Float>((0 ..< 4).map { c in c < channels ? value(format, i, c).0 : 1 }) }
        )
        for (slot, kernel) in [(2, "exact_round_write_float"), (3, "exact_round_write_half")] {
            let (target, codes) = try await made(gpu, format, shift: count / 2)
            try await dispatch(gpu, kernel, width: side, height: side) {
                $0.setTexture(target, index: 0)
                $0.setBuffer(values, offset: 0, index: 0)
            }
            let read = try await codes()
            for i in 0 ..< count {
                counts[slot] += zip(read[i], held(format, i)).filter { $0 != $1 }.count
            }
        }
        return counts
    }
}
