import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.raw.compressed_targets' ZBC arms (drivers/TegraGM20BMetal/raw/runtime/raw_zbc.h zbcColorEntry, zbcDepthEntry): a
/// private one-sample target of each format (compressed memory once the facade has made some of its shape:
/// raw_private_comp.h, C32, C64 or C128, so a first texture asks and the arm's own comes after) cleared again and again
/// to a value the boot table lacks, read back through the copy engine; for Depth16, a memoryless depth and an
/// EQUAL-tested quad at the clear depth over a private BGRA8 target. The first round's clear writes texels (the facade
/// asks for the entry at the third), so every later round must read back the first round's bytes: an entry's L2 texel
/// is what the ROP writes for the value. `zbc_<arm>=<first round texels off the value>:<last round texels off the
/// first>:<ZBC hits>`; FAIL on either count, on no hit, or on an arm that could not run (`zbc_<arm>_error`).
enum CompressedZbcArms {
    static let width = 256, height = 128, rounds = 12

    struct Arm {
        let name: String
        let format: MTLPixelFormat
        let clear: [Double]
        let texel: [UInt8]  // the bytes the clear writes
    }

    static func linear(_ code: Double) -> Double { X1Channel.linear(code / 255) }

    static func le(_ v: UInt32, _ n: Int) -> [UInt8] { (0..<n).map { UInt8(truncatingIfNeeded: v >> (8 * $0)) } }

    static let srgb = [linear(51), linear(102), linear(204), 1]

    static let arms: [Arm] = [
        Arm(name: "bgra8srgb", format: .bgra8Unorm_srgb, clear: srgb, texel: [204, 102, 51, 255]),
        Arm(name: "rgba8srgb", format: .rgba8Unorm_srgb, clear: srgb, texel: [51, 102, 204, 255]),
        Arm(
            name: "rgb10a2", format: .rgb10a2Unorm, clear: [300.0 / 1023, 600.0 / 1023, 900.0 / 1023, 2.0 / 3],
            texel: le(300 | 600 << 10 | 900 << 20 | 2 << 30, 4)),
        Arm(
            name: "rgba16unorm", format: .rgba16Unorm,
            clear: [10000.0 / 65535, 20000.0 / 65535, 40000.0 / 65535, 1],
            texel: le(10000, 2) + le(20000, 2) + le(40000, 2) + le(65535, 2)),
        Arm(
            name: "rgba32f", format: .rgba32Float, clear: [0.3, -1.5, 7.25, 0.5],
            texel: [Float(0.3), -1.5, 7.25, 0.5].flatMap { le($0.bitPattern, 4) }),
    ]

    static let depth16 = Float(39321) / 65535

    static func run(_ gpu: GPU, _ metrics: inout Metrics, _ wrong: inout [String]) async {
        for arm in arms {
            await judged(arm.name, gpu, &metrics, &wrong) {
                let (first, last) = try await colourRounds(gpu, arm)
                let size = arm.texel.count
                var firstOff = 0, lastOff = 0
                for t in 0..<width * height {
                    if Array(first[t * size..<(t + 1) * size]) != arm.texel { firstOff += 1 }
                    if first[t * size..<(t + 1) * size] != last[t * size..<(t + 1) * size] { lastOff += 1 }
                }
                return (firstOff, lastOff)
            }
        }
        await judged("depth16", gpu, &metrics, &wrong) {
            let (first, last) = try await depthRounds(gpu)
            return (unwritten(first), unwritten(last))
        }
    }

    static func judged(
        _ name: String, _ gpu: GPU, _ metrics: inout Metrics, _ wrong: inout [String],
        _ body: () async throws -> (Int, Int)
    ) async {
        let before = RawFacade.compression(of: gpu.device)
        do {
            let (firstOff, lastOff) = try await body()
            let hits = (RawFacade.compression(of: gpu.device)["zbc_hits"] ?? 0) - (before["zbc_hits"] ?? 0)
            metrics.set("zbc_\(name)", "\(firstOff):\(lastOff):\(hits)")
            if firstOff > 0 { wrong.append("zbc_\(name)_first") }
            if lastOff > 0 { wrong.append("zbc_\(name)_held_texel") }
            if hits == 0 { wrong.append("zbc_\(name)_no_hit") }
        } catch {
            metrics.set("zbc_\(name)_error", String(describing: error).prefix(80).description)
            wrong.append("zbc_\(name)_error")
        }
    }

    /// The target's bytes after the first round and after the last
    static func colourRounds(_ gpu: GPU, _ arm: Arm) async throws -> ([UInt8], [UInt8]) {
        _ = try gpu.texture(arm.format, width, height)
        try await Task.sleep(nanoseconds: 200_000_000)
        let target = try gpu.texture(arm.format, width, height)
        var first: [UInt8] = []
        for round in 0..<rounds {
            _ = try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(
                    red: arm.clear[0], green: arm.clear[1], blue: arm.clear[2], alpha: arm.clear[3])
                pass.colorAttachments[0].storeAction = .store
                commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
            }
            if round == 0 { first = try await gpu.read(target, bytesPerPixel: arm.texel.count) }
            try await Task.sleep(nanoseconds: 20_000_000)
        }
        return (first, try await gpu.read(target, bytesPerPixel: arm.texel.count))
    }

    /// The BGRA8 target after the first round and after the last: white where the quad passed EQUAL at the clear depth
    static func depthRounds(_ gpu: GPU) async throws -> ([UInt8], [UInt8]) {
        let colour = try gpu.texture(.bgra8Unorm, width, height, usage: [.renderTarget, .shaderRead])
        let depth = try gpu.texture(.depth16Unorm, width, height, usage: [.renderTarget], storage: .memoryless)
        let state = try gpu.render(fragment: "exact_fragment", format: .bgra8Unorm, depth: .depth16Unorm)
        let equal = gpu.device.makeDepthStencilState(
            descriptor: {
                let d = MTLDepthStencilDescriptor()
                d.depthCompareFunction = .equal
                d.isDepthWriteEnabled = false
                return d
            }())!
        var first: [UInt8] = []
        for round in 0..<rounds {
            _ = try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = colour
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
                pass.colorAttachments[0].storeAction = .store
                pass.depthAttachment.texture = depth
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.clearDepth = Double(depth16)
                pass.depthAttachment.storeAction = .dontCare
                let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                e.setRenderPipelineState(state)
                e.setDepthStencilState(equal)
                GPU.draw(
                    e, corners: GPU.pixelQuad(0, 0, width, height, width: width, height: height, z: depth16),
                    color: SIMD4<Float>(1, 1, 1, 1))
                e.endEncoding()
            }
            if round == 0 { first = try await gpu.read(colour, bytesPerPixel: 4) }
            try await Task.sleep(nanoseconds: 20_000_000)
        }
        return (first, try await gpu.read(colour, bytesPerPixel: 4))
    }

    static func unwritten(_ bytes: [UInt8]) -> Int {
        stride(from: 0, to: bytes.count, by: 4).filter { bytes[$0..<$0 + 4] != [255, 255, 255, 255] }.count
    }
}
