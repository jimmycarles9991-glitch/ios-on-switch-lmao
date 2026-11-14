import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.raw.resolve_twod's format arms (raw_resolve_rule.h twodResolveFit): per format and sample count the codes the
/// format stores for each sample (one-sample draws of its colour, resolve_twod_sample_fragment), the resolve the rule
/// picks for a private target only passes sample, and the pack's pass where the format has a resolve state, each held
/// to Metal's rule computed from the stored codes (unorm nearest, sRGB averaged as linear light, floats ties to even)
/// on every third texel each way. `<cell>=<route>:<grade>:<texels off Metal>:<channels beyond one>`, `<cell>_pass=` the
/// same of the pass (the reference's control), `<cell>_ms=<picked>/<pass>` GPU ms. FAIL: a cell off resolve.twod, a
/// channel beyond one code, or an exact cell (RGBA16Float at 2x) off Metal, where its read arm
/// (`rgba16f_2x_read=<route>:<off>`) must take resolve.twod too; `<cell>_error`: the cell could not run.
extension ResolveTwodTest {
    struct FormatCell {
        let format: X1Format
        let samples: Int
        let exact: Bool
        let pass: Bool
        var name: String { "\(format.name)_\(samples)x" }
    }

    static var formatCells: [FormatCell] {
        X1Format.named(["bgra8srgb", "rgba8srgb", "rgba16f", "rgb10a2", "rg11b10f"]).flatMap { format in
            [2, 4].map { n in
                FormatCell(
                    format: format, samples: n, exact: format.name == "rgba16f" && n == 2,
                    pass: format.name != "rgb10a2")
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
            fragment: "resolve_twod_fragment", format: format.pixelFormat, samples: cell.samples)
        let stored = try await storedSamples(gpu, format, samples: cell.samples)
        let picked = try await resolved(
            gpu, state, samples: cell.samples, format: format.pixelFormat, bytesPerTexel: size)
        let (off, beyond) = Self.offMetal(format, picked.bytes, stored)
        let route = picked.twod > 0 ? "twod" : "pass"
        metrics.set(cell.name, "\(route):\(off == 0 ? "exact" : beyond == 0 ? "near" : "wrong"):\(off):\(beyond)")
        var passMs = "-"
        if cell.pass {
            let pass = try await switched(facade, Self.drawSelector) {
                try await resolved(
                    gpu, state, samples: cell.samples, format: format.pixelFormat, bytesPerTexel: size)
            }
            let (passOff, passBeyond) = Self.offMetal(format, pass.bytes, stored)
            metrics.set("\(cell.name)_pass", "\(pass.path):\(passOff):\(passBeyond)")
            passMs = String(format: "%.3f", pass.gpuMs)
        }
        metrics.set("\(cell.name)_ms", String(format: "%.3f", picked.gpuMs) + "/" + passMs)
        if route != "twod" { wrong.append("\(cell.name)_not_twod") }
        if picked.path != "raw" { wrong.append("\(cell.name)_path") }
        if beyond > 0 { wrong.append("\(cell.name)_beyond_one") }
        if cell.exact && off > 0 { wrong.append("\(cell.name)_off_metal") }
        guard cell.exact else { return }
        let read = try await readInCommandBuffer(
            gpu, state, samples: cell.samples, format: format.pixelFormat, bytesPerTexel: size)
        let (readOff, _) = Self.offMetal(format, read.bytes, stored)
        let readRoute = read.twod > 0 ? "twod" : "pass"
        metrics.set("\(cell.name)_read", "\(readRoute):\(readOff)")
        if readRoute != "twod" { wrong.append("\(cell.name)_read_not_twod") }
        if readOff > 0 { wrong.append("\(cell.name)_read_off_metal") }
    }

    /// Sample s's codes at every pixel: resolve_twod_fragment's colour for s drawn into a one-sample target
    func storedSamples(_ gpu: GPU, _ format: X1Format, samples: Int) async throws -> [[UInt8]] {
        let state = try gpu.render(fragment: "resolve_twod_sample_fragment", format: format.pixelFormat)
        var out: [[UInt8]] = []
        for s in 0..<samples {
            let target = try gpu.texture(format.pixelFormat, Self.width, Self.height)
            _ = try await gpu.run { commands in
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
            }
            out.append(try await gpu.read(target, bytesPerPixel: format.bytesPerTexel))
        }
        return out
    }

    /// Of every third texel each way, those whose codes are not Metal's resolve of the stored samples, and channels
    /// more than one code from it
    static func offMetal(_ format: X1Format, _ got: [UInt8], _ stored: [[UInt8]]) -> (Int, Int) {
        var off = 0, beyond = 0
        for y in stride(from: 0, to: height, by: 3) {
            for x in stride(from: 0, to: width, by: 3) {
                let t = y * width + x
                let codes = format.codes(got, t), want = metalResolve(format, stored.map { format.codes($0, t) })
                var differs = false
                for c in 0..<want.count where codes[c] != want[c] {
                    differs = true
                    if abs(Int64(codes[c]) - Int64(want[c])) > 1 { beyond += 1 }
                }
                if differs { off += 1 }
            }
        }
        return (off, beyond)
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
            default:
                let mean = samples.map { channel.value($0[c]) }.reduce(0, +) / Double(n)
                return X1Rounding.tiesEven.code(mean, channel)
            }
        }
    }
}
