import Darwin
import Foundation
import HWTestCore
import HWTestGPU

/// What the display shows, read back by the GPU through a debug kernel's TegraGM20B read-back
/// (drivers/TegraGM20B/TegraGM20BReadbackABI.h): for the next frame the display presents through the GPU, the
/// compositor's framebuffer as the display took it and the scanout buffer window A scans for it, as window A reads them
/// (B8G8R8A8). `nudge` makes the render server draw that frame. With latest-frame-wins flips a newer frame can replace
/// it before it is scanned; on a still screen that frame is the same picture. The display controller's stages after the
/// scanout buffer (colour pipeline, window B, the panel) are in neither image.
public enum ScanoutReadback {
    public struct Frame {
        public let framebuffer: Raster
        public let scanout: Raster
        /// Window A scans the framebuffer itself: both images are that one buffer.
        public let direct: Bool
        /// Rows whose bytes differ between the two buffers; the display's copy moves bytes, so 0 unless direct.
        public let copyRows: Int
        public let sequence: Int
    }

    /// A frame, or why there is none as a short code for the metrics.
    public enum Result {
        case frame(Frame)
        case unavailable(String)
    }

    public static let method: UInt32 = 0x5242
    public static let disarm: UInt32 = 0
    public static let arm: UInt32 = 1
    public static let query: UInt32 = 2
    public static let armed: Int32 = 1
    public static let inFlight: Int32 = 2
    public static let done: Int32 = 3

    public static func capture(nudge: @escaping @MainActor () -> Void) async -> Result {
        guard let gpu = GPUConnection() else { return .unavailable("no_connection") }
        let (kr, _) = readback(gpu, arm)
        guard kr == KERN_SUCCESS else {
            return .unavailable(
                kr == GPUConnection.kIOReturnUnsupported ? "release_kernel" : "arm_" + GPUConnection.hex(kr))
        }
        defer { _ = readback(gpu, disarm) }
        var reply: [UInt8] = []
        for _ in 0..<30 {
            await MainActor.run { nudge() }
            try? await Task.sleep(nanoseconds: 100_000_000)
            let (kr, answer) = readback(gpu, query)
            guard kr == KERN_SUCCESS else { return .unavailable("query_" + GPUConnection.hex(kr)) }
            reply = answer
            let state = GPUConnection.int32(reply, 0)
            if state != armed && state != inFlight { break }
        }
        switch GPUConnection.int32(reply, 0) {
        case done: break
        case armed, inFlight: return .unavailable("no_gpu_present")
        default: return .unavailable("failed_\(GPUConnection.int32(reply, 4))")
        }
        let (width, height) = (Int(GPUConnection.uint32(reply, 8)), Int(GPUConnection.uint32(reply, 12)))
        guard let framebuffer = gpu.map(GPUConnection.uint32(reply, 20), bytes: width * height * 4) else {
            return .unavailable("map_refused")
        }
        defer { gpu.unmap(framebuffer) }
        guard let scanout = gpu.map(GPUConnection.uint32(reply, 24), bytes: width * height * 4) else {
            return .unavailable("map_refused")
        }
        defer { gpu.unmap(scanout) }
        let rowBytes = width * 4
        let copyRows = (0..<height).filter { y in
            memcmp(framebuffer.base + y * rowBytes, scanout.base + y * rowBytes, rowBytes) != 0
        }.count
        return .frame(
            Frame(
                framebuffer: raster(framebuffer.base, width: width, height: height),
                scanout: raster(scanout.base, width: width, height: height),
                direct: GPUConnection.uint32(reply, 16) & 1 != 0, copyRows: copyRows,
                sequence: Int(GPUConnection.uint64(reply, 32))))
    }

    static func readback(_ gpu: GPUConnection, _ op: UInt32) -> (kern_return_t, [UInt8]) {
        gpu.call(method, GPUConnection.words([op, 0]), outputSize: 40)
    }

    static func raster(_ base: UnsafeRawPointer, width: Int, height: Int) -> Raster {
        let bytes = base.assumingMemoryBound(to: UInt8.self)
        var samples = [Float](repeating: 0, count: width * height * 3)
        for i in 0..<(width * height) {
            samples[i * 3] = Float(bytes[i * 4 + 2]) / 255
            samples[i * 3 + 1] = Float(bytes[i * 4 + 1]) / 255
            samples[i * 3 + 2] = Float(bytes[i * 4]) / 255
        }
        return Raster(width: width, height: height, channels: 3, samples: samples)
    }
}
