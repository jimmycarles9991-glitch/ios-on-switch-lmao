import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A never-written texture of a format decoded on upload reads what an M4 Max reads from a new one (decoded.h
/// DecodedFormat::fresh, the decode of a block of zeros): 16x16 textures, Private and Shared, of 166 (PVRTC RGBA: 0, 0,
/// 0, 0), 162 (PVRTC RGB: 0, 0, 0, 1), 185 (ASTC 4x2 sRGB: magenta 1, 0, 1, 1 through the sRGB decode), 203 (ASTC 4x2
/// linear: 1, 0, 1, 1) and on 24B119 222 (ASTC 4x4 HDR: a NaN in every channel), and level 2 of a mipmapped 64x64 166;
/// each read at texel (3, 5) by a kernel. `decode_fresh_off` 0: reads that are not those, exactly. Red: a new decoded
/// image left as the memory held it, or a fresh value of the wrong form.
struct DecodeFreshTest: HWTest {
    let id = "gpu.decode.fresh"
    let name = "A never-written decoded texture reads an M4 Max's fresh texel"
    let subsystem = Subsystem.metal

    static let nan = Float.nan
    static let cases: [(format: Int, want: SIMD4<Float>)] = [
        (166, SIMD4(0, 0, 0, 0)), (162, SIMD4(0, 0, 0, 1)), (185, SIMD4(1, 0, 1, 1)), (203, SIMD4(1, 0, 1, 1)),
        (222, SIMD4(nan, nan, nan, nan)),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var off: [String] = [], unmade: [String] = []
        var reads = 0
        let at: [SIMD2<UInt32>] = [SIMD2(3, 5)]
        for storage in [MTLStorageMode.private, .shared] {
            let label = storage == .private ? "private" : "shared"
            for c in Self.cases where c.format != 222 || context.ios >= 27 {
                let format = MTLPixelFormat(rawValue: UInt(c.format))!
                guard let texture = DecodeScene.texture(gpu, format, 16, 16, storage: storage) else {
                    unmade.append("\(c.format)_\(label)")
                    continue
                }
                let read = try await DecodeScene.read(gpu, texture, at)
                reads += read.count
                if DecodeScene.differing(read, [c.want]) > 0 { off.append("\(c.format)_\(label)") }
            }
            let pvrtc = MTLPixelFormat(rawValue: 166)!
            guard let chain = DecodeScene.texture(gpu, pvrtc, 64, 64, storage: storage, levels: 7) else {
                unmade.append("166_chain_\(label)")
                continue
            }
            let read = try await DecodeScene.read(gpu, chain, at, level: 2)
            reads += read.count
            if DecodeScene.differing(read, [SIMD4(0, 0, 0, 0)]) > 0 { off.append("166_level2_\(label)") }
        }
        var metrics = Metrics()
        metrics.set("decode_fresh_off", off.count)
        metrics.set("decode_fresh_reads", reads)
        metrics.set("fresh_off", off.isEmpty ? "-" : off.joined(separator: ","))
        metrics.set("unmade", unmade.isEmpty ? "-" : unmade.joined(separator: ","))
        return .judged(
            [
                ("textures not made: \(unmade)", unmade.isEmpty),
                ("\(off.count) of \(reads) fresh reads off: \(off)", off.isEmpty && reads > 0),
            ], metrics, proof: gpu.proof
        ).onRawPath(gpu)
    }
}

/// A readback of a decoded texture (decoded.h; the owner's choice): PVRTC keeps no blocks, so its getBytes and its
/// blit into a buffer move nothing and the command buffer completes, with one gm20b:tex.decoded_readback line a format a
/// process (a 32x32 166 read by getBytes and by a blit with MTLBlitOptionRowLinearPVRTC, then a 16x16 164 by a blit:
/// two lines, the getBytes's and the blit's); an ASTC form keeps its blocks, so a 64x32 209's getBytes and blit give
/// back the uploaded blocks byte for byte, as Apple's do. `decode_readback_moved` 0 (bytes of the 0xa5 fills a PVRTC
/// read changed), `decode_readback_kept_off` 0; the lines are judged from the pulled log in this run's window against
/// `decode_readback_lines_want` (2 on the process's first run, 0 after: the line is once a format a process), and
/// counted in this process's log here (`decode_readback_lines`, reported). Red: a readback that wrote decoded texels or
/// garbage into the caller's memory, a failed command buffer, or kept blocks not given back.
struct DecodeReadbackTest: HWTest {
    let id = "gpu.decode.readback"
    let name = "A decoded PVRTC texture's readbacks move nothing, an ASTC one's give back its blocks"
    let subsystem = Subsystem.metal

    /// The formats this process has read back: the driver's line is once a format a process
    nonisolated(unsafe) static var lined: Set<Int> = []

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let start = Date()
        let uploaded = Array(DecodeVectors.set(209).bytes().prefix(1024))
        guard uploaded.count == 1024 else { return .fail("the 209 vectors hold fewer than 64 blocks") }
        guard let pvrtc = DecodeScene.texture(gpu, MTLPixelFormat(rawValue: 166)!, 32, 32, storage: .shared),
              let small = DecodeScene.texture(gpu, MTLPixelFormat(rawValue: 164)!, 16, 16, storage: .shared),
              let kept = DecodeScene.texture(gpu, MTLPixelFormat(rawValue: 209)!, 64, 32, storage: .shared),
              let vector = DecodeVectors.level(164, side: 16)
        else { return .fail("a 166, 164 or 209 texture not made") }
        let blocks = DecodeVectors.bytes(Self.words(128, seed: 0x5eed))
        blocks.withUnsafeBytes {
            pvrtc.replace(
                region: MTLRegionMake2D(0, 0, 32, 32), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: 0
            )
        }
        vector.bytes.withUnsafeBytes {
            small.replace(
                region: MTLRegionMake2D(0, 0, 16, 16), mipmapLevel: 0, withBytes: $0.baseAddress!, bytesPerRow: 0
            )
        }
        var fills = [UInt8](repeating: 0xa5, count: 512)
        var linesWant = Self.lined.insert(166).inserted ? 1 : 0
        fills.withUnsafeMutableBytes {
            pvrtc.getBytes($0.baseAddress!, bytesPerRow: 0, from: MTLRegionMake2D(0, 0, 32, 32), mipmapLevel: 0)
        }
        let blitBack = try await Self.blitted(gpu, pvrtc, rowBytes: 64, rows: 8, options: .rowLinearPVRTC)
        linesWant += Self.lined.insert(164).inserted ? 1 : 0
        let smallBack = try await Self.blitted(gpu, small, rowBytes: 16, rows: 4, options: .rowLinearPVRTC)
        let moved = (fills + blitBack + smallBack).filter { $0 != 0xa5 }.count

        try await DecodeScene.upload(gpu, uploaded, rowBytes: 128, into: kept, width: 64, height: 32)
        var back = [UInt8](repeating: 0xa5, count: 1024)
        back.withUnsafeMutableBytes {
            kept.getBytes($0.baseAddress!, bytesPerRow: 128, from: MTLRegionMake2D(0, 0, 64, 32), mipmapLevel: 0)
        }
        let keptBack = try await Self.blitted(gpu, kept, rowBytes: 128, rows: 8, options: [])
        let keptOff = zip(back + keptBack, uploaded + uploaded).filter { $0 != $1 }.count
        let lines = ProcessLog.lines(ProcessLog.refusal, "gm20b", containing: "tex.decoded_readback", since: start)
        var metrics = Metrics()
        metrics.set("decode_readback_moved", moved)
        metrics.set("decode_readback_kept_off", keptOff)
        metrics.set("decode_readback_lines_want", linesWant)
        metrics.set("decode_readback_lines", lines?.count ?? -1)
        return .judged(
            [
                ("\(moved) bytes a PVRTC readback wrote", moved == 0),
                ("\(keptOff) of 2048 bytes of the 209's readbacks not its blocks", keptOff == 0),
            ], metrics, proof: gpu.proof
        ).onRawPath(gpu)
    }

    /// A blit of `texture`'s level 0 into a buffer of 0xa5 bytes, `rows` rows of blocks of `rowBytes`
    static func blitted(
        _ gpu: GPU, _ texture: MTLTexture, rowBytes: Int, rows: Int, options: MTLBlitOption
    ) async throws -> [UInt8] {
        let out = try gpu.buffer([UInt8](repeating: 0xa5, count: rowBytes * rows))
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: texture, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(),
                sourceSize: MTLSize(width: texture.width, height: texture.height, depth: 1), to: out,
                destinationOffset: 0, destinationBytesPerRow: rowBytes, destinationBytesPerImage: rowBytes * rows,
                options: options
            )
            blit.endEncoding()
        }
        return [UInt8](UnsafeRawBufferPointer(start: out.contents(), count: rowBytes * rows))
    }

    /// `count` words of a xorshift from `seed`: blocks whose contents do not matter, only that they are not zeros
    static func words(_ count: Int, seed: UInt32) -> [UInt32] {
        var x = seed
        return (0 ..< count).map { _ in
            x ^= x << 13
            x ^= x >> 17
            x ^= x << 5
            return x
        }
    }
}
