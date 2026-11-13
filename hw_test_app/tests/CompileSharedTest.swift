import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The system shader compiler's store (drivers/TegraGM20BMetal/compiler) across requesters and processes. Each run salts a
/// render state and a kernel no pack or store holds yet. Two threads that create the state at once get it from one
/// compile; the state the previous run salted comes from the store with no request at all (that run was another process,
/// maybe another boot, of the same store); a creation on the main thread sleeps while the compiler compiles it, its worker
/// at the main thread's class; and nothing compiles in this process (the counters' compiled_here).
struct CompileSharedTest: HWTest {
    let id = "gpu.compile.shared"
    let name = "States the pack lacks compiled once for every process and boot, never on a UI thread"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 180 }

    static let saltKey = "gpu.compile.shared.salt"
    static let storeKey = "gpu.compile.shared.store"

    struct Made {
        var how = "none"
        var waitUs = 0
        var qos = 0
        var ranClass = 0
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade")
        }
        let before = RawFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []

        let store = RawFacade.compilerStore(of: gpu.device) ?? ""
        metrics.set("store", store.isEmpty ? "none" : String(store.prefix(16)))
        checks.append(("the process maps the shader compiler's store", !store.isEmpty))
        let previousStore = UserDefaults.standard.string(forKey: Self.storeKey)
        if let previous = UserDefaults.standard.object(forKey: Self.saltKey) as? Int, previousStore == store {
            let again = try made(try pipeline(gpu, salt: UInt32(previous)))
            metrics.set("relaunch_how", again.how)
            checks.append(("the previous run's state came from the store with no request", again.how == "store"))
        } else {
            metrics.set("relaunch_how", "first_run_of_this_store")
        }

        let salt = UInt32.random(in: 1...UInt32.max)
        UserDefaults.standard.set(Int(salt), forKey: Self.saltKey)
        UserDefaults.standard.set(store, forKey: Self.storeKey)
        let pair = try await together(gpu, salt: salt)
        let compiled = pair.filter { $0.how == "compiled" }.count
        let shared = pair.filter { ["joined", "stored", "table", "store"].contains($0.how) }.count
        metrics.set("first_how", pair[0].how)
        metrics.set("second_how", pair[1].how)
        checks.append(("two requesters: one compile, the other shares it", compiled == 1 && shared == 1))

        let mainSalt = salt ^ 0x5a5a_5a5a
        let main = try await MainActor.run { try made(try pipeline(gpu, salt: mainSalt)) }
        metrics.set("main_how", main.how)
        metrics.set("main_wait_ms", Double(main.waitUs) / 1000, digits: 1)
        metrics.set("main_qos", String(format: "0x%x", main.qos))
        metrics.set("compiler_class", main.ranClass)
        checks.append(("the main thread's state was compiled by the compiler", main.how == "compiled"))
        checks.append(("at the main thread's class", main.ranClass == 4))

        let wrong = try await kernel(gpu, salt: salt)
        metrics.set("kernel_wrong", wrong)
        checks.append(("the salted kernel's every element exact", wrong == 0))

        let after = RawFacade.counters(of: gpu.device)
        let here = (after["compiled_here"] ?? 0) - (before["compiled_here"] ?? 0)
        metrics.set("compiled_here", here)
        checks.append(("nothing compiled in this process", here == 0))
        return .judged(checks, metrics, proof: "salt \(salt)")
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

    func made(_ state: MTLRenderPipelineState) throws -> Made {
        guard let answer = RawFacade.compile(of: state) else {
            throw TestFailure("the state answers no switchRawCompile (a release build?)")
        }
        return Made(
            how: answer["how"] as? String ?? "?", waitUs: (answer["wait_us"] as? NSNumber)?.intValue ?? 0,
            qos: (answer["qos"] as? NSNumber)?.intValue ?? 0,
            ranClass: (answer["ran_class"] as? NSNumber)?.intValue ?? 0)
    }

    /// The same state created on two threads released together
    func together(_ gpu: GPU, salt: UInt32) async throws -> [Made] {
        let start = DispatchSemaphore(value: 0)
        let results = Results()
        let group = DispatchGroup()
        for slot in 0..<2 {
            group.enter()
            DispatchQueue.global(qos: .userInitiated).async {
                start.wait()
                results.set(slot, Result { try made(try pipeline(gpu, salt: salt)) })
                group.leave()
            }
        }
        start.signal()
        start.signal()
        await withCheckedContinuation { continuation in group.notify(queue: .global()) { continuation.resume() } }
        return try results.all()
    }

    func kernel(_ gpu: GPU, salt: UInt32) async throws -> Int {
        var value = salt
        let constants = MTLFunctionConstantValues()
        constants.setConstantValue(&value, type: .uint, index: 0)
        let function = try await gpu.library.makeFunction(name: "salted_kernel", constantValues: constants)
        let state = try await gpu.device.makeComputePipelineState(function: function)
        let count = 1024
        let data = try gpu.buffer(count * 4)
        try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(state)
            encoder.setBuffer(data, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: count, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1))
            encoder.endEncoding()
        }
        let values = data.contents().bindMemory(to: UInt32.self, capacity: count)
        return (0..<count).filter { values[$0] != UInt32($0) ^ salt }.count
    }

    final class Results: @unchecked Sendable {
        private let lock = NSLock()
        private var made: [Int: Result<Made, Error>] = [:]

        func set(_ slot: Int, _ result: Result<Made, Error>) {
            lock.lock()
            made[slot] = result
            lock.unlock()
        }

        func all() throws -> [Made] {
            lock.lock()
            defer { lock.unlock() }
            return try (0..<2).map { try made[$0]!.get() }
        }
    }
}
