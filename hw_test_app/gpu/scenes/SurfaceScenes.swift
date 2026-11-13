import Foundation
import HWTestCore
import IOSurface
import Metal

/// IOSurfaces both ways round the kernel's layout decision (TegraGM20BLayout.h): one the GPU writes first, which it
/// makes block-linear and shows the CPU as rows through a copy-engine scratch at each lock; and one the CPU fills
/// first and the GPU only samples, which stays rows for good. Each takes several lock cycles of small writes at odd
/// offsets, each followed by GPU readbacks (blit and shader) of the whole surface: written texels must arrive and
/// every untouched texel must survive the scratch's write-back.
struct SurfaceLockScene: GPUScene {
    let id = "gpu.iosurface.cpu_lock"
    let name = "IOSurface CPU locks against GPU writes, tiled and rows"

    static let shapes: [(TexelFormat, Int, Int)] = [
        (.bgra8Unorm, 256, 256), (.r8Unorm, 512, 64), (.rgba16Float, 128, 128), (.bgra8Unorm, 333, 77),
        (.rgba16Float, 72, 72), (.rgba16Float, 136, 136),
    ]
    static let writes = [
        UploadRect(x: 37, y: 11, width: 12, height: 14), UploadRect(x: 63, y: 7, width: 3, height: 9),
        UploadRect(x: 1, y: 0, width: 70, height: 2),
    ]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        for (format, width, height) in Self.shapes {
            try await gpuFirst(gpu, format, width, height, &result)
            try await cpuFirst(gpu, format, width, height, &result)
        }
        return result
    }

    func gpuFirst(
        _ gpu: GPU, _ format: TexelFormat, _ width: Int, _ height: Int, _ result: inout SceneResult
    )
        async throws
    {
        let label = "\(format.rawValue) \(width)x\(height) gpu-first"
        let target = try SurfaceTarget.make(gpu, format, width, height, usage: [.renderTarget, .shaderRead])
        let pipeline = try gpu.render(fragment: "solid_fragment", format: format.metal)
        var model = TexelImage(
            format, width, height,
            Raster(width: width, height: height, channels: format.channels) {
                _, _, _ in 0
            })
        let rect = UploadRect(x: width / 4, y: height / 4, width: width / 2, height: height / 3)
        try gpu.finish([
            try gpu.clearAndFill(
                target.texture, clear: [0.2, 0.4, 0.6, 1], rect: rect, color: [1, 0.5, 0, 1], pipeline: pipeline)
        ])
        model.fill([0.2, 0.4, 0.6, 1])
        model.fill([1, 0.5, 0, 1], rect)
        try compareCPU(label + " lock after draw", target, model, &result)
        try await cycles(label, gpu, target, &model, &result)
        let late = UploadRect(x: 5, y: 3, width: 9, height: 5)
        let lateBytes = Patterns.bytes(Patterns.busy(width: 9, height: 5, channels: format.channels, seed: 9), format)
        try await gpu.blitUpload([late], [lateBytes], into: target.texture, texel: format.bytesPerTexel)
        model.write(late, lateBytes)
        try compareCPU(label + " lock after a GPU copy", target, model, &result)
    }

    func cpuFirst(
        _ gpu: GPU, _ format: TexelFormat, _ width: Int, _ height: Int, _ result: inout SceneResult
    )
        async throws
    {
        let label = "\(format.rawValue) \(width)x\(height) cpu-first"
        let target = try SurfaceTarget.make(gpu, format, width, height, usage: [.shaderRead])
        var model = TexelImage(
            format, width, height, Patterns.busy(width: width, height: height, channels: format.channels))
        try target.cpuWrite(UploadRect(x: 0, y: 0, width: width, height: height), model.bytes)
        try await result.compareBothWays(label + " sampled", target.texture, model, gpu: gpu)
        try await cycles(label, gpu, target, &model, &result)
    }

    func cycles(
        _ label: String, _ gpu: GPU, _ target: SurfaceTarget, _ model: inout TexelImage, _ result: inout SceneResult
    )
        async throws
    {
        let format = target.format
        for (cycle, rect) in Self.writes.enumerated() {
            let clipped = UploadRect(
                x: rect.x, y: rect.y, width: min(rect.width, target.width - rect.x),
                height: min(rect.height, target.height - rect.y))
            let bytes = Patterns.bytes(
                Patterns.busy(width: clipped.width, height: clipped.height, channels: format.channels, seed: cycle + 3),
                format)
            try target.cpuWrite(clipped, bytes)
            model.write(clipped, bytes)
            try await result.compareBothWays("\(label) cycle \(cycle) \(clipped.name)", target.texture, model, gpu: gpu)
        }
        try compareCPU(label + " lock after cycles", target, model, &result)
    }

    func compareCPU(_ label: String, _ target: SurfaceTarget, _ model: TexelImage, _ result: inout SceneResult) throws {
        result.compare(
            label, bytes: try target.cpuRead(), expected: model.bytes, format: target.format, width: target.width,
            height: target.height, tolerance: target.format.step() * 1.01)
    }
}
