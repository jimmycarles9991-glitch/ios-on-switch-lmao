import Foundation
import HWTestCore
import HWTestGPU
import Metal
import MetalPerformanceShaders

struct MPSBlurTest: HWTest {
    let id = "metal.mps.blur"
    let name = "MPS Gaussian blur"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard MPSSupportsMTLDevice(gpu.device) else { return .fail("MPSSupportsMTLDevice is false") }
        let size = 64
        // Mid grey with one white texel in the middle: the blur keeps flat grey flat and spreads the spike evenly.
        var bytes = [UInt8](repeating: 128, count: size * size * 4)
        let centre = (size / 2 * size + size / 2) * 4
        bytes.replaceSubrange(centre..<(centre + 4), with: [255, 255, 255, 255])
        let source = try gpu.texture(.rgba8Unorm, size, size, usage: [.shaderRead])
        let destination = try gpu.texture(.rgba8Unorm, size, size, usage: [.shaderRead, .shaderWrite])
        try await gpu.upload(bytes, to: source, bytesPerRow: size * 4)
        let blur = MPSImageGaussianBlur(device: gpu.device, sigma: 2)
        let timing = try await gpu.run { commands in
            blur.encode(commandBuffer: commands, sourceTexture: source, destinationTexture: destination)
        }
        let out = try await gpu.read(destination, bytesPerPixel: 4)
        let value = { (x: Int, y: Int) in Int(out.pixel(x, y, width: size).first!) }
        let peak = value(size / 2, size / 2)
        let flat = [value(4, 4), value(size - 5, 8), value(10, size - 6)]
        let symmetric =
            value(size / 2 - 2, size / 2) == value(size / 2 + 2, size / 2)
            && value(size / 2, size / 2 - 2) == value(size / 2, size / 2 + 2)
        var metrics = Metrics()
        metrics.set("peak", peak)
        metrics.set("gpu_ms", timing.gpuMilliseconds, digits: 3)
        return .judged(
            [
                ("flat areas stay 128", flat.allSatisfy { abs($0 - 128) <= 1 }),
                ("the spike spreads (128 < peak < 255)", peak > 128 && peak < 255),
                ("the kernel is symmetric", symmetric),
            ], metrics, proof: gpu.proof)
    }
}

struct MPSMatrixTest: HWTest {
    let id = "metal.mps.matmul"
    let name = "MPS matrix multiply"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard MPSSupportsMTLDevice(gpu.device) else { return .fail("MPSSupportsMTLDevice is false") }
        let n = 256
        // Small integers: every product and sum is exact in float32, whatever order the GPU adds them in.
        let a = (0..<(n * n)).map { Float($0 % 9 - 4) }
        let b = (0..<(n * n)).map { Float(($0 * 7) % 5 - 2) }
        let rowBytes = MPSMatrixDescriptor.rowBytes(forColumns: n, dataType: .float32)
        func matrix(_ values: [Float]?) throws -> MPSMatrix {
            let buffer = try gpu.buffer(rowBytes * n)
            if let values {
                for row in 0..<n {
                    values.withUnsafeBytes {
                        (buffer.contents() + row * rowBytes).copyMemory(
                            from: $0.baseAddress! + row * n * 4, byteCount: n * 4)
                    }
                }
            }
            return MPSMatrix(
                buffer: buffer,
                descriptor: MPSMatrixDescriptor(rows: n, columns: n, rowBytes: rowBytes, dataType: .float32))
        }
        let (left, right, product) = (try matrix(a), try matrix(b), try matrix(nil))
        let multiply = MPSMatrixMultiplication(
            device: gpu.device, transposeLeft: false, transposeRight: false, resultRows: n, resultColumns: n,
            interiorColumns: n, alpha: 1, beta: 0)
        let timing = try await gpu.run { commands in
            multiply.encode(commandBuffer: commands, leftMatrix: left, rightMatrix: right, resultMatrix: product)
        }
        var wrong = 0
        let result = product.data.contents()
        for row in 0..<n {
            for column in 0..<n {
                var expected: Float = 0
                for k in 0..<n { expected += a[row * n + k] * b[k * n + column] }
                if result.load(fromByteOffset: row * rowBytes + column * 4, as: Float.self) != expected { wrong += 1 }
            }
        }
        var metrics = Metrics()
        metrics.set("wrong", wrong)
        metrics.set("gpu_ms", timing.gpuMilliseconds, digits: 3)
        metrics.set("gflops", timing.gpuTimed ? 2 * Double(n * n * n) / timing.gpuMilliseconds / 1e6 : 0, digits: 1)
        return .judged([("every element exact", wrong == 0)], metrics, proof: gpu.proof)
    }
}
