import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// Passes into textures made with ShaderRead usage alone (Metal's validation error, which a release process does not
/// run): a 16x16 RGBA8Unorm 2D texture and a 16x16x4 RGBA8Unorm 3D texture, shared, each plane filled with its own byte
/// (0x11, 0x22, 0x33, 0x44), a pass cleared red into the 2D one and into plane 2 of the 3D one. An M4 Max renders into
/// them (m2v-passes2/probe/nort.mm, and this scene): the 2D texture red, plane 2 red, planes 0, 1 and 3 as filled, both
/// command buffers completed; tolerance 0. flat_off texels of the 2D one not red, plane_off texels of plane 2 not red,
/// others_changed texels of planes 0, 1 and 3 changed, errors the command buffers' errors ("-" for none); every command
/// buffer raw, failed_cbs=0.
struct NoRenderUsageTest: HWTest {
    let id = "gpu.gm20b.norender_usage"
    let name = "Passes render into textures made without RenderTarget usage, as Apple's GPU does, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let side = 16, planes = 4
    static let red: [UInt8] = [255, 0, 0, 255]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        var errors: [String] = []
        do {
            let flat = try await Self.cleared(gpu, depth: 1, &errors)
            let off = Self.notRed(flat[0])
            metrics.set("flat_off", off)
            checks.append(("the 2D texture red", off == 0))
        } catch {
            checks.append(("2D: \(GPU.describe(error))", false))
        }
        do {
            let volume = try await Self.cleared(gpu, depth: Self.planes, &errors)
            let off = Self.notRed(volume[2])
            let others = [0, 1, 3].map { plane in
                stride(from: 0, to: volume[plane].count, by: 4).filter { at in
                    volume[plane][at ..< at + 4].contains { $0 != Self.fill(plane) }
                }.count
            }.reduce(0, +)
            metrics.set("plane_off", off)
            metrics.set("others_changed", others)
            checks.append(("plane 2 of the 3D texture red", off == 0))
            checks.append(("planes 0, 1 and 3 kept", others == 0))
        } catch {
            checks.append(("3D: \(GPU.describe(error))", false))
        }
        metrics.set("errors", errors.isEmpty ? "-" : String(errors.joined(separator: ";").prefix(160)))
        checks.append(("both command buffers completed", errors.isEmpty))
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func fill(_ plane: Int) -> UInt8 { UInt8(0x11 * (plane + 1)) }

    static func notRed(_ bytes: [UInt8]) -> Int {
        stride(from: 0, to: bytes.count, by: 4).filter { Array(bytes[$0 ..< $0 + 4]) != red }.count
    }

    static func cleared(_ gpu: GPU, depth: Int, _ errors: inout [String]) async throws -> [[UInt8]] {
        let d = MTLTextureDescriptor()
        d.textureType = depth > 1 ? .type3D : .type2D
        d.pixelFormat = .rgba8Unorm
        (d.width, d.height, d.depth) = (side, side, depth)
        d.usage = .shaderRead
        d.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: d) else {
            throw TestFailure("no texture of depth \(depth)")
        }
        let filled = try gpu.buffer((0 ..< depth).flatMap { [UInt8](repeating: fill($0), count: side * side * 4) })
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: filled, sourceOffset: 0, sourceBytesPerRow: side * 4, sourceBytesPerImage: side * side * 4,
                sourceSize: MTLSize(width: side, height: side, depth: depth), to: texture, destinationSlice: 0,
                destinationLevel: 0, destinationOrigin: MTLOrigin()
            )
            blit.endEncoding()
        }
        let path = try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].depthPlane = depth > 1 ? 2 : 0
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 1, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }.path
        if path.hasPrefix("error:") { errors.append(String(path.dropFirst("error:".count))) }
        var planes: [[UInt8]] = []
        for plane in 0 ..< depth { planes.append(try await gpu.read(texture, bytesPerPixel: 4, plane: plane)) }
        return planes
    }
}
