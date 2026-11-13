import Foundation
import HWTestCore
import Metal

/// METALMAP d1 (F11) and d24: what new buffers hold, and textures over buffers at offsets and rows 16 mod 32.
public enum X1MemoryProbes {
    /// Metal's contract: a new MTLBuffer reads zero. Each size is made, checked, filled with 0xAB and freed 64 times,
    /// so the raw path's pool and write-combined spans are handed out again; pattern bytes are 0xAB found in a new one.
    /// Only a span handed out again tests the claim: with every buffer zero and none at an address seen before, the
    /// pages were fresh and the verdict is INCONCLUSIVE.
    public static func bufferZero(_ gpu: GPU) async throws -> ProbeFinding {
        var wrong: [String: Int] = [:]
        var metrics = Metrics()
        var reusedAll = 0
        let modes: [(String, MTLResourceOptions)] = [
            ("shared", .storageModeShared), ("wc", [.storageModeShared, .cpuCacheModeWriteCombined]),
        ]
        for (mode, options) in modes {
            var dirtyBuffers = 0
            var dirtyBytes = 0
            var patternBytes = 0
            var reused = 0
            var seen = Set<UInt>()
            for length in [4096, 65536, 1 << 20] {
                for _ in 0..<64 {
                    try autoreleasepool {
                        guard let buffer = gpu.device.makeBuffer(length: length, options: options) else {
                            throw TestFailure("makeBuffer(\(length)) returned nil")
                        }
                        if !seen.insert(UInt(bitPattern: buffer.contents())).inserted { reused += 1 }
                        let bytes = UnsafeBufferPointer(
                            start: buffer.contents().bindMemory(to: UInt8.self, capacity: length), count: length)
                        let dirty = bytes.reduce(0) { $0 + ($1 != 0 ? 1 : 0) }
                        if dirty > 0 {
                            dirtyBuffers += 1
                            dirtyBytes += dirty
                            patternBytes += bytes.reduce(0) { $0 + ($1 == 0xAB ? 1 : 0) }
                        }
                        memset(buffer.contents(), 0xAB, length)
                    }
                }
            }
            wrong[mode + "_dirty_buffers"] = dirtyBuffers
            metrics.set(mode + "_dirty_bytes", dirtyBytes)
            metrics.set(mode + "_pattern_bytes", patternBytes)
            metrics.set(mode + "_reused", reused)
            reusedAll += reused
        }
        let finding = ProbeFinding.counted(wrong, paths: [], metrics: metrics)
        guard finding.verdict == .pass, reusedAll == 0 else { return finding }
        return ProbeFinding(.inconclusive, finding.metrics, note: "no buffer came back at an address seen before")
    }

    /// RGBA8 textures over a buffer, sampled at each texel's centre: one at offset 48 with rows of 48 bytes (16 mod
    /// 32, which the raw path's texture header cannot address, LIMITS row), one at 64 with rows of 64 as the control.
    public static func linearAlign(_ gpu: GPU) async throws -> ProbeFinding {
        var wrong: [String: Int] = [:]
        var metrics = Metrics()
        var paths: [String] = []
        metrics.set("min_linear_alignment", gpu.device.minimumLinearTextureAlignment(for: .rgba8Unorm))
        let kernel = try gpu.compute("sample_texture")
        for (name, offset, width) in [("odd16", 48, 12), ("even32", 64, 16)] {
            let height = 8
            let rowBytes = width * 4
            let source = try gpu.buffer(offset + rowBytes * height)
            let bytes = source.contents().bindMemory(to: UInt8.self, capacity: offset + rowBytes * height)
            for i in 0..<(rowBytes * height) { bytes[offset + i] = UInt8((i * 37 + 11) & 255) }
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .rgba8Unorm, width: width, height: height, mipmapped: false)
            descriptor.usage = [.shaderRead]
            descriptor.storageMode = .shared
            guard let texture = source.makeTexture(descriptor: descriptor, offset: offset, bytesPerRow: rowBytes) else {
                metrics.set(name + "_made", false)
                continue
            }
            let out = try gpu.buffer(width * height * 16)
            paths.append(
                try await gpu.run { commands in
                    let encoder = commands.makeComputeCommandEncoder()!
                    encoder.setComputePipelineState(kernel)
                    encoder.setTexture(texture, index: 0)
                    encoder.setBuffer(out, offset: 0, index: 0)
                    encoder.dispatchThreads(
                        MTLSize(width: width, height: height, depth: 1),
                        threadsPerThreadgroup: MTLSize(width: 4, height: 4, depth: 1))
                    encoder.endEncoding()
                }.path)
            let texels = out.contents().bindMemory(to: Float.self, capacity: width * height * 4)
            var count = 0
            for i in 0..<(width * height * 4) where UInt8((texels[i] * 255).rounded()) != bytes[offset + i] {
                count += 1
            }
            wrong[name + "_wrong"] = count
        }
        return ProbeFinding.counted(wrong, paths: paths, metrics: metrics)
    }
}
