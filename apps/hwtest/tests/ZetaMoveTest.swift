import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestShared
import Metal

/// A stored one-sample depth texture made plain for want of compressed memory moves onto it once some is made
/// (drivers/TegraGM20BMetal/private_comp.h compressedFromPlain, its zeta branch; comp.h
/// storedZetaKind), at the submission after the compression thread settled, its values with it. Per arm (Depth32Float,
/// Depth32Float_Stencil8 with stencil 0x5a, Depth16Unorm), a private 1376x784 texture, larger than any depth texture an
/// earlier test makes (kept compressed memory of a kind at least as large serves a texture at its creation:
/// CompQueue::fit), then three command buffers that clear it to 1 and draw 64x64-pixel cells at 0.25 + 0.0625 * ((cx +
/// cy) % 9) (LESS, writes), then a pass that loads it and draws red at 0.53125 (LESS) over a black BGRA8 target. Every
/// depth (Depth16Unorm: its nearest code, or the one below, StoredDepthTest's rule), stencil and colour texel is the
/// CPU's model of the scene, which an M4 Max matches (hwtests12 mac/gpu.gm20b.zeta_move). Judged: `<arm>_depth_wrong`,
/// `<arm>_stencil_wrong`, `<arm>_colour_wrong` 0, `<arm>_comptags_after` 1; where the texture was made plain
/// (`<arm>_comptags_at_creation` 0), memory made for it (`<arm>_made`) and its move (`<arm>_zeta_moves`) at least 1;
/// Depth32Float's 1.0 clears, a boot ZBC entry, a ZBC hit (`d32f_zbc_hits`), and its copy into a shared Depth32Float
/// read by getBytes (`d32f_shared_wrong` 0). Red: the move never ran (comptags_after 0), moved ZF32 bytes wrongly, or a
/// header still names the plain zeta (depth or colour wrong); a GPU fault: its plain span went back early.
struct ZetaMoveTest: HWTest {
    let id = "gpu.gm20b.zeta_move"
    let name = "A stored depth texture made plain moves onto compressed memory, its values with it"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    static let width = 1376, height = 784, cell = 64
    static let colourDepth: Float = 0.53125
    static let stencilClear: UInt8 = 0x5a

    struct Arm {
        let name: String
        let format: MTLPixelFormat
        let depthBytes: Int
        var stencil: Bool { format == .depth32Float_stencil8 }
    }

    static let arms: [Arm] = [
        Arm(name: "d32f", format: .depth32Float, depthBytes: 4),
        Arm(name: "d32fs8", format: .depth32Float_stencil8, depthBytes: 4),
        Arm(name: "d16", format: .depth16Unorm, depthBytes: 2),
    ]

