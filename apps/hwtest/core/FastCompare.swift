import Dispatch
import Foundation

/// Byte-level comparisons with memcmp fast paths and pointer loops for hot readback checks.
public enum ByteCompare {
    public static func diffCount(_ a: [UInt8], _ b: [UInt8]) -> Int {
        let count = min(a.count, b.count)
        guard count > 0 else { return 0 }
        return a.withUnsafeBufferPointer { ap in
            b.withUnsafeBufferPointer { bp in
                let pa = ap.baseAddress!, pb = bp.baseAddress!
                if memcmp(pa, pb, count) == 0 { return 0 }
                var diff = 0
                for i in 0 ..< count where pa[i] != pb[i] { diff += 1 }
                return diff
            }
        }
    }

    public static func word(_ b0: UInt8, _ b1: UInt8, _ b2: UInt8, _ b3: UInt8) -> UInt32 {
        UInt32(b0) | UInt32(b1) << 8 | UInt32(b2) << 16 | UInt32(b3) << 24
    }

    public static func wordWrongCount(_ bytes: UnsafeRawBufferPointer, want: UInt32) -> Int {
        let count = bytes.count / 4
        guard count > 0, let base = bytes.baseAddress else { return 0 }
        var wrong = 0
        for i in 0 ..< count where base.loadUnaligned(fromByteOffset: i * 4, as: UInt32.self).littleEndian != want {
            wrong += 1
        }
        return wrong
    }

    public static func wordWrongCount(_ bytes: [UInt8], want: UInt32) -> Int {
        bytes.withUnsafeBytes { wordWrongCount($0, want: want) }
    }

    public static func compareBGRA(_ a: [UInt8], _ b: [UInt8]) -> (beyond: Int, oneOff: [Int]) {
        let count = min(a.count, b.count)
        guard count > 0 else { return (0, [0, 0, 0, 0]) }
        return a.withUnsafeBufferPointer { ap in
            b.withUnsafeBufferPointer { bp in
                let pa = ap.baseAddress!, pb = bp.baseAddress!
                if memcmp(pa, pb, count) == 0 { return (0, [0, 0, 0, 0]) }
                var beyond = 0, o0 = 0, o1 = 0, o2 = 0, o3 = 0
                for i in 0 ..< count {
                    let av = Int(pa[i]), bv = Int(pb[i])
                    if av == bv { continue }
                    let d = av - bv
                    if d < -1 || d > 1 {
                        beyond += 1
                    } else {
                        switch i & 3 {
                        case 0: o0 += 1
                        case 1: o1 += 1
                        case 2: o2 += 1
                        default: o3 += 1
                        }
                    }
                }
                return (beyond, [o0, o1, o2, o3])
            }
        }
    }
}

/// Compares GPU-sampled SIMD4<Float> texels directly against locked CVPixelBuffer plane rows.
public enum NativePlaneCompare {
    public static func wrongBytes(
        sampled: UnsafePointer<SIMD4<Float>>,
        planeBase: UnsafeRawPointer,
        width: Int,
        height: Int,
        bytesPerRow: Int,
        channels: Int,
        sampleBytes: Int
    ) -> Int {
        guard width > 0, height > 0 else { return 0 }
        let chunks = min(max(1, ProcessInfo.processInfo.activeProcessorCount), height)
        var counts = [Int](repeating: 0, count: chunks)
        let plane = planeBase.assumingMemoryBound(to: UInt8.self)
        counts.withUnsafeMutableBufferPointer { out in
            DispatchQueue.concurrentPerform(iterations: chunks) { chunk in
                let y0 = chunk * height / chunks
                let y1 = (chunk + 1) * height / chunks
                var wrong = 0
                if sampleBytes == 2 {
                    for y in y0 ..< y1 {
                        let row = plane.advanced(by: y * bytesPerRow)
                        let texelRow = sampled.advanced(by: y * width)
                        for x in 0 ..< width {
                            let t = texelRow[x]
                            let base = x * channels * 2
                            for c in 0 ..< channels {
                                let v = UInt16((Double(t[c]) * 65535.0).rounded())
                                if row[base + c * 2] != UInt8(v & 0xff) { wrong += 1 }
                                if row[base + c * 2 + 1] != UInt8(v >> 8) { wrong += 1 }
                            }
                        }
                    }
                } else {
                    for y in y0 ..< y1 {
                        let row = plane.advanced(by: y * bytesPerRow)
                        let texelRow = sampled.advanced(by: y * width)
                        for x in 0 ..< width {
                            let t = texelRow[x]
                            let base = x * channels
                            for c in 0 ..< channels {
                                let v = UInt8(UInt16((Double(t[c]) * 255.0).rounded()))
                                if row[base + c] != v { wrong += 1 }
                            }
                        }
                    }
                }
                out[chunk] = wrong
            }
        }
        return counts.reduce(0, +)
    }
}

