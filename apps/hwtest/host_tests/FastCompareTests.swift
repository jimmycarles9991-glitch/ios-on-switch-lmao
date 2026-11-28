import Foundation
import HWTestCore
import XCTest

final class FastCompareTests: XCTestCase {
    func testByteCompareAndBGRA() {
        let a = (0 ..< 4096).map { UInt8(($0 &* 37) & 255) }
        var b = a
        XCTAssertEqual(ByteCompare.diffCount(a, b), 0)
        let eq = ByteCompare.compareBGRA(a, b)
        XCTAssertEqual(eq.beyond, 0)
        XCTAssertEqual(eq.oneOff, [0, 0, 0, 0])

        b[0] = b[0] &+ 1
        b[5] = b[5] &- 1
        b[10] = b[10] &+ 1
        b[15] = b[15] &- 1
        b[20] = b[20] ^ 0x40
        let refDiff = zip(a, b).filter { $0 != $1 }.count
        XCTAssertEqual(ByteCompare.diffCount(a, b), refDiff)

        var refBeyond = 0, refOne = [0, 0, 0, 0]
        for i in 0 ..< a.count where a[i] != b[i] {
            if abs(Int(a[i]) - Int(b[i])) > 1 { refBeyond += 1 } else { refOne[i % 4] += 1 }
        }
        let got = ByteCompare.compareBGRA(a, b)
        XCTAssertEqual(got.beyond, refBeyond)
        XCTAssertEqual(got.oneOff, refOne)

        let clearBytes: [UInt8] = [191, 128, 64, 255]
        var clearBuf = (0 ..< 64).flatMap { _ in clearBytes }
        clearBuf[12] = 0
        clearBuf[43] = 1
        let wantWord = ByteCompare.word(191, 128, 64, 255)
        let refWrong = stride(from: 0, to: clearBuf.count, by: 4).filter {
            !clearBuf[$0 ..< ($0 + 4)].elementsEqual(clearBytes)
        }.count
        XCTAssertEqual(ByteCompare.wordWrongCount(clearBuf, want: wantWord), refWrong)
    }

    func testNativePlaneCompareMatchesFlatMapReference() {
        for (sampleBytes, channels) in [(1, 1), (1, 2), (2, 1), (2, 2)] {
            let width = 37, height = 19, bpp = channels * sampleBytes
            let bytesPerRow = (width * bpp + 31) & ~31
            let scale = sampleBytes == 2 ? 65535.0 : 255.0
            var texels = [SIMD4<Float>](repeating: .zero, count: width * height)
            var plane = [UInt8](repeating: 0xaa, count: height * bytesPerRow)
            for y in 0 ..< height {
                for x in 0 ..< width {
                    let v0 = Float((x * 13 + y * 29) % 256) / 255
                    let v1 = Float((x * 7 + y * 41) % 256) / 255
                    texels[y * width + x] = SIMD4(v0, v1, 0, 1)
                    for c in 0 ..< channels {
                        let q = UInt16((Double(texels[y * width + x][c]) * scale).rounded())
                        let off = y * bytesPerRow + x * bpp + c * sampleBytes
                        if sampleBytes == 2 {
                            plane[off] = UInt8(q & 0xff)
                            plane[off + 1] = UInt8(q >> 8)
                        } else {
                            plane[off] = UInt8(q)
                        }
                    }
                }
            }
            plane[3 * bytesPerRow + 5 * bpp] ^= 1
            plane[11 * bytesPerRow + 17 * bpp + (sampleBytes - 1)] ^= 0x80

            let refSampled: [UInt8] = texels.flatMap { texel in
                (0 ..< channels).flatMap { c -> [UInt8] in
                    let value = UInt16((Double(texel[c]) * scale).rounded())
                    return sampleBytes == 2 ? [UInt8(value & 0xff), UInt8(value >> 8)] : [UInt8(value)]
                }
            }
            let refRows: [UInt8] = (0 ..< height).flatMap { y in
                Array(plane[(y * bytesPerRow) ..< (y * bytesPerRow + width * bpp)])
            }
            let refWrong = zip(refSampled, refRows).filter { $0 != $1 }.count
            let gotWrong = texels.withUnsafeBufferPointer { tp in
                plane.withUnsafeBytes { pp in
                    NativePlaneCompare.wrongBytes(
                        sampled: tp.baseAddress!, planeBase: pp.baseAddress!, width: width, height: height,
                        bytesPerRow: bytesPerRow, channels: channels, sampleBytes: sampleBytes
                    )
                }
            }
            XCTAssertEqual(gotWrong, refWrong, "sampleBytes=\(sampleBytes) channels=\(channels)")
        }
    }

    func testScratchOrderCheckMatchesPatternReference() {
        let width = 43, height = 29, seed: UInt32 = 5
        var pattern = [UInt8](repeating: 0, count: width * height * 4)
        for y in 0 ..< height {
            for x in 0 ..< width {
                let h = (UInt32(x) &* 73_856_093) ^ (UInt32(y) &* 19_349_663) ^ (seed &* 83_492_791)
                let at = (y * width + x) * 4
                pattern[at] = UInt8((h >> 16) & 255)
                pattern[at + 1] = UInt8((h >> 8) & 255)
                pattern[at + 2] = UInt8(h & 255)
                pattern[at + 3] = 255
            }
        }
        var gotA = pattern
        gotA[0] ^= 1
        gotA[400] ^= 2
        gotA[401] ^= 4
        let refA = stride(from: 0, to: gotA.count, by: 4).filter { gotA[$0 ..< ($0 + 4)] != pattern[$0 ..< ($0 + 4)] }
            .count
        XCTAssertEqual(ScratchOrderCheck.wrongA(gotA, width: width, height: height, seed: seed), refA)

        let clearBytes: [UInt8] = [191, 128, 64, 255]
        var gotB = (0 ..< (width * height)).flatMap { _ in clearBytes }
        gotB[16] = 1
        gotB[99] = 2
        let refB = stride(from: 0, to: gotB.count, by: 4).filter { Array(gotB[$0 ..< ($0 + 4)]) != clearBytes }.count
        XCTAssertEqual(ScratchOrderCheck.wrongB(gotB, clearWord: ByteCompare.word(191, 128, 64, 255)), refB)
    }

