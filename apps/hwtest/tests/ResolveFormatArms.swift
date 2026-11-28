import Foundation
import HWTestCore
import HWTestGPU
import HWTestX1
import Metal

/// gpu.gm20b.resolve_twod's format arms (resolve_rule.h twodResolveFit): per format and sample count the codes the
/// format stores for each sample (one-sample draws of its colour, resolve_twod_sample_fragment), the resolve the rule
/// picks for a private target only passes sample, and the pack's pass where the format has a resolve state, each held
/// to Metal's rule computed from the stored codes on every third texel each way. An M4 Max's resolve of this scene
/// sets the rule (25 Nov, every channel of the 720x1280 target): unorm nearest, sRGB averaged as linear light,
/// RG11B10Float's mean to nearest with ties to even at 2x and 4x, and RGBA16Float's to nearest with ties up (this
/// scene has no negative value), every channel at 2x and all but 71 of 3,686,400 at 4x, where ties to even misses
/// 614,218 and 190,766.
/// `<cell>=<route>:<grade>:<texels off Metal>:<channels beyond one>`, `<cell>_pass=` the
/// same of the pass (the reference's control), `<cell>_ms=<picked>/<pass>` GPU ms. FAIL: a cell off resolve.twod, a
/// channel beyond one code, or an exact cell (RGBA16Float at 2x) off Metal, where its read arm
/// (`rgba16f_2x_read=<route>:<off>`) must take resolve.twod too; `<cell>_error`: the cell could not run. Then the
/// engine arms (ResolveEngineArms.swift). A cell or arm with a channel beyond one code adds
/// `<cell>[_<arm>]_beyond_texels` to the report.
extension ResolveTwodTest {
    struct FormatCell {
        let format: X1Format
        let samples: Int
        let exact: Bool
        let twod: Bool
        let pass: Bool
        var name: String { "\(format.name)_\(samples)x" }
    }

    static var formatCells: [FormatCell] {
        X1Format.named(["bgra8srgb", "rgba8srgb", "rgba16f", "rgb10a2", "rg11b10f"]).flatMap { format in
            [2, 4].map { n in
                FormatCell(
                    format: format, samples: n, exact: format.name == "rgba16f" && n == 2,
                    twod: !(format.name.hasSuffix("srgb") && n == 4), pass: format.name != "rgb10a2"
                )
            }
        }
    }

    func formatArms(_ gpu: GPU, _ facade: NSObject, _ metrics: inout Metrics, _ wrong: inout [String]) async {
        for cell in Self.formatCells where gpu.device.supportsTextureSampleCount(cell.samples) {
            do {
                try await formatArm(cell, gpu, facade, &metrics, &wrong)
            } catch {
                metrics.set("\(cell.name)_error", String(describing: error).prefix(80).description)
                wrong.append("\(cell.name)_error")
            }
        }
    }