/// Direct hash and clear-word checks for ScratchOrderTest without materializing pattern arrays.
public enum ScratchOrderCheck {
    public static func wrongA(_ bytes: [UInt8], width: Int, height: Int, seed: UInt32) -> Int {
        let count = width * height
        guard bytes.count >= count * 4, height > 0 else { return 0 }
        let chunks = min(max(1, ProcessInfo.processInfo.activeProcessorCount), height)
        let seedMix = seed &* 83_492_791
        var counts = [Int](repeating: 0, count: chunks)
        bytes.withUnsafeBytes { raw in
            let base = raw.baseAddress!
            counts.withUnsafeMutableBufferPointer { out in
                DispatchQueue.concurrentPerform(iterations: chunks) { chunk in
                    let y0 = chunk * height / chunks
                    let y1 = (chunk + 1) * height / chunks
                    var wrong = 0
                    for y in y0 ..< y1 {
                        let yMix = (UInt32(y) &* 19_349_663) ^ seedMix
                        let rowOff = y * width * 4
                        for x in 0 ..< width {
                            let h = (UInt32(x) &* 73_856_093) ^ yMix
                            let want = ((h >> 16) & 255) | (h & 0xff00) | ((h & 255) << 16) | 0xff00_0000
                            let got = base.loadUnaligned(fromByteOffset: rowOff + x * 4, as: UInt32.self).littleEndian
                            if got != want { wrong += 1 }
                        }
                    }
                    out[chunk] = wrong
                }
            }
        }
        return counts.reduce(0, +)
    }

    public static func wrongB(_ bytes: [UInt8], clearWord: UInt32) -> Int {
        let texels = bytes.count / 4
        guard texels > 0 else { return 0 }
        let chunks = min(max(1, ProcessInfo.processInfo.activeProcessorCount), max(1, texels / 4096))
        var counts = [Int](repeating: 0, count: chunks)
        bytes.withUnsafeBytes { raw in
            let base = raw.baseAddress!
            counts.withUnsafeMutableBufferPointer { out in
                DispatchQueue.concurrentPerform(iterations: chunks) { chunk in
                    let i0 = chunk * texels / chunks
                    let i1 = (chunk + 1) * texels / chunks
                    var wrong = 0
                    for i in i0 ..< i1 {
                        let got = base.loadUnaligned(fromByteOffset: i * 4, as: UInt32.self).littleEndian
                        if got != clearWord { wrong += 1 }
                    }
                    out[chunk] = wrong
                }
            }
        }
        return counts.reduce(0, +)
    }
}

/// Allocation-free quantization and byte packing for FetchBlendState formats.
public enum FetchBlendRef {
    @inline(__always)
    public static func scale(_ format: TexelFormat) -> Float {
        format.isFloat ? 256 : format == .bgr10a2Unorm ? 1023 : 255
    }

