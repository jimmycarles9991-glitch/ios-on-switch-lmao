import Darwin
import Foundation
import HWTestCore
import MachO
import SwitchA64XProbe

/// The app's own code as the kernel maps it. An app built for iOS 26 or later is A12 code (LSE atomics, FP16
/// arithmetic): the a64x loader must translate its image (rewritten= counts the __text words that differ in memory
/// from the executable on disk) and run every site through a stub, never the trap path. An iOS 17 app is ARMv8.0 and
/// maps as is. Either way four threads' atomic adds must come out exact, and a half-precision multiply then add must
/// round as FP16 does. The kern.a64x.* counters (the iOS 27 kernel's) are reported as they stand after the run, with
/// d_traps, d_traps_unhandled and d_stub_mismatch over it.
struct A64XSelfTest: HWTest {
    let id = "a64x.self"
    let name = "The app's own LSE and FP16 code as the a64x loader maps it"
    let subsystem = Subsystem.cpu

    static let counters = [
        "images", "images_native", "images_marked", "text_pages", "stub_words", "stub_mismatch", "traps",
        "traps_unhandled",
    ]
    static let (threads, adds) = (4, 1_000_000)
    /// (1 + 2^-10)^2 rounds to 1 + 2^-9 in half precision; adding 2^-11 lands halfway between two halves and rounds
    /// to the even one, 1 + 2^-9 again. Fused or single-precision arithmetic gives 1 + 3 * 2^-10.
    static let (fp16A, fp16B, fp16C, fp16Want): (Float, Float, Float, Float) = (
        1 + 0x1p-10, 1 + 0x1p-10, 0x1p-11, 1 + 0x1p-9
    )

    func run(_ context: TestContext) async throws -> Outcome {
        let before = Self.sysctls().values
        let (rewritten, words) = try Self.rewrittenTextWords()
        nonisolated(unsafe) let counter = UnsafeMutablePointer<UInt64>.allocate(capacity: 1)
        counter.initialize(to: 0)
        defer { counter.deallocate() }
        DispatchQueue.concurrentPerform(iterations: Self.threads) { _ in
            switch_a64x_hammer(counter, UInt64(Self.adds))
        }
        let sum = switch_a64x_fp16_mul_add(Self.fp16A, Self.fp16B, Self.fp16C)
        let (after, unread) = Self.sysctls()
        let delta = { (name: String) in (after[name] ?? 0) - (before[name] ?? 0) }

        var metrics = Metrics()
        metrics.set("a64x", !after.isEmpty)
        if after.isEmpty { metrics.set("sysctl", KernelSysctl.code(unread)) }
        metrics.set("translated", rewritten > 0)
        metrics.set("rewritten", rewritten)
        metrics.set("text_words", words)
        metrics.set("count", Int(counter.pointee))
        metrics.set("fp16", Double(sum), digits: 10)
        for name in Self.counters { if let value = after[name] { metrics.set(name, value) } }
        for name in ["traps", "traps_unhandled", "stub_mismatch"] where !after.isEmpty {
            metrics.set("d_" + name, delta(name))
        }

        var checks: [(String, Bool)] = [
            (
                "\(Self.threads) threads x \(Self.adds) atomic adds exact",
                counter.pointee == UInt64(Self.threads * Self.adds)
            ),
            ("FP16 multiply then add rounds to even (\(Self.fp16Want))", sum == Self.fp16Want),
        ]
        if context.ios >= 26 {
            checks += [
                (
                    after.isEmpty && unread != ENOENT
                        ? KernelSysctl.unreadable("kern.a64x.*", unread) : "the kernel has a64x (kern.a64x.*)",
                    !after.isEmpty
                ),
                ("the app's A12 code was translated (rewritten > 0)", rewritten > 0),
                ("no LSE site took the trap path (d_traps 0)", delta("traps") == 0),
                ("no unhandled instruction (d_traps_unhandled 0)", delta("traps_unhandled") == 0),
                ("every page's stubs matched (d_stub_mismatch 0)", delta("stub_mismatch") == 0),
            ]
        } else {
            checks.append(("an ARMv8.0 app maps as is (rewritten 0)", rewritten == 0))
        }
        return .judged(checks, metrics)
    }

    /// kern.a64x.* by name, and the errno of the first that could not be read.
    static func sysctls() -> (values: [String: Int], unread: Int32) {
        var values: [String: Int] = [:]
        var unread: Int32 = 0
        for name in counters {
            switch KernelSysctl.integer("kern.a64x." + name, as: UInt64.self) {
            case .value(let value): values[name] = Int(value)
            case .failed(let error): if unread == 0 { unread = error }
            }
        }
        return (values, unread)
    }

    /// The words of this image's __TEXT,__text that differ between memory and the executable file, and how many
    /// there are. Reading them pages the whole section in. __TEXT starts at file offset 0 with the Mach-O header,
    /// so a __TEXT section's distance from the header in memory is its offset in the file.
    static func rewrittenTextWords() throws -> (rewritten: Int, words: Int) {
        let header = #dsohandle.assumingMemoryBound(to: mach_header_64.self)
        var size: UInt = 0
        guard let mapped = getsectiondata(header, "__TEXT", "__text", &size) else {
            throw TestFailure("__TEXT,__text is not mapped")
        }
        guard let path = Bundle.main.executablePath, let file = FileManager.default.contents(atPath: path) else {
            throw TestFailure("the executable cannot be read")
        }
        let (offset, length) = (UnsafeRawPointer(mapped) - #dsohandle, Int(size))
        guard file.count >= offset + length,
            file.prefix(4).elementsEqual(withUnsafeBytes(of: MH_MAGIC_64.littleEndian, Array.init))
        else { throw TestFailure("the executable is not the thin image this process maps") }
        let words = length / 4
        let rewritten = file.withUnsafeBytes { disk in
            let memory = UnsafeRawPointer(mapped)
            return (0..<words).reduce(0) { count, index in
                let at = index * 4
                let changed =
                    disk.loadUnaligned(fromByteOffset: offset + at, as: UInt32.self)
                    != memory.loadUnaligned(fromByteOffset: at, as: UInt32.self)
                return count + (changed ? 1 : 0)
            }
        }
        return (rewritten, words)
    }
}
