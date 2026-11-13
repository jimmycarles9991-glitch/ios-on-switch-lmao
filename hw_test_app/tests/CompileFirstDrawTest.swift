import Darwin
import Foundation
import HWTestCore
import HWTestGPU
import Metal
import SwitchCompilerProbe
import notify

/// A render state the pack lacks is usable when its creation returns: each salted state (no pack or store holds it before
/// its run) is drawn by the first command buffer after its creation, on the raw path, every pixel its colour. Made once
/// by the synchronous form, once by the completion-handler form, and once while the shader compiler dies holding its
/// request (a debug compiler's com.brunostjohn.switch.debug.compiler-dies-at-next-request, its state this app's pid: the
/// compiler dies at this app's next request): the creation completes once launchd's next compiler has made it. Nothing goes to m2v (T-v3g: state.making, a returned state still being made) and
/// nothing compiles in this process.
struct CompileFirstDrawTest: HWTest {
    let id = "gpu.compile.first_draw"
    let name = "A state the pack lacks draws raw from the first command buffer after its creation"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 180 }

    static let size = 16

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade")
        }
        let before = RawFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        let salt = UInt32.random(in: 1...UInt32.max / 4)

        let made = try pipeline(gpu, salt: salt)
        checks += try await drawn(gpu, made, salt: salt, label: "created", &metrics)

        let later = try await gpu.device.makeRenderPipelineState(descriptor: try descriptor(gpu, salt: salt + 1))
        checks += try await drawn(gpu, later, salt: salt + 1, label: "handler", &metrics)

        var probe = switch_compiler_probe()
        switch_compiler_probe(&probe)
        let generation = probe.generation
        let armed = Self.armCompilerDeath()
        metrics.set("armed", armed ? "yes" : "no")
        checks.append(("the compiler's death armed for this app's next request", armed))
        let survived = try pipeline(gpu, salt: salt + 2)
        switch_compiler_probe(&probe)
        if probe.generation > generation { DeliberateCompilerDeaths.shared.add() }
        metrics.set("generations", Int(probe.generation) - Int(generation))
        checks.append(("the compiler died holding the request and the next one made it", probe.generation > generation))
        if let answer = RawFacade.compile(of: survived) {
            metrics.set("killed_how", answer["how"] as? String ?? "?")
            metrics.set("killed_wait_ms", Double((answer["wait_us"] as? NSNumber)?.intValue ?? 0) / 1000, digits: 1)
        }
        checks += try await drawn(gpu, survived, salt: salt + 2, label: "killed", &metrics)

        let paths = gpu.paths.paths.keys.sorted()
        metrics.set("paths", paths.joined(separator: ","))
        checks.append(("every command buffer on the raw path", paths == ["raw"]))
        let after = RawFacade.counters(of: gpu.device)
        let here = (after["compiled_here"] ?? 0) - (before["compiled_here"] ?? 0)
        metrics.set("compiled_here", here)
        checks.append(("nothing compiled in this process", here == 0))
        return .judged(checks, metrics, proof: "salt \(salt)")
    }

    /// The debug compiler's death at this process's next request: the name's notify state is the pid it dies for, which
    /// the compiler reads as it takes each request
    static func armCompilerDeath() -> Bool {
        var token: Int32 = 0
        guard
            notify_register_check("com.brunostjohn.switch.debug.compiler-dies-at-next-request", &token)
                == NOTIFY_STATUS_OK
        else { return false }
        defer { notify_cancel(token) }
        return notify_set_state(token, UInt64(getpid())) == NOTIFY_STATUS_OK
    }

    func descriptor(_ gpu: GPU, salt: UInt32) throws -> MTLRenderPipelineDescriptor {
        var value = salt
        let constants = MTLFunctionConstantValues()
        constants.setConstantValue(&value, type: .uint, index: 0)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = gpu.library.makeFunction(name: "quad_vertex")
        descriptor.fragmentFunction = try gpu.library.makeFunction(name: "salted_fragment", constantValues: constants)
        descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
        return descriptor
    }

    func pipeline(_ gpu: GPU, salt: UInt32) throws -> MTLRenderPipelineState {
        try gpu.device.makeRenderPipelineState(descriptor: try descriptor(gpu, salt: salt))
    }

    /// The state drawn by the first command buffer after its creation, read back: salted_fragment's colour everywhere
    func drawn(
        _ gpu: GPU, _ state: MTLRenderPipelineState, salt: UInt32, label: String, _ metrics: inout Metrics
    ) async throws -> [(String, Bool)] {
        let target = try gpu.texture(.bgra8Unorm, Self.size, Self.size)
        let timing = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        let bytes = try await gpu.read(target, bytesPerPixel: 4)
        let want: [UInt8] = [64, UInt8((salt >> 8) & 0xff), UInt8(salt & 0xff), 255]
        let wrong = (0..<(Self.size * Self.size)).filter {
            Array(bytes.pixel($0 % Self.size, $0 / Self.size, width: Self.size)) != want
        }.count
        metrics.set(label + "_path", timing.path)
        metrics.set(label + "_wrong", wrong)
        return [
            ("\(label): its first command buffer on the raw path", timing.path == "raw"),
            ("\(label): every pixel its colour", wrong == 0),
        ]
    }
}

/// The compiler deaths a test caused on purpose this run, which system.compiler_alive counts as no loss
final class DeliberateCompilerDeaths: @unchecked Sendable {
    static let shared = DeliberateCompilerDeaths()
    private let lock = NSLock()
    private var deaths = 0

    func add() {
        lock.lock()
        deaths += 1
        lock.unlock()
    }

    var count: Int {
        lock.lock()
        defer { lock.unlock() }
        return deaths
    }
}