    @inline(__always)
    public static func stored(_ n: SIMD4<Int>, _ format: TexelFormat) -> SIMD4<Int> {
        switch format {
        case .rgba8Unorm, .bgra8Unorm:
            return SIMD4(
                min(max(n.x, 0), 255), min(max(n.y, 0), 255),
                min(max(n.z, 0), 255), min(max(n.w, 0), 255)
            )
        case .rgba16Float:
            let round = { (v: Int) -> Int in
                if v >= 0 && v <= 2048 { return v }
                return Int((Float(Float16(Float(v) / 256)) * 256).rounded())
            }
            return SIMD4(round(n.x), round(n.y), round(n.z), round(n.w))
        case .bgr10a2Unorm:
            let a2 = n.w <= 170 ? 0 : n.w <= 511 ? 341 : n.w <= 852 ? 682 : 1023
            return SIMD4(min(max(n.x, 0), 1023), min(max(n.y, 0), 1023), min(max(n.z, 0), 1023), a2)
        default:
            let s = scale(format)
            let values = format.decode(format.encode((0 ..< 4).map { Float(n[$0]) / s })[...])
            return SIMD4((0 ..< 4).map { Int((values[$0] * s).rounded()) })
        }
    }

    public static func bytes(_ pixels: [SIMD4<Int>], _ format: TexelFormat) -> [UInt8] {
        let bpt = format.bytesPerTexel
        var out = [UInt8](repeating: 0, count: pixels.count * bpt)
        switch format {
        case .rgba8Unorm:
            out.withUnsafeMutableBufferPointer { dst in
                for i in 0 ..< pixels.count {
                    let n = pixels[i], at = i * 4
                    dst[at] = UInt8(min(max(n.x, 0), 255))
                    dst[at + 1] = UInt8(min(max(n.y, 0), 255))
                    dst[at + 2] = UInt8(min(max(n.z, 0), 255))
                    dst[at + 3] = UInt8(min(max(n.w, 0), 255))
                }
            }
        case .bgra8Unorm:
            out.withUnsafeMutableBufferPointer { dst in
                for i in 0 ..< pixels.count {
                    let n = pixels[i], at = i * 4
                    dst[at] = UInt8(min(max(n.z, 0), 255))
                    dst[at + 1] = UInt8(min(max(n.y, 0), 255))
                    dst[at + 2] = UInt8(min(max(n.x, 0), 255))
                    dst[at + 3] = UInt8(min(max(n.w, 0), 255))
                }
            }
        case .rgba16Float:
            out.withUnsafeMutableBytes { dst in
                for i in 0 ..< pixels.count {
                    let n = pixels[i], at = i * 8
                    let h0 = Float16(Float(n.x) / 256).bitPattern.littleEndian
                    let h1 = Float16(Float(n.y) / 256).bitPattern.littleEndian
                    let h2 = Float16(Float(n.z) / 256).bitPattern.littleEndian
                    let h3 = Float16(Float(n.w) / 256).bitPattern.littleEndian
                    dst.storeBytes(of: h0, toByteOffset: at, as: UInt16.self)
                    dst.storeBytes(of: h1, toByteOffset: at + 2, as: UInt16.self)
                    dst.storeBytes(of: h2, toByteOffset: at + 4, as: UInt16.self)
                    dst.storeBytes(of: h3, toByteOffset: at + 6, as: UInt16.self)
                }
            }
        case .bgr10a2Unorm:
            out.withUnsafeMutableBytes { dst in
                for i in 0 ..< pixels.count {
                    let n = pixels[i]
                    let r = UInt32(min(max(n.x, 0), 1023))
                    let g = UInt32(min(max(n.y, 0), 1023))
                    let b = UInt32(min(max(n.z, 0), 1023))
                    let a: UInt32 = n.w <= 170 ? 0 : n.w <= 511 ? 1 : n.w <= 852 ? 2 : 3
                    let word = b | g << 10 | r << 20 | a << 30
                    dst.storeBytes(of: word.littleEndian, toByteOffset: i * 4, as: UInt32.self)
                }
            }
        default:
            let s = scale(format)
            return pixels.flatMap { n in format.encode((0 ..< 4).map { Float(n[$0]) / s }) }
        }
        return out
    }
}

