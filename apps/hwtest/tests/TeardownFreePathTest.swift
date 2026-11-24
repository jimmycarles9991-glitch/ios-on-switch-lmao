import Darwin
import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Resource teardown detaches what it lets go of under Device::lock_ and runs BoFree, VA release, vm_deallocate and
/// CFRelease synchronously on the calling thread once the lock is let go (drivers/TegraGM20BMetal/alias.cpp,
/// arena.cpp, memory.cpp). Before, holding Device::lock_ across those kernel and VM calls stalled every concurrent
/// creation behind BoFree (24-120 us measured, up to 9.4 ms behind the submit gate) and multi-MiB vm_deallocate.
/// One thread drops 16 resident 2 MiB shared heaps one by one while this thread creates and releases small private
/// textures from a warmed chunk (taking Device::lock_ with no kernel call): overlapping creations' p99 must stay under
/// the teardowns' p50. A second arm verifies synchronous no-copy BoFree: releasing a 4 KiB no-copy buffer invokes its
/// deallocator and frees its kernel backing before return, so immediately wrapping 128 KiB over the same pages never
/// fails with a mapped-backing collision.
struct TeardownFreePathTest: GPUPathTest {
    let id = "gpu.path.teardown_free"
    let name = "Resource teardown runs BoFree and vm_deallocate with the device lock let go"
    let counters: [String] = []
    static let heapCount = 16, heapBytes = 2 << 20
    static let noCopyRounds = 16, noCopySmall = 4 << 10, noCopyBig = 128 << 10

    struct Span {
        let start: Double, end: Double
        var ms: Double { end - start }
        func overlaps(_ other: Span) -> Bool { start < other.end && other.start < end }
    }

    final class Releaser: @unchecked Sendable {
        private let lock = NSLock()
        private var running = true
        private let ready = DispatchSemaphore(value: 0)
        private let finished = DispatchSemaphore(value: 0)
        private var heaps: [MTLHeap]
        private(set) var spans: [Span] = []

        init(_ heaps: [MTLHeap]) { self.heaps = heaps }

        var isRunning: Bool {
            lock.lock()
            defer { lock.unlock() }
            return running
        }

        func start() {
            Thread { [self] in
                ready.wait()
                while !heaps.isEmpty {
                    let start = TeardownFreePathTest.nowMs()
                    autoreleasepool { _ = heaps.removeLast() }
                    spans.append(Span(start: start, end: TeardownFreePathTest.nowMs()))
                }
                lock.lock()
                running = false
                lock.unlock()
                finished.signal()
            }.start()
        }

        func go() { ready.signal() }
        func wait() { finished.wait() }
    }

    static func nowMs() -> Double { Double(DispatchTime.now().uptimeNanoseconds) / 1e6 }

    func exercise(_ gpu: GPU, _: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let small = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: 16, height: 16, mipmapped: false
        )
        small.storageMode = .private
        small.usage = [.shaderRead, .pixelFormatView]
        guard let keeper = gpu.device.makeTexture(descriptor: small) else {
            throw TestFailure("failed to make warm private texture")
        }

        let heapDesc = MTLHeapDescriptor()
        heapDesc.size = Self.heapBytes
        heapDesc.storageMode = .shared
        var heaps: [MTLHeap] = []
        heaps.reserveCapacity(Self.heapCount)
        for _ in 0 ..< Self.heapCount {
            guard let heap = gpu.device.makeHeap(descriptor: heapDesc) else {
                throw TestFailure("failed to make shared heap")
            }
            autoreleasepool {
                if let buf = heap.makeBuffer(length: Self.heapBytes, options: .storageModeShared) {
                    memset(buf.contents(), 0x5A, Self.heapBytes)
                }
            }
            heaps.append(heap)
        }

        let releaser = Releaser(heaps)
        heaps.removeAll()
        releaser.start()
        var creates: [Span] = []
        releaser.go()
        while releaser.isRunning {
            let start = Self.nowMs()
            let ok = autoreleasepool { gpu.device.makeTexture(descriptor: small) != nil }
            let end = Self.nowMs()
            if !ok { throw TestFailure("failed to make private texture beside heap teardown") }
            creates.append(Span(start: start, end: end))
        }
        releaser.wait()
        withExtendedLifetime(keeper) {}

        let overlapped = creates.filter { span in releaser.spans.contains { $0.overlaps(span) } }
        let teardownMs = releaser.spans.map(\.ms)
        let createMs = overlapped.map(\.ms)
        let teardownP50 = Stats.percentile(teardownMs, 0.5)
        let createP99 = Stats.percentile(createMs, 0.99)

        metrics.set("teardowns", releaser.spans.count)
        metrics.set("overlapped", overlapped.count)
        metrics.set("teardown_p50_ms", teardownP50, digits: 3)
        metrics.set("teardown_p99_ms", Stats.percentile(teardownMs, 0.99), digits: 3)
        metrics.set("create_p50_ms", Stats.percentile(createMs, 0.5), digits: 3)
        metrics.set("create_p99_ms", createP99, digits: 3)

        var failures: [String] = []
        if overlapped.isEmpty {
            failures.append("no texture creation overlapped a heap teardown")
        } else if createP99 >= teardownP50 {
            failures.append(
                String(
                    format: "creation p99 %.3f ms beside teardown not under teardown p50 %.3f ms",
                    createP99, teardownP50
                )
            )
        }

        var pages: UnsafeMutableRawPointer?
        guard posix_memalign(&pages, Int(getpagesize()), Self.noCopyBig) == 0, let pages else {
            throw TestFailure("no page-aligned memory for no-copy release check")
        }
        defer { free(pages) }
        var deallocCount = 0
        var growsOk = 0
        for round in 0 ..< Self.noCopyRounds {
            let smallMade = autoreleasepool {
                gpu.device.makeBuffer(
                    bytesNoCopy: pages, length: Self.noCopySmall, options: .storageModeShared,
                    deallocator: { _, _ in deallocCount += 1 }
                ) != nil
            }
            if !smallMade {
                failures.append("round \(round): 4 KiB no-copy buffer returned nil")
                break
            }
            if deallocCount != round + 1 {
                failures.append("round \(round): no-copy deallocator ran \(deallocCount) times, expected \(round + 1)")
                break
            }
            let bigMade = autoreleasepool {
                gpu.device.makeBuffer(
                    bytesNoCopy: pages, length: Self.noCopyBig, options: .storageModeShared, deallocator: nil
                ) != nil
            }
            if !bigMade {
                failures.append("round \(round): 128 KiB no-copy buffer refused right after 4 KiB release")
                break
            }
            growsOk += 1
        }
        metrics.set("nocopy_grows", growsOk)
        return failures
    }
}