    func testFetchBlendRefMatchesEncodeDecodeBitForBit() {
        let formats: [TexelFormat] = [.rgba8Unorm, .bgra8Unorm, .rgba16Float, .bgr10a2Unorm]
        for format in formats {
            let scale = FetchBlendRef.scale(format)
            var pixels: [SIMD4<Int>] = []
            for v in -5 ... 1030 where format == .bgr10a2Unorm || v <= 260 {
                let n = SIMD4<Int>(v, (v * 3) % 260, (v * 7) % 260, v)
                let refDecoded = format.decode(format.encode((0 ..< 4).map { Float(n[$0]) / scale })[...])
                let refStored = SIMD4((0 ..< 4).map { Int((refDecoded[$0] * scale).rounded()) })
                XCTAssertEqual(FetchBlendRef.stored(n, format), refStored, "\(format.rawValue) n=\(n)")
                if pixels.count < 64 { pixels.append(refStored) }
            }
            let refBytes = pixels.flatMap { n in format.encode((0 ..< 4).map { Float(n[$0]) / scale }) }
            XCTAssertEqual(FetchBlendRef.bytes(pixels, format), refBytes, "\(format.rawValue) bytes")
        }
    }

    func testCoherenceCheckMatchesOriginalClassifier() {
        let pixels = 128
        let cpu = (0 ..< (pixels * 4)).map { UInt8(($0 &* 7 &+ 13) % 200) }
        let added = SIMD4<Int>(3, 2, 1, 4)
        let older = SIMD4<Int>(215, 220, 225, 244)
        var out = (0 ..< (pixels * 4)).map { i in UInt8(min(255, Int(cpu[i]) + added[i % 4])) }
        for c in 0 ..< 4 { out[4 * 3 + c] = UInt8(older[c]) }
        for c in 0 ..< 4 { out[4 * 7 + c] = UInt8(older[c] - 1) }
        out[4 * 11] = 0
        let gotConst = CoherenceCheck.countConstantOlder(out: out, cpu: cpu, added: added, older: older)
        XCTAssertEqual(gotConst.stale, 2)
        XCTAssertEqual(gotConst.wrong, 1)

        var gpu = [UInt8](repeating: 16, count: pixels * 4)
        gpu[4 * 2] = 20
        var benchOut = (0 ..< (pixels * 4)).map { i in UInt8(min(255, Int(cpu[i]) + Int(gpu[i]))) }
        for c in 0 ..< 4 { benchOut[4 * 5 + c] = UInt8(min(255, 2 * Int(gpu[4 * 5 + c]))) }
        for c in 0 ..< 4 { benchOut[4 * 9 + c] = gpu[4 * 9 + c] }
        benchOut[4 * 13] = 250
        let gotBench = benchOut.withUnsafeBytes { ob in
            gpu.withUnsafeBytes { gb in CoherenceCheck.countBench(out: ob, cpu: cpu, gpu: gb) }
        }
        XCTAssertEqual(gotBench.cpuStale, 1)
        XCTAssertEqual(gotBench.stale, 2)
        XCTAssertEqual(gotBench.wrong, 1)
    }

    func testResolveRefMatchesSerialReference() {
        let width = 37, height = 23
        for samples in [2, 4] {
            var ref = [UInt8](repeating: 0, count: width * height * 4)
            let n = UInt32(samples), half = UInt32(samples / 2)
            for y in 0 ..< height {
                for x in 0 ..< width {
                    var b: UInt32 = 0, g: UInt32 = 0, r: UInt32 = 0, a: UInt32 = 0
                    for s in 0 ..< samples {
                        let h = ResolveRef.sampleHash(x, y, s)
                        r += h & 255
                        g += (h >> 8) & 255
                        b += (h >> 16) & 255
                        a += h >> 24
                    }
                    let at = (y * width + x) * 4
                    ref[at] = UInt8((b + half) / n)
                    ref[at + 1] = UInt8((g + half) / n)
                    ref[at + 2] = UInt8((r + half) / n)
                    ref[at + 3] = UInt8((a + half) / n)
                }
            }
            XCTAssertEqual(ResolveRef.reference(width: width, height: height, samples: samples), ref)
        }
        var refBox = [UInt8](repeating: 0, count: width * height * 4), refComp = refBox
        let shift: [UInt32] = [16, 8, 0, 24]
        for y in 0 ..< height {
            for x in 0 ..< width {
                let h0 = ResolveRef.sampleHash(x, y, 0), h1 = ResolveRef.sampleHash(x, y, 1)
                for c in 0 ..< 4 {
                    let a = (h0 >> shift[c]) & 255, b = (h1 >> shift[c]) & 255
                    refBox[(y * width + x) * 4 + c] = UInt8(X1TwodResolveReading.box(a, b))
                    refComp[(y * width + x) * 4 + c] = UInt8(X1TwodResolveReading.complement(a, b))
                }
            }
        }
        let got = ResolveRef.engineReading(width: width, height: height)
        XCTAssertEqual(got.box, refBox)
        XCTAssertEqual(got.complement, refComp)
    }
}