/// Allocation-free coherence pixel classifiers for CoherenceRewriteTest and CoherenceBench.
public enum CoherenceCheck {
    public static func countConstantOlder(
        out: [UInt8], cpu: [UInt8], added: SIMD4<Int>, older: SIMD4<Int>
    ) -> (stale: Int, wrong: Int) {
        guard out.count == cpu.count else { return (0, cpu.count / 4) }
        let pixels = cpu.count / 4
        var stale = 0, wrong = 0
        out.withUnsafeBufferPointer { op in
            cpu.withUnsafeBufferPointer { cp in
                let o = op.baseAddress!, c = cp.baseAddress!
                for p in 0 ..< pixels {
                    let at = p * 4
                    let v0 = Int(o[at]), v1 = Int(o[at + 1]), v2 = Int(o[at + 2]), v3 = Int(o[at + 3])
                    let f0 = abs(v0 - min(255, Int(c[at]) + added.x)) <= 1
                    let f1 = abs(v1 - min(255, Int(c[at + 1]) + added.y)) <= 1
                    let f2 = abs(v2 - min(255, Int(c[at + 2]) + added.z)) <= 1
                    let f3 = abs(v3 - min(255, Int(c[at + 3]) + added.w)) <= 1
                    if f0 && f1 && f2 && f3 { continue }
                    let old =
                        abs(v0 - older.x) <= 1 && abs(v1 - older.y) <= 1 && abs(v2 - older.z) <= 1
                            && abs(v3 - older.w) <= 1
                    if old { stale += 1 } else { wrong += 1 }
                }
            }
        }
        return (stale, wrong)
    }

    public static func countBench(
        out: UnsafeRawBufferPointer, cpu: [UInt8], gpu: UnsafeRawBufferPointer
    ) -> (stale: Int, wrong: Int, cpuStale: Int) {
        guard out.count == cpu.count, gpu.count == cpu.count else { return (0, cpu.count / 4, 0) }
        let pixels = cpu.count / 4
        var stale = 0, wrong = 0, cpuStale = 0
        let o = out.bindMemory(to: UInt8.self).baseAddress!
        let g = gpu.bindMemory(to: UInt8.self).baseAddress!
        cpu.withUnsafeBufferPointer { cp in
            let c = cp.baseAddress!
            for p in 0 ..< pixels {
                let at = p * 4
                let g0 = Int(g[at]), g1 = Int(g[at + 1]), g2 = Int(g[at + 2]), g3 = Int(g[at + 3])
                if abs(g0 - 16) > 1 || abs(g1 - 16) > 1 || abs(g2 - 16) > 1 || abs(g3 - 16) > 1 {
                    cpuStale += 1
                }
                let v0 = Int(o[at]), v1 = Int(o[at + 1]), v2 = Int(o[at + 2]), v3 = Int(o[at + 3])
                let fresh =
                    abs(v0 - min(255, Int(c[at]) + g0)) <= 1 && abs(v1 - min(255, Int(c[at + 1]) + g1)) <= 1
                        && abs(v2 - min(255, Int(c[at + 2]) + g2)) <= 1 && abs(v3 - min(255, Int(c[at + 3]) + g3)) <= 1
                if fresh { continue }
                let old0 =
                    abs(v0 - min(255, 2 * g0)) <= 1 && abs(v1 - min(255, 2 * g1)) <= 1
                        && abs(v2 - min(255, 2 * g2)) <= 1 && abs(v3 - min(255, 2 * g3)) <= 1
                let old1 =
                    abs(v0 - g0) <= 1 && abs(v1 - g1) <= 1 && abs(v2 - g2) <= 1 && abs(v3 - g3) <= 1
                if old0 || old1 { stale += 1 } else { wrong += 1 }
            }
        }
        return (stale, wrong, cpuStale)
    }
}