    func formatArm(
        _ cell: FormatCell, _ gpu: GPU, _ facade: NSObject, _ metrics: inout Metrics, _ wrong: inout [String]
    ) async throws {
        let format = cell.format, size = format.bytesPerTexel
        let state = try gpu.render(
            fragment: "resolve_twod_fragment", format: format.pixelFormat, samples: cell.samples
        )
        let stored = try await storedSamples(gpu, format, samples: cell.samples)
        let picked = try await resolved(
            gpu, state, samples: cell.samples, format: format.pixelFormat, bytesPerTexel: size
        )
        let (off, beyond) = Self.offMetal(format, picked.bytes, stored)
        let route = picked.twod > 0 ? "twod" : "pass"
        metrics.set(cell.name, "\(route):\(Self.grade(off, beyond)):\(off):\(beyond)")
        Self.noteBeyond(cell.name, Self.beyondTexels(format, picked.bytes, stored), beyond, &metrics)
        var passMs = "-"
        if cell.pass {
            let pass = try await switched(facade, Self.drawSelector) {
                try await resolved(
                    gpu, state, samples: cell.samples, format: format.pixelFormat, bytesPerTexel: size
                )
            }
            let (passOff, passBeyond) = Self.offMetal(format, pass.bytes, stored)
            metrics.set("\(cell.name)_pass", "\(pass.path):\(passOff):\(passBeyond)")
            Self.noteBeyond(cell.name + "_pass", Self.beyondTexels(format, pass.bytes, stored), passBeyond, &metrics)
            passMs = String(format: "%.3f", pass.gpuMs)
        }
        metrics.set("\(cell.name)_ms", String(format: "%.3f", picked.gpuMs) + "/" + passMs)
        if route != (cell.twod ? "twod" : "pass") {
            wrong.append("\(cell.name)_not_\(cell.twod ? "twod" : "pass")")
        }
        if picked.path != "gm20b" { wrong.append("\(cell.name)_path") }
        if beyond > 0 { wrong.append("\(cell.name)_beyond_one") }
        if cell.exact && off > 0 { wrong.append("\(cell.name)_off_metal") }
        await engineArms(ArmScene(cell: cell, gpu: gpu, state: state, stored: stored), facade, &metrics, &wrong)
        guard cell.exact else { return }
        let read = try await readInCommandBuffer(
            gpu, state, samples: cell.samples, format: format.pixelFormat, bytesPerTexel: size
        )
        let (readOff, readBeyond) = Self.offMetal(format, read.bytes, stored)
        let readRoute = read.twod > 0 ? "twod" : "pass"
        metrics.set("\(cell.name)_read", "\(readRoute):\(readOff)")
        Self.noteBeyond(cell.name + "_read", Self.beyondTexels(format, read.bytes, stored), readBeyond, &metrics)
        if readRoute != "twod" { wrong.append("\(cell.name)_read_not_twod") }
        if readOff > 0 { wrong.append("\(cell.name)_read_off_metal") }
    }

    /// Sample s's codes at every pixel: resolve_twod_fragment's colour for s drawn into a one-sample target
    func storedSamples(_ gpu: GPU, _ format: X1Format, samples: Int) async throws -> [[UInt8]] {
        let state = try gpu.render(fragment: "resolve_twod_sample_fragment", format: format.pixelFormat)
        let row = Self.width * format.bytesPerTexel, total = row * Self.height
        var readbacks: [MTLBuffer] = []
        var committed: [MTLCommandBuffer] = []
        for s in 0 ..< samples {
            let target = try gpu.texture(format.pixelFormat, Self.width, Self.height)
            let out = try gpu.buffer(total)
            readbacks.append(out)
            committed.append(
                try gpu.commit { commands in
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = target
                    pass.colorAttachments[0].loadAction = .dontCare
                    pass.colorAttachments[0].storeAction = .store
                    let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                    e.setRenderPipelineState(state)
                    var corners = GPU.fullScreen, index = UInt32(s)
                    e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    e.setFragmentBytes(&index, length: 4, index: 0)
                    e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    e.endEncoding()
                    let blit = commands.makeBlitCommandEncoder()!
                    blit.copy(
                        from: target, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(),
                        sourceSize: MTLSize(width: Self.width, height: Self.height, depth: 1), to: out,
                        destinationOffset: 0, destinationBytesPerRow: row, destinationBytesPerImage: 0
                    )
                    blit.endEncoding()
                }
            )
        }
        try gpu.finish(committed)
        return readbacks.map { [UInt8](UnsafeRawBufferPointer(start: $0.contents(), count: total)) }
    }

