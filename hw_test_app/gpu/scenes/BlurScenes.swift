import CoreImage
import Foundation
import HWTestCore
import Metal
import MetalPerformanceShaders

/// MPS's Gaussian blur, edges clamped, against a Gaussian worked in double on the CPU, at small, medium and large
/// sigma. MPS runs compute kernels, which the raw facade sends to m2v.
struct MPSBlurScene: GPUScene {
    let id = "gpu.blur.mps"
    let name = "MPS Gaussian blur against a CPU Gaussian"

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        guard MPSSupportsMTLDevice(gpu.device) else {
            result.expect(false, "MPSSupportsMTLDevice is false")
            return result
        }
        let size = 128
        let bytes = Patterns.bytes(Patterns.busy(width: size, height: size, channels: 4), .rgba8Unorm)
        let image = TexelFormat.rgba8Unorm.raster(bytes, width: size, height: size)
        let source = try gpu.texture(.rgba8Unorm, size, size, usage: [.shaderRead])
        try await gpu.blitUpload([UploadRect(x: 0, y: 0, width: size, height: size)], [bytes], into: source, texel: 4)
        for sigma in [1.5, 4, 10] as [Float] {
            let destination = try gpu.texture(.rgba8Unorm, size, size, usage: [.shaderRead, .shaderWrite])
            let blur = MPSImageGaussianBlur(device: gpu.device, sigma: sigma)
            blur.edgeMode = .clamp
            let timing = try await gpu.run { commands in
                blur.encode(commandBuffer: commands, sourceTexture: source, destinationTexture: destination)
            }
            result.metrics.set("gpu_ms_sigma\(Int(sigma))", timing.gpuMilliseconds, digits: 3)
            let want = TexelFormat.rgba8Unorm.quantized(Gaussian.blur(image, sigma: Double(sigma)))
            result.compare(
                "sigma \(sigma)", try await gpu.raster(destination, .rgba8Unorm), want, tolerance: BlurTolerance.mps)
        }
        return result
    }
}

/// Core Image's CIGaussianBlur (its radius is the standard deviation) on a Metal-backed context with colour
/// management off, edges clamped and cropped back, against the same CPU Gaussian. The picture is opaque: Core Image
/// takes RGBA8 as premultiplied and clamps colour to alpha. Large radii go through Core
/// Image's downsampled pyramid, so they get a wider tolerance.
struct CoreImageBlurScene: GPUScene {
    let id = "gpu.blur.coreimage"
    let name = "Core Image Gaussian blur against a CPU Gaussian"

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let size = 128
        let bytes = Patterns.bytes(Patterns.opaque(Patterns.busy(width: size, height: size, channels: 4)), .rgba8Unorm)
        let image = TexelFormat.rgba8Unorm.raster(bytes, width: size, height: size)
        let context = CIContext(
            mtlCommandQueue: gpu.queue,
            options: [.workingColorSpace: NSNull(), .outputColorSpace: NSNull(), .cacheIntermediates: false])
        let input = CIImage(
            bitmapData: Data(bytes), bytesPerRow: size * 4, size: CGSize(width: size, height: size), format: .RGBA8,
            colorSpace: nil)
        for (radius, tolerance) in [
            (2.0, BlurTolerance.coreImage), (6, BlurTolerance.coreImage), (16, BlurTolerance.coreImageLarge),
        ] {
            let blurred = input.clampedToExtent().applyingGaussianBlur(sigma: radius).cropped(to: input.extent)
            var out = [UInt8](repeating: 0, count: size * size * 4)
            let start = CFAbsoluteTimeGetCurrent()
            context.render(
                blurred, toBitmap: &out, rowBytes: size * 4, bounds: input.extent, format: .RGBA8, colorSpace: nil)
            result.metrics.set("ms_radius\(Int(radius))", (CFAbsoluteTimeGetCurrent() - start) * 1000, digits: 2)
            let want = TexelFormat.rgba8Unorm.quantized(Gaussian.blur(image, sigma: radius))
            result.compare(
                "radius \(radius)", TexelFormat.rgba8Unorm.raster(out, width: size, height: size), want,
                tolerance: tolerance)
        }
        return result
    }
}

/// Apple's GPUs (the host test) stray one level from the double-precision Gaussian at every sigma; one more level is
/// allowed for another GPU's rounding, and Core Image's downsampled large radii a third.
enum BlurTolerance {
    static let mps: Float = 2.01 / 255
    static let coreImage: Float = 2.01 / 255
    static let coreImageLarge: Float = 3.01 / 255
}
