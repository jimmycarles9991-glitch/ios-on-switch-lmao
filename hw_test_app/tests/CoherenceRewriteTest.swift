import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// A texture the GPU wrote, rewritten by the CPU, then read back through the ROP by a blended draw. The GPU's L2 is not
/// coherent with the CPU: the draw sees the CPU's bytes only when the kernel dropped the L2's lines of that memory before
/// the job, which it does for a write it was told of. Four ways the CPU writes: replaceRegion of a Shared texture (a new
/// one each round, on the span the last one freed), a Shared buffer's texture, an IOSurface's lock, and the bench's fill,
/// which as a bare memcpy left about one bench cell in 20 reading the previous experiment's texels.
struct CoherenceRewriteTest: HWTest {
    let id = "gpu.coherence.cpu_rewrite"
    let name = "A GPU-written texture the CPU rewrites: the GPU reads the CPU's bytes, never its own older ones"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 300 }

    static let rounds = 1000
    static let side = 64
    static let rowBytes = side * 4
    /// coherence_fragment's (1, 2, 3, 4) / 255 in BGRA8's byte order
    static let added: [UInt8] = [3, 2, 1, 4]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        let blend = try gpu.render(vertex: "coherence_vertex", fragment: "coherence_fragment", format: .bgra8Unorm) {
            let colour = $0.colorAttachments[0]!
            colour.isBlendingEnabled = true
            colour.sourceRGBBlendFactor = .one
            colour.destinationRGBBlendFactor = .one
            colour.sourceAlphaBlendFactor = .one
            colour.destinationAlphaBlendFactor = .one
        }
        var tallies: [CoherenceTally] = []
        for arm in try Self.arms(gpu) {
            var tally = CoherenceTally(arm: arm.name)
            for round in 0..<Self.rounds {
                try await Self.round(gpu, blend, arm, round, &tally)
            }
            tallies.append(tally)
        }
        tallies.append(try CoherenceBench.run(rounds: Self.rounds))

        var metrics = Metrics()
        metrics.set("rounds", Self.rounds)
        var checks: [(String, Bool)] = []
        for tally in tallies {
            tally.report(into: &metrics)
            checks.append(("\(tally.arm): every round ran", tally.rounds == Self.rounds && tally.failure.isEmpty))
            checks.append(("\(tally.arm): the GPU read the CPU's bytes, none of its own older ones", tally.clean))
            checks.append(("\(tally.arm): the CPU read what the GPU wrote", tally.cpuStale == 0))
        }
        checks.append(("no command buffer failed", gpu.commandBufferErrors.isEmpty))
        return .judged(checks, metrics)
    }

    /// The GPU clears the texture to `gpuBytes(round)`, the CPU reads that and writes `cpuBytes(round)`, and the blended
    /// draw adds `added` to what it finds
    static func round(
        _ gpu: GPU, _ blend: MTLRenderPipelineState, _ arm: CoherenceArm, _ round: Int,
        _ tally: inout CoherenceTally
    ) async throws {
        let texture = try arm.texture()
        let written = gpuBytes(round)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.colorAttachments[0].clearColor = MTLClearColor(
                red: Double(written[2]) / 255, green: Double(written[1]) / 255, blue: Double(written[0]) / 255,
                alpha: Double(written[3]) / 255)
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        tally.cpuStale += CoherenceTally.differing(try arm.read(texture), from: written)
        let cpu = cpuBytes(round)
        try arm.write(texture, cpu)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(blend)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            encoder.endEncoding()
        }
        let older = written.enumerated().map { min(255, Int($0.element) + Int(added[$0.offset])) }
        tally.count(
            try arm.read(texture), cpu: cpu, added: { _, c in Int(added[c]) }, older: [{ _, c in older[c] }])
    }

    /// Far from every CPU byte (0...199), so a read of the GPU's own clear never passes for the CPU's
    static func gpuBytes(_ round: Int) -> [UInt8] {
        [UInt8(210 + round % 40), UInt8(215 + round % 35), UInt8(220 + round % 30), 240]
    }

    static func cpuBytes(_ round: Int) -> [UInt8] {
        (0..<(side * side * 4)).map { UInt8(($0 &* 7 &+ round &* 13) % 200) }
    }

    static func arms(_ gpu: GPU) throws -> [CoherenceArm] {
        let region = MTLRegionMake2D(0, 0, side, side)
        var shared: MTLTexture?
        let replaced = CoherenceArm(
            name: "shared",
            texture: {
                shared = nil
                shared = try gpu.texture(
                    .bgra8Unorm, side, side, usage: [.renderTarget, .shaderRead], storage: .shared)
                return shared!
            },
            write: { texture, bytes in
                texture.replace(region: region, mipmapLevel: 0, withBytes: bytes, bytesPerRow: rowBytes)
            },
            read: { texture in
                var out = [UInt8](repeating: 0, count: side * side * 4)
                texture.getBytes(&out, bytesPerRow: rowBytes, from: region, mipmapLevel: 0)
                return out
            })

        let buffer = try gpu.buffer(side * rowBytes)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: side, height: side, mipmapped: false)
        descriptor.usage = [.renderTarget, .shaderRead]
        descriptor.storageMode = .shared
        guard let bufferTexture = buffer.makeTexture(descriptor: descriptor, offset: 0, bytesPerRow: rowBytes) else {
            throw TestFailure("makeTexture(descriptor:offset:bytesPerRow:) returned nil")
        }
        let viaBuffer = CoherenceArm(
            name: "buffer", texture: { bufferTexture },
            write: { _, bytes in buffer.contents().copyMemory(from: bytes, byteCount: bytes.count) },
            read: { _ in [UInt8](UnsafeRawBufferPointer(start: buffer.contents(), count: side * rowBytes)) })

        let properties: [IOSurfacePropertyKey: Any] = [
            .width: side, .height: side, .bytesPerElement: 4, .pixelFormat: kCVPixelFormatType_32BGRA,
        ]
        guard let surface = IOSurface(properties: properties),
            let surfaceTexture = gpu.device.makeTexture(
                descriptor: descriptor, iosurface: unsafeBitCast(surface, to: IOSurfaceRef.self), plane: 0)
        else { throw TestFailure("no IOSurface-backed texture") }
        let viaSurface = CoherenceArm(
            name: "iosurface", texture: { surfaceTexture },
            write: { _, bytes in
                surface.lock(options: [], seed: nil)
                defer { surface.unlock(options: [], seed: nil) }
                for y in 0..<side {
                    (surface.baseAddress + y * surface.bytesPerRow).copyMemory(
                        from: Array(bytes[(y * rowBytes)..<((y + 1) * rowBytes)]), byteCount: rowBytes)
                }
            },
            read: { _ in
                surface.lock(options: .readOnly, seed: nil)
                defer { surface.unlock(options: .readOnly, seed: nil) }
                return (0..<side).flatMap {
                    [UInt8](
                        UnsafeRawBufferPointer(start: surface.baseAddress + $0 * surface.bytesPerRow, count: rowBytes))
                }
            })
        return [replaced, viaBuffer, viaSurface]
    }
}

struct CoherenceArm {
    let name: String
    let texture: () throws -> MTLTexture
    let write: (MTLTexture, [UInt8]) throws -> Void
    let read: (MTLTexture) throws -> [UInt8]
}
