import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A pass into one plane of a one-level 3D texture laid out several planes a block: a shared 16x16x8 texture made with
/// ShaderRead usage alone, RGBA8Unorm (the spec's) or A1BGR5Unorm (a format the driver renders through a stand-in),
/// every byte 0x11. clear: a pass at depthPlane 5, Clear (1, 0, 0, 1), Store: plane 5 red, planes 0-4 and 6-7 keep
/// 0x11. drawn: a pass at depthPlane 5, Load, a draw of green over the left half: plane 5 green there and 0x11 on the
/// right, the other planes 0x11. Tolerance 0. An M4 Max renders into such textures (m2v-passes2 probe/nort.mm, the 3D
/// case; this scene on an M4 Max: 0 off in every case, red and green A1BGR5 words 0xf801 and 0x07c1). Per case
/// <case>_plane_off texels of plane 5 off, <case>_others_changed texels of the other planes changed; every command
/// buffer raw, failed_cbs=0.
struct PlaneInBlocksTest: HWTest {
    let id = "gpu.gm20b.plane_in_blocks"
    let name = "A pass into one plane of a 3D texture laid out several planes a block changes that plane alone, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    struct Format {
        let key: String
        let format: MTLPixelFormat
        let red: [UInt8]
        let green: [UInt8]

        var bytes: Int { red.count }
    }

    static let side = 16, depth = 8, plane = 5
    static let formats = [
        Format(key: "rgba8", format: .rgba8Unorm, red: [255, 0, 0, 255], green: [0, 255, 0, 255]),
        Format(key: "a1bgr5", format: .a1bgr5Unorm, red: [0x01, 0xf8], green: [0xc1, 0x07]),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for f in Self.formats {
            for drawn in [false, true] {
                let key = "\(f.key)_\(drawn ? "drawn" : "clear")"
                do {
                    let planes = try await Self.passed(gpu, f, drawn: drawn)
                    let judged = Self.judged(f, planes, drawn: drawn)
                    metrics.set(key + "_plane_off", judged.plane)
                    metrics.set(key + "_others_changed", judged.others)
                    checks.append(("\(key): plane 5 rendered, the others kept", judged.plane + judged.others == 0))
                } catch {
                    checks.append(("\(key): \(GPU.describe(error))", false))
                }
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func judged(_ f: Format, _ planes: [[UInt8]], drawn: Bool) -> (plane: Int, others: Int) {
        let filled = [UInt8](repeating: 0x11, count: f.bytes)
        var judged = (plane: 0, others: 0)
        for (z, bytes) in planes.enumerated() {
            for texel in 0 ..< side * side {
                let got = Array(bytes[texel * f.bytes ..< (texel + 1) * f.bytes])
                guard z == plane else {
                    judged.others += got == filled ? 0 : 1
                    continue
                }
                let want = drawn ? (texel % side < side / 2 ? f.green : filled) : f.red
                judged.plane += got == want ? 0 : 1
            }
        }
        return judged
    }

    static func passed(_ gpu: GPU, _ f: Format, drawn: Bool) async throws -> [[UInt8]] {
        let d = MTLTextureDescriptor()
        d.textureType = .type3D
        d.pixelFormat = f.format
        (d.width, d.height, d.depth) = (side, side, depth)
        d.usage = .shaderRead
        d.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no \(f.key) 3D texture") }
        let image = side * side * f.bytes
        let filled = try gpu.buffer([UInt8](repeating: 0x11, count: image * depth))
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: filled, sourceOffset: 0, sourceBytesPerRow: side * f.bytes, sourceBytesPerImage: image,
                sourceSize: MTLSize(width: side, height: side, depth: depth), to: texture, destinationSlice: 0,
                destinationLevel: 0, destinationOrigin: MTLOrigin()
            )
            blit.endEncoding()
        }
        let state = try gpu.render(fragment: "solid_fragment", format: f.format)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].depthPlane = plane
            pass.colorAttachments[0].loadAction = drawn ? .load : .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 1, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            if drawn {
                encoder.setRenderPipelineState(state)
                let left = GPU.pixelQuad(0, 0, side / 2, side, width: side, height: side)
                GPU.draw(encoder, corners: left, color: SIMD4(0, 1, 0, 1))
            }
            encoder.endEncoding()
        }
        var planes: [[UInt8]] = []
        for z in 0 ..< depth { planes.append(try await gpu.read(texture, bytesPerPixel: f.bytes, plane: z)) }
        return planes
    }
}
