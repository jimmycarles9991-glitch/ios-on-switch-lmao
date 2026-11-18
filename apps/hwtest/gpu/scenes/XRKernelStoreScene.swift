import Foundation
import HWTestCore
import Metal

/// A kernel's stores into RGB10A8_2P_XR10 over a two-plane surface, which Apple's Metal makes with ShaderWrite (the
/// raw path refused it until the kernel record split the store across the planes: KernelTexturePlane). The probe's
/// float4(0.25, 0.5, 1.25, 0.75) and Apple's own stores of float4 and half4 values (XRKernelTable: NaNs, infinities,
/// past ±1.5, the codes' ties, alpha outside [0, 1]): plane 0 must hold Apple's word and plane 1 its byte for each, every
/// command buffer on the raw path.
struct XRKernelStoreScene: GPUScene {
    let id = "gpu.raw.xr_kernel_store"
    let name = "Kernel stores into an RGB10A8_2P_XR10 surface texture's two planes"

    /// The probe's case (xrprobe, an Apple M4 Max): its float bits, word and byte
    static let probe: [UInt32] = [0x3e80_0000, 0x3f00_0000, 0x3fa0_0000, 0x3f40_0000, 0x2009_fffe, 0xbf]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        var paths: [String] = []
        var wrong: [String] = []
        var rows = 0
        for (kernel, table, half) in [
            ("xr_kernel_store", Self.probe + XRKernelTable.floatStores, false),
            ("xr_kernel_store_half", XRKernelTable.halfStores, true),
        ] {
            let cases = table.count / 6
            let texels = (cases + XRKernelPlanes.width - 1) / XRKernelPlanes.width * XRKernelPlanes.width
            let planes = try XRKernelPlanes(gpu, texels: texels, usage: [.shaderWrite])
            planes.plant { _ in (0x5a5a_5a5a, 0x5a) }
            let lanes = (0..<texels).flatMap { i in (0..<4).map { table[6 * (i % cases) + $0] } }
            let input =
                half
                ? lanes.map { UInt16(truncatingIfNeeded: $0) }.withUnsafeBytes { Data($0) }
                : lanes.withUnsafeBytes { Data($0) }
            guard
                let buffer = input.withUnsafeBytes({
                    gpu.device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared)
                })
            else { throw TestFailure("makeBuffer returned nil") }
            paths.append(
                try await XRKernelPlanes.dispatch(gpu, try gpu.compute(kernel), planes, buffer: buffer, slots: [0]))
            for i in 0..<texels {
                let row = 6 * (i % cases)
                let got = planes.texel(i)
                if got.word != table[row + 4] || UInt32(got.byte) != table[row + 5] {
                    wrong.append(
                        String(
                            format: "%@ %d: %08x %02x not %08x %02x", kernel, i, got.word, got.byte, table[row + 4],
                            table[row + 5]))
                }
            }
            rows += cases
        }
        result.metrics.set("route", paths.joined(separator: ","))
        result.metrics.set("rows", rows)
        result.metrics.set("mismatches", wrong.count)
        if let first = wrong.first { result.metrics.set("first", first) }
        result.expect(
            wrong.isEmpty, "\(wrong.count) texels hold another word or byte than Apple's: \(wrong.first ?? "")")
        if RawFacade.kind(of: gpu.device) == "raw" {
            result.expect(paths.allSatisfy { $0 == "raw" }, "the stores took \(paths), not raw")
        }
        return result
    }
}
