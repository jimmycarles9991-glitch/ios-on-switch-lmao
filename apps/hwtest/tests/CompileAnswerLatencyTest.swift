import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The shader compiler starts and answers a request at its waiter's class while lower classes hold every core.
/// Boot 22's compiler dropped each worker to the background class before answering and parked it there: on a busy
/// system a state compiled in 216 ms reached its client 8.1 s later, and urgent requests sat seconds before an idle
/// worker took them. Here one thread per core spins at the utility class (above background, below the main thread)
/// for at most `spinLimit`, while the main thread creates a state no pack or store holds. The compiler's account of
/// the wait (RawMetalCompileProbe.mm) is held to the quiet system's: on boot 22 an answer reached a quiet client in
/// 0-1 ms and a HW Test request started 6-12 ms after it was sent (the take thread's copy and prepare of the request).
/// Delivery may cost the waiter less than one 60 Hz frame; the start, three. A background-class worker under this load
/// waits for the scheduler to give the background class a turn, or for the spinners to end.
struct CompileAnswerLatencyTest: HWTest {
    let id = "gpu.compile.answer_latency"
    let name = "A state the pack lacks is started and answered at once while lower classes hold every core"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    static let spinLimit: TimeInterval = 3
    static let deliveryBoundMs = 16.7
    static let queueBoundMs = 50.0

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade")
        }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        let salt = UInt32.random(in: 1...UInt32.max)
        let spinners = Spinners(count: ProcessInfo.processInfo.activeProcessorCount, limit: Self.spinLimit)
        spinners.start()
        let account = try await MainActor.run { try Account(of: try pipeline(gpu, salt: salt)) }
        let spunMs = spinners.stop() * 1000
        let us = { (key: String) in account.us[key] ?? -1 }
        let ms = { (key: String) in Double(us(key)) / 1000 }
        let how = account.how
        metrics.set("how", how)
        metrics.set("spinners", spinners.count)
        metrics.set("spun_ms", spunMs, digits: 1)
        metrics.set("wait_ms", ms("wait_us"), digits: 1)
        metrics.set("queue_ms", ms("queue_us"), digits: 1)
        metrics.set("compile_ms", ms("compile_us"), digits: 1)
        metrics.set("answer_ms", ms("answer_us"), digits: 2)
        metrics.set("wake_ms", ms("wake_us"), digits: 2)
        metrics.set("store_ms", ms("store_us"), digits: 2)
        metrics.set("answer_priority", us("answer_priority"))
        metrics.set("ran_class", us("ran_class"))
        let measured = ["queue_us", "compile_us", "answer_us", "wake_us"].allSatisfy { us($0) >= 0 }
        let deliveryMs = ms("answer_us") + ms("wake_us")
        metrics.set("delivery_ms", deliveryMs, digits: 2)

        checks.append(("the main thread's state was compiled for it", how == "compiled"))
        checks.append(("the compiler gave its account of the wait", measured))
        checks.append(("every core was held below the main thread until the answer", spunMs >= ms("wait_us")))
        checks.append(("started within \(Int(Self.queueBoundMs)) ms", measured && ms("queue_us") <= Self.queueBoundMs))
        checks.append(("delivered within a frame of its compile", measured && deliveryMs <= Self.deliveryBoundMs))
        return .judged(checks, metrics, proof: "salt \(salt)").onRawPath(gpu)
    }

    func pipeline(_ gpu: GPU, salt: UInt32) throws -> MTLRenderPipelineState {
        var value = salt
        let constants = MTLFunctionConstantValues()
        constants.setConstantValue(&value, type: .uint, index: 0)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = gpu.library.makeFunction(name: "quad_vertex")
        descriptor.fragmentFunction = try gpu.library.makeFunction(name: "salted_fragment", constantValues: constants)
        descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
        return try gpu.device.makeRenderPipelineState(descriptor: descriptor)
    }

    /// The creation's switchRawCompile, its times in microseconds
    struct Account: Sendable {
        static let keys = [
            "wait_us", "queue_us", "compile_us", "answer_us", "wake_us", "store_us", "answer_priority", "ran_class",
        ]
        var how = "?"
        var us: [String: Int] = [:]

        init(of state: MTLRenderPipelineState) throws {
            guard let answer = RawFacade.compile(of: state) else {
                throw TestFailure("the state answers no switchRawCompile (a release build?)")
            }
            how = answer["how"] as? String ?? "?"
            for key in Self.keys {
                if let value = (answer[key] as? NSNumber)?.intValue { us[key] = value }
            }
        }
    }

    /// Threads at the utility class that spin until stopped or until `limit` passes, whichever is first
    final class Spinners: @unchecked Sendable {
        let count: Int
        private let limit: TimeInterval
        private let lock = NSLock()
        private var stopped = false
        private var began = Date()

        init(count: Int, limit: TimeInterval) {
            self.count = count
            self.limit = limit
        }

        private var isStopped: Bool {
            lock.lock()
            defer { lock.unlock() }
            return stopped
        }

        func start() {
            began = Date()
            let deadline = began.addingTimeInterval(limit)
            for _ in 0..<count {
                let thread = Thread { [self] in
                    while !isStopped && Date() < deadline {}
                }
                thread.qualityOfService = .utility
                thread.start()
            }
        }

        /// How long they spun
        func stop() -> TimeInterval {
            lock.lock()
            stopped = true
            lock.unlock()
            return min(Date().timeIntervalSince(began), limit)
        }
    }
}