    static func cellDepth(_ x: Int, _ y: Int) -> Float { 0.25 + 0.0625 * Float((x / cell + y / cell) % 9) }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        guard (gpu.device as? NSObject)?.responds(to: CompressionSettled.factsSelector) == true else {
            return .skip(GM20BFacade.missing("switchMetalProbeFacts:", on: gpu.device))
        }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for arm in Self.arms {
            do {
                try await check(gpu, arm, &metrics, &checks)
            } catch {
                metrics.set(arm.name + "_error", String(GPU.describe(error).prefix(80)))
                checks.append(("\(arm.name): \(GPU.describe(error))", false))
            }
        }
        return Outcome.judged(checks, metrics, proof: gpu.proof).onRawPath(gpu)
    }

    func check(_ gpu: GPU, _ arm: Arm, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]) async throws {
        let (w, h) = (Self.width, Self.height)
        let p = arm.name + "_"
        let movesBefore = GM20BFacade.counters(of: gpu.device)["private_compressed_moves"] ?? 0
        let madeBefore = GM20BFacade.compression(of: gpu.device)["made"] ?? 0
        let depth = try gpu.texture(arm.format, w, h, usage: [.renderTarget, .shaderRead])
        let atCreation = CompressionSettled.comptags(gpu.device, depth) ?? -1
        if atCreation == 0 { try await CompressionSettled.settle(gpu.device) }
        let made = (GM20BFacade.compression(of: gpu.device)["made"] ?? 0) - madeBefore
        let colour = try gpu.texture(.bgra8Unorm, w, h, usage: [.renderTarget, .pixelFormatView])
        let pipeline = try gpu.render(
            fragment: "solid_fragment", format: .bgra8Unorm, depth: arm.format,
            stencil: arm.stencil ? arm.format : .invalid
        )
        let zbcBefore = GM20BFacade.compression(of: gpu.device)["zbc_hits"] ?? 0
        for _ in 0 ..< 3 {
            try await gpu.run { commands in
                let encoder = commands.makeRenderCommandEncoder(descriptor: Self.pass(arm, colour, depth, load: false))!
                encoder.setRenderPipelineState(pipeline)
                encoder.setDepthStencilState(Self.less(gpu, writes: true))
                for cy in stride(from: 0, to: h, by: Self.cell) {
                    for cx in stride(from: 0, to: w, by: Self.cell) {
                        let corners = GPU.pixelQuad(
                            cx, cy, min(cx + Self.cell, w), min(cy + Self.cell, h), width: w, height: h,
                            z: Self.cellDepth(cx, cy)
                        )
                        GPU.draw(encoder, corners: corners, color: SIMD4(0, 0, 1, 1))
                    }
                }
                encoder.endEncoding()
            }
        }
        let zbcHits = (GM20BFacade.compression(of: gpu.device)["zbc_hits"] ?? 0) - zbcBefore
        let after = CompressionSettled.comptags(gpu.device, depth) ?? -1
        let moves = (GM20BFacade.counters(of: gpu.device)["private_compressed_moves"] ?? 0) - movesBefore
        try await gpu.run { commands in
            let encoder = commands.makeRenderCommandEncoder(descriptor: Self.pass(arm, colour, depth, load: true))!
            encoder.setRenderPipelineState(pipeline)
            encoder.setDepthStencilState(Self.less(gpu, writes: false))
            GPU.draw(
                encoder, corners: GPU.pixelQuad(0, 0, w, h, width: w, height: h, z: Self.colourDepth),
                color: SIMD4(1, 0, 0, 1)
            )
            encoder.endEncoding()
        }
        let depths = try await Self.read(
            gpu, depth, bytesPerTexel: arm.depthBytes, options: arm.stencil ? .depthFromDepthStencil : []
        )
        let (depthWrong, floorCodes) = Self.depthWrong(depths, arm)
        let pixels = try await gpu.read(colour, bytesPerPixel: 4)
        var colourWrong = 0
        for y in 0 ..< h {
            for x in 0 ..< w {
                let red: UInt8 = Self.cellDepth(x, y) > Self.colourDepth ? 255 : 0
                if Array(pixels.pixel(x, y, width: w)) != [0, 0, red, 255] { colourWrong += 1 }
            }
        }
        metrics.set(p + "comptags_at_creation", atCreation)
        metrics.set(p + "comptags_after", after)
        metrics.set(p + "made", made)
        metrics.set(p + "zeta_moves", moves)
        metrics.set(p + "depth_wrong", depthWrong)
        metrics.set(p + "colour_wrong", colourWrong)
        metrics.set(p + "zbc_hits", zbcHits)
        checks.append(("\(arm.name): \(depthWrong) depth texels off the scene", depthWrong == 0))
        checks.append(("\(arm.name): \(colourWrong) colour texels off the depth test's", colourWrong == 0))
        checks.append(("\(arm.name): on compressed memory after its work (comptags \(after))", after == 1))
        if atCreation == 0 {
            checks.append(("\(arm.name): compressed memory made for it (\(made))", made >= 1))
            checks.append(("\(arm.name): moved onto it (\(moves) moves)", moves >= 1))
        }
        if arm.format == .depth16Unorm { metrics.set(p + "floor_codes", floorCodes) }
        if arm.stencil {
            let stencils = try await Self.read(gpu, depth, bytesPerTexel: 1, options: .stencilFromDepthStencil)
            let stencilWrong = stencils.filter { $0 != Self.stencilClear }.count
            metrics.set(p + "stencil_wrong", stencilWrong)
            checks.append(("\(arm.name): \(stencilWrong) stencil texels not the clear", stencilWrong == 0))
        }
        if arm.format == .depth32Float {
            checks.append(("d32f: its 1.0 clears hit the boot ZBC entry (\(zbcHits))", zbcHits >= 1))
            let sharedWrong = try await Self.sharedCopyWrong(gpu, depth)
            metrics.set(p + "shared_wrong", sharedWrong)
            checks.append(("d32f: \(sharedWrong) texels off in its copy to a shared texture", sharedWrong == 0))
        }
    }

    static func less(_ gpu: GPU, writes: Bool) -> MTLDepthStencilState {
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .less
        d.isDepthWriteEnabled = writes
        return gpu.device.makeDepthStencilState(descriptor: d)!
    }

    /// The grid's pass (cleared to 1, stencil 0x5a, the colour neither cleared nor kept, so the ZBC hits are the
    /// depth's) or the red pass (both loaded, the colour cleared to black and kept). The colour target is of
    /// pixelFormatView usage, which keeps it off compressed memory: its own move would count in
    /// private_compressed_moves.
    static func pass(_ arm: Arm, _ colour: MTLTexture, _ depth: MTLTexture, load: Bool) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = colour
        pass.colorAttachments[0].loadAction = load ? .clear : .dontCare
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        pass.colorAttachments[0].storeAction = load ? .store : .dontCare
        pass.depthAttachment.texture = depth
        pass.depthAttachment.loadAction = load ? .load : .clear
        pass.depthAttachment.clearDepth = 1
        pass.depthAttachment.storeAction = .store
        if arm.stencil {
            pass.stencilAttachment.texture = depth
            pass.stencilAttachment.loadAction = load ? .load : .clear
            pass.stencilAttachment.clearStencil = UInt32(stencilClear)
            pass.stencilAttachment.storeAction = .store
        }
        return pass
    }

    /// Texels off the grid's depths, and of Depth16Unorm the texels at the code below the nearest one (not counted off)
    static func depthWrong(_ depths: [UInt8], _ arm: Arm) -> (Int, Int) {
        var wrong = 0, floors = 0
        for y in 0 ..< height {
            for x in 0 ..< width {
                let z = cellDepth(x, y), at = (y * width + x) * arm.depthBytes
                if arm.depthBytes == 2 {
                    let got = Int(depths[at]) | Int(depths[at + 1]) << 8, exact = Double(z) * 65535
                    if got == Int(exact.rounded(.down)) && got != Int(exact.rounded()) {
                        floors += 1
                    } else if got != Int(exact.rounded()) {
                        wrong += 1
                    }
                } else if depths.loaded(as: UInt32.self, at: y * width + x) != z.bitPattern {
                    wrong += 1
                }
            }
        }
        return (wrong, floors)
    }

    static func read(
        _ gpu: GPU, _ texture: MTLTexture, bytesPerTexel: Int, options: MTLBlitOption
    ) async throws -> [UInt8] {
        let row = width * bytesPerTexel
        let out = try gpu.buffer(row * height)
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: texture, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(),
                sourceSize: MTLSize(width: width, height: height, depth: 1), to: out, destinationOffset: 0,
                destinationBytesPerRow: row, destinationBytesPerImage: row * height, options: options
            )
            blit.endEncoding()
        }
        return Array(UnsafeRawBufferPointer(start: out.contents(), count: row * height))
    }

    /// The Depth32Float texture blitted into a shared one, whose getBytes is compared with the grid's depths
    static func sharedCopyWrong(_ gpu: GPU, _ depth: MTLTexture) async throws -> Int {
        let shared = try gpu.texture(.depth32Float, width, height, usage: [.shaderRead], storage: .shared)
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(from: depth, to: shared)
            blit.endEncoding()
        }
        var words = [UInt32](repeating: 0, count: width * height)
        words.withUnsafeMutableBytes {
            shared.getBytes(
                $0.baseAddress!, bytesPerRow: width * 4, from: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0
            )
        }
        var wrong = 0
        for y in 0 ..< height {
            for x in 0 ..< width where words[y * width + x] != cellDepth(x, y).bitPattern { wrong += 1 }
        }
        return wrong
    }
}
