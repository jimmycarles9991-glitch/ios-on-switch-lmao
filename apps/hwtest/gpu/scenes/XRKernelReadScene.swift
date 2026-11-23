import Foundation
import HWTestCore
import Metal

/// A kernel's read() and nearest sample of RGB10A8_2P_XR10 over a two-plane surface, through the kernel's own view of
/// the planes (the driver refused any view but its stand-in's: compute.image's TermTwoPlanesBare). The planes hold
/// Apple's planted words, some with their unread top bits set, and bytes (XRKernelTable.reads); each read and sample
/// must be Apple's value bit for bit, (c - 384) / 510 a colour code and the byte over 255, every command buffer on the
/// raw path.
struct XRKernelReadScene: GPUScene {
    let id = "gpu.gm20b.xr_kernel_read"
    let name = "Kernel reads and samples of an RGB10A8_2P_XR10 surface texture's two planes"

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let table = XRKernelTable.reads
        let cases = table.count / 6
        let planes = try XRKernelPlanes(gpu, texels: cases, usage: [.shaderRead])
        let texels = XRKernelPlanes.width * planes.height
        planes.plant { i in (table[6 * (i % cases)], UInt8(table[6 * (i % cases) + 1])) }
        guard let out = gpu.device.makeBuffer(length: texels * 32, options: .storageModeShared) else {
            throw TestFailure("makeBuffer returned nil")
        }
        memset(out.contents(), 0xab, out.length)
        let path = try await XRKernelPlanes.dispatch(
            gpu, try gpu.compute("xr_kernel_read"), planes, buffer: out, slots: [0, 1]
        )
        let got = out.contents().assumingMemoryBound(to: UInt32.self)
        var reads = 0, samples = 0
        var first: String?
        for i in 0 ..< texels {
            let want = (2 ..< 6).map { table[6 * (i % cases) + $0] }
            for (k, form) in ["read", "sample"].enumerated() {
                let lanes = (0 ..< 4).map { got[8 * i + 4 * k + $0] }
                guard lanes != want else { continue }
                if k == 0 { reads += 1 } else { samples += 1 }
                first =
                    first
                        ?? "\(form) \(i) of \(String(format: "%08x %02x", table[6 * (i % cases)], table[6 * (i % cases) + 1])): "
                        + lanes.map { String(format: "%08x", $0) }.joined(separator: " ")
            }
        }
        result.metrics.set("route", path)
        result.metrics.set("texels", texels)
        result.metrics.set("reads_wrong", reads)
        result.metrics.set("samples_wrong", samples)
        if let first { result.metrics.set("first", first) }
        result.expect(
            reads == 0 && samples == 0, "\(reads) reads and \(samples) samples are not Apple's: \(first ?? "")"
        )
        if GM20BFacade.kind(of: gpu.device) == "gm20b" {
            result.expect(path == "gm20b", "the reads took \(path), not raw")
        }
        return result
    }
}
