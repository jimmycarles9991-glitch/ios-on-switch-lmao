import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUForms
import HWTestKit
import Metal

/// A 64-texel RGB10A8_2P_XR10 (550) and sRGB (551) texture buffer, ShaderRead and ShaderWrite, over a 16384x1 'BGRA'
/// surface of 0xff bytes (gpu/XrTexelBufferWritten.swift), written (0.5, 0.25, -0.25, 0.75) by a kernel and, over a
/// fresh surface, by a fragment function at its column, then read back by a kernel. An M4 Max under macOS 27.2.1 (this
/// test's scene code, three runs alike): every texture made (xr_tbuf_written_made; nil before the driver held a
/// written one as its words), each store leaves every word 0x27f80100 (551: 0x2f7a486e) and no other byte changed
/// (xr_tbuf_kernel_words_off, xr_tbuf_fragment_words_off, xr_tbuf_bytes_outside 0), and every texel reads (0.5,
/// 0.2509804, -0.2509804, 0) (551: (0.49987793, 0.25012207, -0.25012207, 0)) within half a code (xr_tbuf_read_off 0,
/// xr_tbuf_read_alpha 0).
struct XrTexelBufferWrittenTest: HWTest {
    let id = "gpu.gm20b.xr_texel_buffer_written"
    let name = "Written RGB10A8_2P_XR10 texel buffers over a surface store and read an M4 Max's words, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    static let apple: [(format: UInt, word: UInt32, read: SIMD4<Float>)] = [
        (550, 0x27f8_0100, SIMD4(0.5, 0.2509804, -0.2509804, 0)),
        (551, 0x2f7a_486e, SIMD4(0.49987793, 0.25012207, -0.25012207, 0)),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            var checks: [(String, Bool)] = []
            var made = true, kernelOff = 0, fragmentOff = 0, outside = 0, readOff = 0, alpha: Float = 0
            for arm in Self.apple {
                for fragment in [false, true] {
                    let label = "\(arm.format) \(fragment ? "fragment" : "kernel")"
                    do {
                        let got = try await XrTexelBufferWritten.observe(
                            gpu, format: arm.format, fragment: fragment, word: arm.word
                        )
                        made = made && got.made
                        guard got.made else {
                            checks.append(("\(label): no texture", false))
                            continue
                        }
                        guard !got.status.hasPrefix("error:") else {
                            checks.append(("\(label): \(got.status)", false))
                            continue
                        }
                        if fragment { fragmentOff += got.wordsOff } else { kernelOff += got.wordsOff }
                        outside += got.bytesOutside
                        let off = Self.readOff(got.read, arm.read)
                        readOff += off
                        alpha = max(alpha, got.read.map { abs($0.w) }.max() ?? 0)
                        checks.append(
                            (
                                "\(label): \(got.wordsOff) words off 0x\(String(arm.word, radix: 16)) (word 0 "
                                    + "0x\(String(got.first, radix: 16))), \(got.bytesOutside) bytes past them "
                                    + "changed, \(off) texels read off",
                                got.wordsOff == 0 && got.bytesOutside == 0 && off == 0
                            )
                        )
                    } catch {
                        made = false
                        checks.append(("\(label): \(GPU.describe(error))", false))
                    }
                }
            }
            metrics.set("xr_tbuf_written_made", made)
            metrics.set("xr_tbuf_kernel_words_off", kernelOff)
            metrics.set("xr_tbuf_fragment_words_off", fragmentOff)
            metrics.set("xr_tbuf_bytes_outside", outside)
            metrics.set("xr_tbuf_read_off", readOff)
            metrics.set("xr_tbuf_read_alpha", Double(alpha), digits: 6)
            return checks
        }
    }

    static func readOff(_ texels: [SIMD4<Float>], _ want: SIMD4<Float>) -> Int {
        let halfCode: Float = 0.5 / 510
        return texels.filter { t in (0 ..< 3).contains { abs(t[$0] - want[$0]) >= halfCode } || t.w != want.w }.count
    }
}