/// Parallel CPU reference generators for ResolveTwodTest.
public enum ResolveRef {
    @inline(__always)
    public static func sampleHash(_ x: Int, _ y: Int, _ sample: Int) -> UInt32 {
        var h = (UInt32(x) &* 73_856_093) ^ (UInt32(y) &* 19_349_663) ^ (UInt32(sample + 1) &* 83_492_791)
        h ^= h >> 13
        h = h &* 0x5bd1_e995
        h ^= h >> 15
        return h
    }

    public static func reference(width: Int, height: Int, samples: Int) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: width * height * 4)
        let n = UInt32(samples), half = UInt32(samples / 2)
        let chunks = min(max(1, ProcessInfo.processInfo.activeProcessorCount), max(1, height))
        out.withUnsafeMutableBufferPointer { buf in
            let dst = buf.baseAddress!
            DispatchQueue.concurrentPerform(iterations: chunks) { chunk in
                let y0 = chunk * height / chunks
                let y1 = (chunk + 1) * height / chunks
                for y in y0 ..< y1 {
                    let yMix = UInt32(y) &* 19_349_663
                    let rowOff = y * width * 4
                    for x in 0 ..< width {
                        let xyMix = (UInt32(x) &* 73_856_093) ^ yMix
                        var b: UInt32 = 0, g: UInt32 = 0, r: UInt32 = 0, a: UInt32 = 0
                        for s in 0 ..< samples {
                            var h = xyMix ^ (UInt32(s + 1) &* 83_492_791)
                            h ^= h >> 13
                            h = h &* 0x5bd1_e995
                            h ^= h >> 15
                            r += h & 255
                            g += (h >> 8) & 255
                            b += (h >> 16) & 255
                            a += h >> 24
                        }
                        let at = rowOff + x * 4
                        dst[at] = UInt8((b + half) / n)
                        dst[at + 1] = UInt8((g + half) / n)
                        dst[at + 2] = UInt8((r + half) / n)
                        dst[at + 3] = UInt8((a + half) / n)
                    }
                }
            }
        }
        return out
    }

    public static func engineReading(width: Int, height: Int) -> (box: [UInt8], complement: [UInt8]) {
        var box = [UInt8](repeating: 0, count: width * height * 4)
        var complement = [UInt8](repeating: 0, count: width * height * 4)
        let chunks = min(max(1, ProcessInfo.processInfo.activeProcessorCount), max(1, height))
        box.withUnsafeMutableBufferPointer { boxBuf in
            complement.withUnsafeMutableBufferPointer { compBuf in
                let bp = boxBuf.baseAddress!, cp = compBuf.baseAddress!
                DispatchQueue.concurrentPerform(iterations: chunks) { chunk in
                    let y0 = chunk * height / chunks
                    let y1 = (chunk + 1) * height / chunks
                    for y in y0 ..< y1 {
                        let rowOff = y * width * 4
                        for x in 0 ..< width {
                            let h0 = sampleHash(x, y, 0), h1 = sampleHash(x, y, 1)
                            let at = rowOff + x * 4
                            let b0 = (h0 >> 16) & 255, b1 = (h1 >> 16) & 255
                            let g0 = (h0 >> 8) & 255, g1 = (h1 >> 8) & 255
                            let r0 = h0 & 255, r1 = h1 & 255
                            let a0 = h0 >> 24, a1 = h1 >> 24
                            bp[at] = UInt8(X1TwodResolveReading.box(b0, b1))
                            cp[at] = UInt8(X1TwodResolveReading.complement(b0, b1))
                            bp[at + 1] = UInt8(X1TwodResolveReading.box(g0, g1))
                            cp[at + 1] = UInt8(X1TwodResolveReading.complement(g0, g1))
                            bp[at + 2] = UInt8(X1TwodResolveReading.box(r0, r1))
                            cp[at + 2] = UInt8(X1TwodResolveReading.complement(r0, r1))
                            bp[at + 3] = UInt8(X1TwodResolveReading.box(a0, a1))
                            cp[at + 3] = UInt8(X1TwodResolveReading.complement(a0, a1))
                        }
                    }
                }
            }
        }
        return (box, complement)
    }
}
