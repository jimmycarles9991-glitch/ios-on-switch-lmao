import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A render pass into a view's slice: a 64x64 BGRA8Unorm 2D array of four slices and seven levels, every level of every
/// slice uploaded its own bytes, a 2D array view of its slices 1-2 at level 0, a pass at the view's slice 1 cleared to
/// red. Slice 2 of the array at level 0 is red and every other slice and level keeps its bytes, tolerance 0 (the
/// spec's; an M4 Max, this scene: 0 texels off, 0 bytes changed elsewhere). target_off texels of slice 2 level 0 not
/// red, changed_elsewhere bytes changed in any other slice or level, changed the parts they lie in (s<slice>l<level>,
/// "-" for none); every command buffer raw, failed_cbs=0.
struct ViewSliceTargetTest: HWTest {
    let id = "gpu.gm20b.view_slice_target"
    let name = "A pass into a slice of a 2D array view renders into that slice and level of the texture alone, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let side = 64, slices = 4, levels = 7
    static let red: [UInt8] = [0, 0, 255, 255]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        do {
            let parts = try await Self.drawn(gpu)
            var targetOff = 0
            var elsewhere = 0
            var changed: [String] = []
            for (part, bytes) in parts {
                if part.slice == 2 && part.level == 0 {
                    targetOff =
                        stride(from: 0, to: bytes.count, by: 4).filter { Array(bytes[$0 ..< $0 + 4]) != Self.red }
                            .count
                    continue
                }
                let off = zip(bytes, Self.uploaded(slice: part.slice, level: part.level)).filter { $0 != $1 }.count
                elsewhere += off
                if off > 0 { changed.append("s\(part.slice)l\(part.level)") }
            }
            metrics.set("target_off", targetOff)
            metrics.set("changed_elsewhere", elsewhere)
            metrics.set("changed", changed.isEmpty ? "-" : changed.joined(separator: ","))
            checks.append(("slice 2 level 0 red", targetOff == 0))
            checks.append(("every other slice and level kept", elsewhere == 0))
        } catch {
            checks.append(("the pass: \(GPU.describe(error))", false))
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func uploaded(slice: Int, level: Int) -> [UInt8] {
        let width = side >> level
        return (0 ..< width * width * 4).map { UInt8(truncatingIfNeeded: $0 &* 7 &+ slice &* 41 &+ level &* 97) }
    }

    static func drawn(_ gpu: GPU) async throws -> [(part: (slice: Int, level: Int), bytes: [UInt8])] {
        let d = MTLTextureDescriptor()
        d.textureType = .type2DArray
        d.pixelFormat = .bgra8Unorm
        (d.width, d.height, d.arrayLength, d.mipmapLevelCount) = (side, side, slices, levels)
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = .private
        guard let array = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no BGRA8 array") }
        for slice in 0 ..< slices {
            for level in 0 ..< levels {
                try await gpu.upload(
                    uploaded(slice: slice, level: level), to: array, bytesPerRow: (side >> level) * 4, level: level,
                    slice: slice
                )
            }
        }
        guard
            let view = array.makeTextureView(
                pixelFormat: .bgra8Unorm, textureType: .type2DArray, levels: 0 ..< 1, slices: 1 ..< 3
            )
        else { throw TestFailure("no view of slices 1-2") }
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = view
            pass.colorAttachments[0].slice = 1
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 1, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        var parts: [(part: (slice: Int, level: Int), bytes: [UInt8])] = []
        for slice in 0 ..< slices {
            for level in 0 ..< levels {
                parts.append(((slice, level), try await gpu.read(array, bytesPerPixel: 4, level: level, slice: slice)))
            }
        }
        return parts
    }
}
