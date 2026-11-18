import Darwin
import Foundation
import HWTestGPU
import HWTestKit

/// The scanout read-back (tests/ScanoutReadback.swift's kernel method) held across presents: armed, it takes the next
/// frame the display hands the GPU; once a query finds it done, the frame is read in place and the read-back is armed
/// again for the next present. A live kernel (TegraGM20BReadbackABI.h, Arm with reserved 1) puts in the second buffer
/// the frame offered before this one, read at this present: what window A scanned at the end of that frame's time on
/// screen. An older debug kernel refuses live; the second buffer is then this frame's scanout buffer, read after the
/// first.
final class ScanoutSession {
    struct Frame {
        let sequence: UInt64
        /// Live: the offer sequence (low 32 bits) of the frame the second buffer holds.
        let previous: UInt32?
        let direct: Bool
        let width: Int
        let height: Int
        let first: UnsafeRawPointer
        let second: UnsafeRawPointer
        let ticks: UInt64
    }

    enum Next {
        case frame(Frame)
        case none
        case failed(String)
    }

    let gpu: GPUConnection
    let live: Bool
    private var mapped: (GPUConnection.Mapping, GPUConnection.Mapping)?
    private var mappedBytes = 0

    /// nil and why: no connection, a release kernel, or the arm refused.
    static func open() -> (ScanoutSession?, String) {
        guard let gpu = GPUConnection() else { return (nil, "no_connection") }
        if readback(gpu, ScanoutReadback.arm, live: true).0 == KERN_SUCCESS {
            return (ScanoutSession(gpu, live: true), "ok")
        }
        let (kr, _) = readback(gpu, ScanoutReadback.arm, live: false)
        guard kr == KERN_SUCCESS else {
            return (nil, kr == GPUConnection.kIOReturnUnsupported ? "release_kernel" : "arm_" + GPUConnection.hex(kr))
        }
        return (ScanoutSession(gpu, live: false), "ok")
    }

    private init(_ gpu: GPUConnection, live: Bool) {
        self.gpu = gpu
        self.live = live
    }

    deinit { close() }

    func close() {
        if let (a, b) = mapped {
            gpu.unmap(a)
            gpu.unmap(b)
            mapped = nil
        }
        _ = Self.readback(gpu, ScanoutReadback.disarm, live: false)
    }

    /// The next frame read back, waiting up to `timeout` seconds for a present; its pointers hold until `rearm()`.
    func next(timeout: TimeInterval) -> Next {
        let deadline = Date().addingTimeInterval(timeout)
        var reply: [UInt8] = []
        repeat {
            let (kr, answer) = Self.readback(gpu, ScanoutReadback.query, live: false)
            guard kr == KERN_SUCCESS else { return .failed("query_" + GPUConnection.hex(kr)) }
            reply = answer
            let state = GPUConnection.int32(reply, 0)
            if state != ScanoutReadback.armed && state != ScanoutReadback.inFlight { break }
            usleep(2_000)
        } while Date() < deadline
        switch GPUConnection.int32(reply, 0) {
        case ScanoutReadback.done: break
        case ScanoutReadback.armed, ScanoutReadback.inFlight: return .none
        default: return .failed("failed_\(GPUConnection.int32(reply, 4))")
        }
        let ticks = mach_absolute_time()
        let (width, height) = (Int(GPUConnection.uint32(reply, 8)), Int(GPUConnection.uint32(reply, 12)))
        let bytes = width * height * 4
        if mapped == nil || mappedBytes != bytes {
            if let (a, b) = mapped {
                gpu.unmap(a)
                gpu.unmap(b)
                mapped = nil
            }
            guard let a = gpu.map(GPUConnection.uint32(reply, 20), bytes: bytes) else { return .failed("map_refused") }
            guard let b = gpu.map(GPUConnection.uint32(reply, 24), bytes: bytes) else {
                gpu.unmap(a)
                return .failed("map_refused")
            }
            mapped = (a, b)
            mappedBytes = bytes
        }
        let flags = GPUConnection.uint32(reply, 16)
        return .frame(
            Frame(
                sequence: GPUConnection.uint64(reply, 32),
                previous: flags & Self.previousFlag != 0 ? GPUConnection.uint32(reply, 28) : nil,
                direct: flags & 1 != 0, width: width, height: height, first: mapped!.0.base, second: mapped!.1.base,
                ticks: ticks))
    }

    /// Arms the read-back for the next present (the same buffers: the mappings stay).
    func rearm() -> Bool {
        Self.readback(gpu, ScanoutReadback.arm, live: live).0 == KERN_SUCCESS
    }

    static let previousFlag: UInt32 = 1 << 2

    static func readback(_ gpu: GPUConnection, _ op: UInt32, live: Bool) -> (kern_return_t, [UInt8]) {
        gpu.call(ScanoutReadback.method, GPUConnection.words([op, live ? 1 : 0]), outputSize: 40)
    }
}