    /// Of every third texel each way, those whose codes are not Metal's resolve of the stored samples, and channels
    /// more than one code from it
    static func offMetal(_ format: X1Format, _ got: [UInt8], _ stored: [[UInt8]]) -> (Int, Int) {
        let n = stored.count
        guard n > 0 else { return (0, 0) }
        let channels = format.channels
        let channelCount = channels.count
        let rows = (height + 2) / 3
        let chunks = min(max(1, ProcessInfo.processInfo.activeProcessorCount), max(1, rows))
        let srgbLUT: [Double] = channels.contains(.srgb8) ? (0 ..< 256).map { X1Channel.linear(Double($0) / 255) } : []
        var offCounts = [Int](repeating: 0, count: chunks)
        var beyondCounts = [Int](repeating: 0, count: chunks)
        let nU = UInt32(n), halfU = UInt32(n / 2), invN = 1.0 / Double(n)
        got.withUnsafeBufferPointer { gp in
            let gBase = gp.baseAddress!
            let s0 = stored[0], s1 = stored[min(1, n - 1)]
            let s2 = stored[min(2, n - 1)], s3 = stored[min(3, n - 1)]
            s0.withUnsafeBufferPointer { p0 in
                s1.withUnsafeBufferPointer { p1 in
                    s2.withUnsafeBufferPointer { p2 in
                        s3.withUnsafeBufferPointer { p3 in
                            let sp0 = p0.baseAddress!, sp1 = p1.baseAddress!
                            let sp2 = p2.baseAddress!, sp3 = p3.baseAddress!
                            offCounts.withUnsafeMutableBufferPointer { offBuf in
                                beyondCounts.withUnsafeMutableBufferPointer { beyBuf in
                                    DispatchQueue.concurrentPerform(iterations: chunks) { chunk in
                                        let r0 = chunk * rows / chunks
                                        let r1 = (chunk + 1) * rows / chunks
                                        var off = 0, beyond = 0
                                        for r in r0 ..< r1 {
                                            let y = r * 3
                                            let rowBase = y * width
                                            for x in stride(from: 0, to: width, by: 3) {
                                                let t = rowBase + x
                                                let codes = format.codes4(gBase, t)
                                                let c0 = format.codes4(sp0, t)
                                                let c1 = n > 1 ? format.codes4(sp1, t) : .zero
                                                let c2 = n > 2 ? format.codes4(sp2, t) : .zero
                                                let c3 = n > 3 ? format.codes4(sp3, t) : .zero
                                                var differs = false
                                                for c in 0 ..< channelCount {
                                                    let channel = channels[c]
                                                    let want: UInt32
                                                    switch channel {
                                                    case .unorm:
                                                        var sum = c0[c]
                                                        if n > 1 { sum += c1[c] }
                                                        if n > 2 { sum += c2[c] }
                                                        if n > 3 { sum += c3[c] }
                                                        want = (sum + halfU) / nU
                                                    case .srgb8:
                                                        var sum = srgbLUT[Int(c0[c])]
                                                        if n > 1 { sum += srgbLUT[Int(c1[c])] }
                                                        if n > 2 { sum += srgbLUT[Int(c2[c])] }
                                                        if n > 3 { sum += srgbLUT[Int(c3[c])] }
                                                        want = X1Rounding.nearest.code(
                                                            X1Channel.encoded(sum * invN), channel
                                                        )
                                                    case .float(10):
                                                        var sum = channel.value(c0[c])
                                                        if n > 1 { sum += channel.value(c1[c]) }
                                                        if n > 2 { sum += channel.value(c2[c]) }
                                                        if n > 3 { sum += channel.value(c3[c]) }
                                                        want = X1Rounding.nearest.code(sum * invN, channel)
                                                    default:
                                                        var sum = channel.value(c0[c])
                                                        if n > 1 { sum += channel.value(c1[c]) }
                                                        if n > 2 { sum += channel.value(c2[c]) }
                                                        if n > 3 { sum += channel.value(c3[c]) }
                                                        want = X1Rounding.tiesEven.code(sum * invN, channel)
                                                    }
                                                    if codes[c] != want {
                                                        differs = true
                                                        if abs(Int64(codes[c]) - Int64(want)) > 1 { beyond += 1 }
                                                    }
                                                }
                                                if differs { off += 1 }
                                            }
                                        }
                                        offBuf[chunk] = off
                                        beyBuf[chunk] = beyond
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        return (offCounts.reduce(0, +), beyondCounts.reduce(0, +))
    }

    static func metalResolve(_ format: X1Format, _ samples: [[UInt32]]) -> [UInt32] {
        let n = samples.count
        return format.channels.enumerated().map { c, channel in
            switch channel {
            case .unorm:
                let sum = samples.reduce(UInt32(0)) { $0 + $1[c] }
                return (sum + UInt32(n / 2)) / UInt32(n)
            case .srgb8:
                let linear = samples.map { X1Channel.linear(channel.value($0[c])) }.reduce(0, +) / Double(n)
                return X1Rounding.nearest.code(X1Channel.encoded(linear), channel)
            case .float(10):
                let mean = samples.map { channel.value($0[c]) }.reduce(0, +) / Double(n)
                return X1Rounding.nearest.code(mean, channel)
            default:
                let mean = samples.map { channel.value($0[c]) }.reduce(0, +) / Double(n)
                return X1Rounding.tiesEven.code(mean, channel)
            }
        }
    }
}
