import Darwin
import Foundation
import HWTestCore

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

    static let method: UInt32 = 0x5242
    static let disarm: UInt32 = 0
    static let arm: UInt32 = 1
    static let query: UInt32 = 2
    static let armed: Int32 = 1
    static let inFlight: Int32 = 2
    static let done: Int32 = 3
    static let kIOReturnUnsupported = kern_return_t(bitPattern: 0xe000_02c7)

    public static func capture(nudge: @escaping @MainActor () -> Void) async -> Result {
        guard let gpu = GPUConnection() else { return .unavailable("no_connection") }
        let (kr, _) = gpu.readback(arm)
        guard kr == KERN_SUCCESS else {
            return .unavailable(kr == kIOReturnUnsupported ? "release_kernel" : "arm_" + hex(kr))
        }
        defer { _ = gpu.readback(disarm) }
        var reply: [UInt8] = []
        for _ in 0..<30 {
            await MainActor.run { nudge() }
            try? await Task.sleep(nanoseconds: 100_000_000)
            let (kr, answer) = gpu.readback(query)
            guard kr == KERN_SUCCESS else { return .unavailable("query_" + hex(kr)) }
            reply = answer
            let state = int32(reply, 0)
            if state != armed && state != inFlight { break }
        }
        switch int32(reply, 0) {
        case done: break
        case armed, inFlight: return .unavailable("no_gpu_present")
        default: return .unavailable("failed_\(int32(reply, 4))")
        }
        let (width, height) = (Int(uint32(reply, 8)), Int(uint32(reply, 12)))
        guard let framebuffer = gpu.map(uint32(reply, 20), bytes: width * height * 4) else {
            return .unavailable("map_refused")
        }
        defer { gpu.unmap(framebuffer) }
        guard let scanout = gpu.map(uint32(reply, 24), bytes: width * height * 4) else {
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
                direct: uint32(reply, 16) & 1 != 0, copyRows: copyRows, sequence: Int(uint64(reply, 32))))
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

    static func hex(_ kr: kern_return_t) -> String { "0x" + String(UInt32(bitPattern: kr), radix: 16) }
    static func int32(_ b: [UInt8], _ at: Int) -> Int32 { Int32(bitPattern: uint32(b, at)) }
    static func uint32(_ b: [UInt8], _ at: Int) -> UInt32 {
        b.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: at, as: UInt32.self) }
    }
    static func uint64(_ b: [UInt8], _ at: Int) -> UInt64 {
        b.withUnsafeBytes { $0.loadUnaligned(fromByteOffset: at, as: UInt64.self) }
    }
}

/// A TegraGM20B connection of the test's own (drivers/TegraGM20B/TegraGM20BABI.h's wire form): IOKit through dlsym,
/// which the iOS SDK has no headers for.
private final class GPUConnection {
    struct Mapping {
        let token: UInt32
        let address: UInt64
        var base: UnsafeRawPointer { UnsafeRawPointer(bitPattern: UInt(address))! }
    }

    typealias Matching = @convention(c) (UnsafePointer<CChar>) -> UnsafeMutableRawPointer?
    typealias Service = @convention(c) (mach_port_t, UnsafeMutableRawPointer?) -> mach_port_t
    typealias Open =
        @convention(c) (mach_port_t, mach_port_t, UInt32, UnsafeMutablePointer<mach_port_t>) ->
        kern_return_t
    typealias Release = @convention(c) (mach_port_t) -> kern_return_t
    typealias Call =
        @convention(c) (
            mach_port_t, UInt32, UnsafeRawPointer?, Int, UnsafeMutableRawPointer?, UnsafeMutablePointer<Int>?
        ) -> kern_return_t

    static let openType: UInt32 = 0x4e56
    static let tunnelMagic: UInt32 = 0x4e56_544e
    static let tunnelProbe: UInt32 = 0xffff_ffff
    static let mapMethod: UInt32 = 12
    static let unmapMethod: UInt32 = 13
    static let mapReadOnly: UInt32 = 1

    let connection: mach_port_t
    let callStruct: Call
    let close: Release
    /// The first selector the sandbox admits, which carries every method with the tunnel's prefix; nil: none answered
    /// the probe, and each method goes by its own selector.
    var tunnel: UInt32?

    init?() {
        guard let iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW),
            let matching = dlsym(iokit, "IOServiceMatching"), let service = dlsym(iokit, "IOServiceGetMatchingService"),
            let open = dlsym(iokit, "IOServiceOpen"), let release = dlsym(iokit, "IOObjectRelease"),
            let close = dlsym(iokit, "IOServiceClose"), let call = dlsym(iokit, "IOConnectCallStructMethod")
        else { return nil }
        let accelerator = unsafeBitCast(service, to: Service.self)(
            0, unsafeBitCast(matching, to: Matching.self)("IOAcceleratorES"))
        guard accelerator != 0 else { return nil }
        var connection: mach_port_t = 0
        let kr = unsafeBitCast(open, to: Open.self)(accelerator, mach_task_self_, Self.openType, &connection)
        _ = unsafeBitCast(release, to: Release.self)(accelerator)
        guard kr == KERN_SUCCESS else { return nil }
        self.connection = connection
        callStruct = unsafeBitCast(call, to: Call.self)
        self.close = unsafeBitCast(close, to: Release.self)
        let probe = words([Self.tunnelMagic, Self.tunnelProbe])
        tunnel = (0..<256).map(UInt32.init).first { send($0, probe, 0).0 == KERN_SUCCESS }
    }

    deinit { _ = close(connection) }

    func readback(_ op: UInt32) -> (kern_return_t, [UInt8]) {
        call(ScanoutReadback.method, words([op, 0]), outputSize: 40)
    }

    func map(_ token: UInt32, bytes: Int) -> Mapping? {
        let (kr, reply) = call(Self.mapMethod, words([token, Self.mapReadOnly, 0, 0]), outputSize: 16)
        guard kr == KERN_SUCCESS else { return nil }
        let mapping = Mapping(token: token, address: ScanoutReadback.uint64(reply, 0))
        guard Int(ScanoutReadback.uint64(reply, 8)) >= bytes else {
            unmap(mapping)
            return nil
        }
        return mapping
    }

    func unmap(_ mapping: Mapping) {
        let address = mapping.address
        _ = call(
            Self.unmapMethod, words([mapping.token, 0, UInt32(address & 0xffff_ffff), UInt32(address >> 32)]),
            outputSize: 0)
    }

    func call(_ method: UInt32, _ input: [UInt8], outputSize: Int) -> (kern_return_t, [UInt8]) {
        guard let tunnel else { return send(method, input, outputSize) }
        return send(tunnel, words([Self.tunnelMagic, method]) + input, outputSize)
    }

    func send(_ selector: UInt32, _ input: [UInt8], _ outputSize: Int) -> (kern_return_t, [UInt8]) {
        var output = [UInt8](repeating: 0, count: outputSize)
        var size = outputSize
        let kr = input.withUnsafeBytes { inBytes in
            output.withUnsafeMutableBytes { outBytes in
                callStruct(connection, selector, inBytes.baseAddress, inBytes.count, outBytes.baseAddress, &size)
            }
        }
        return (kr, output)
    }

    func words(_ values: [UInt32]) -> [UInt8] { values.flatMap { withUnsafeBytes(of: $0.littleEndian, Array.init) } }
}
