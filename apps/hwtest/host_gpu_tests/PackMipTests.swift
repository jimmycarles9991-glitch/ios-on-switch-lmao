import Foundation
import HWTestCore
import HWTestGPU
import Metal
import XCTest

/// The raw pack's mip states (drivers/TegraGM20BMetal/raw/pack/mip.metal) on this Mac's GPU: every step of every chain
/// format, odd steps included, within one code of Metal's step (MipReference), each level drawn from the one above as
/// the runtime's mipPasses draws it (buffer 0: the two levels' sizes; an sRGB level read as its codes). A 32-bit float field may be two ulps off: the
/// state blends in float, rounding twice where MipReference rounds once.
final class PackMipTests: XCTestCase {
    private func library(_ device: MTLDevice) throws -> MTLLibrary {
        let root = ProcessInfo.processInfo.environment["TEST_SRCDIR"].map { $0 + "/_main/" } ?? ""
        let source = try String(
            contentsOfFile: root + "drivers/TegraGM20BMetal/raw/pack/mip.metal", encoding: .utf8)
        return try device.makeLibrary(source: source, options: MTLCompileOptions())
    }

    private static func pattern(_ w: Int, _ h: Int, _ layout: MipLayout) -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: w * h * layout.bytes)
        for t in 0..<w * h {
            for (i, f) in layout.fields.enumerated() {
                var v = UInt32(truncatingIfNeeded: t &* 73_856_093) ^ UInt32(truncatingIfNeeded: (i + 1) &* 83_492_791)
                v ^= v >> 13
                v = v &* 0x5bd1_e995
                v ^= v >> 15
                let code = f.kind == .float ? layout.code(of: Double(v & 0xffff) / 65535, f) : v & f.mask
                layout.put(code, &bytes, texel: t, field: f)
            }
        }
        return bytes
    }

    /// What the runtime binds an sRGB level as: its codes (Device::levelTic's codes header)
    private static func codes(_ format: MTLPixelFormat) -> MTLPixelFormat {
        switch format {
        case .bgra8Unorm_srgb: return .bgra8Unorm
        case .rgba8Unorm_srgb: return .rgba8Unorm
        default: return format
        }
    }

    func testEveryStepIsMetalsStep() async throws {
        let device = try XCTUnwrap(MTLCreateSystemDefaultDevice())
        let queue = try XCTUnwrap(device.makeCommandQueue())
        let gpu = GPU(device: device, queue: queue, library: try library(device))
        for format in MipFormats.twod + MipFormats.candidates {
            let name = format.name.replacingOccurrences(of: "srgb", with: "_srgb")
            let state = try gpu.render(vertex: "mip_vert", fragment: "mip_\(name)_frag", format: format.pixel)
            var tally = MipTally()
            for (w, h) in [(255, 129), (776, 24), (64, 64)] {
                var dims = [(w, h)]
                while dims.last! != (1, 1) { dims.append((max(1, dims.last!.0 / 2), max(1, dims.last!.1 / 2))) }
                var level = Self.pattern(w, h, format.layout)
                for l in 1..<dims.count {
                    let (sw, sh) = dims[l - 1], (dw, dh) = dims[l]
                    let above = try gpu.texture(
                        Self.codes(format.pixel), sw, sh, usage: [.shaderRead], storage: .shared)
                    above.replace(
                        region: MTLRegionMake2D(0, 0, sw, sh), mipmapLevel: 0, withBytes: level,
                        bytesPerRow: sw * format.layout.bytes)
                    let target = try gpu.texture(format.pixel, dw, dh, usage: [.renderTarget, .shaderRead])
                    try await gpu.run { commands in
                        let pass = MTLRenderPassDescriptor()
                        pass.colorAttachments[0].texture = target
                        pass.colorAttachments[0].loadAction = .dontCare
                        pass.colorAttachments[0].storeAction = .store
                        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                        encoder.setRenderPipelineState(state)
                        encoder.setFragmentTexture(above, index: 0)
                        var step: [UInt32] = [UInt32(sw), UInt32(sh), UInt32(dw), UInt32(dh)]
                        encoder.setFragmentBytes(&step, length: 16, index: 0)
                        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                        encoder.endEncoding()
                    }
                    let drawn = try await gpu.read(target, bytesPerPixel: format.layout.bytes)
                    tally.add(
                        drawn, MipReference.step(level, sw, sh, dw, dh, format.layout), texels: dw * dh, format.layout)
                    level = drawn
                }
            }
            print("pack mip \(format.name) exact=\(tally.exact) within_one=\(tally.withinOne) wrong=\(tally.wrong)")
            let float32 = format.layout.fields.contains { $0.kind == .float && $0.width == 32 }
            XCTAssertLessThanOrEqual(tally.maxDiff, float32 ? 2 : 1, format.name)
        }
    }
}
