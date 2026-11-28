import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal
import MetalPerformanceShaders

/// A function's functionConstantsDictionary, vertexAttributes and stageInputAttributes come from its metallib's public
/// metadata (drivers/TegraGM20BMetal/function_tags.h), never from an m2v twin. The expected answers for
/// shaders/FunctionTags.metal are what an M-series Mac's Metal gives for the same source built for iOS. A function made
/// with constant values answers no constants, and the attributes its constants leave (tags_has_normal false drops
/// normal), which the shader compiler reads from its specialized AIR. MPS reads its kernels' constants before
/// specializing them, so a Gaussian blur is encoded too. The case fails on a wrong answer, or on any command buffer
/// that failed, MPS's included (failed_cbs=, failed_why=).
struct FunctionTagsTest: HWTest {
    let id = "gpu.gm20b.function_tags"
    let name = "Function constants and attributes from the metallib"
    let subsystem = Subsystem.metal

    static let measured: [(function: String, constants: String, vertex: String, stageInput: String)] = [
        (
            "tags_vertex", "tags_has_normal:53:4:o tags_scale:3:1:r ",
            "position:0:6:a-- uv:2:4:a-- normal:5:5:a-- colour:7:19:--- ",
            "position:0:6:a-- uv:2:4:a-- normal:5:5:a-- colour:7:19:--- "
        ),
        ("tags_patch", "", "p:0:6:a-c extra:3:6:ap- ", "p:0:6:a-c extra:3:6:ap- "),
        ("tags_fragment", "tags_tint:53:0:o ", "nil", "nil"),
        ("tags_kernel", "tags_mode:29:3:r ", "nil", "p:1:4:a-- id:4:33:--- "),
    ]

    static func constants(_ f: MTLFunction) -> String {
        f.functionConstantsDictionary.keys.sorted().map { key in
            let c = f.functionConstantsDictionary[key]!
            return "\(c.name):\(c.type.rawValue):\(c.index):\(c.required ? "r" : "o") "
        }.joined()
    }

    static func attribute(
        _ name: String, _ index: Int, _ type: MTLDataType, _ active: Bool, _ patch: Bool, _ controlPoint: Bool
    ) -> String {
        "\(name):\(index):\(type.rawValue):\(active ? "a" : "-")\(patch ? "p" : "-")\(controlPoint ? "c" : "-") "
    }

    static func attributes(_ list: [MTLVertexAttribute]?) -> String {
        guard let list else { return "nil" }
        return list.map {
            attribute(
                $0.name, $0.attributeIndex, $0.attributeType, $0.isActive, $0.isPatchData, $0.isPatchControlPointData
            )
        }.joined()
    }

    static func attributes(_ list: [MTLAttribute]?) -> String {
        guard let list else { return "nil" }
        return list.map {
            attribute(
                $0.name, $0.attributeIndex, $0.attributeType, $0.isActive, $0.isPatchData, $0.isPatchControlPointData
            )
        }.joined()
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var wrong: [String] = []

        for m in Self.measured {
            guard let f = gpu.library.makeFunction(name: m.function) else { throw TestFailure("no \(m.function)") }
            let answers = [
                Self.constants(f), Self.attributes(f.vertexAttributes), Self.attributes(f.stageInputAttributes),
            ]
            metrics.set("\(m.function)_constants", answers[0].isEmpty ? "-" : answers[0])
            if answers != [m.constants, m.vertex, m.stageInput] { wrong.append(m.function) }
        }
        let values = MTLFunctionConstantValues()
        var mode: Int32 = 2
        values.setConstantValue(&mode, type: .int, index: 3)
        let specialized = try await gpu.library.makeFunction(name: "tags_kernel", constantValues: values)
        if !Self.constants(specialized).isEmpty { wrong.append("specialized_constants") }
        for normal in [false, true] {
            let gated = MTLFunctionConstantValues()
            var has = normal
            var scale: Float = 1
            gated.setConstantValue(&has, type: .bool, index: 4)
            gated.setConstantValue(&scale, type: .float, index: 1)
            let f = try await gpu.library.makeFunction(name: "tags_vertex", constantValues: gated)
            let expected =
                "position:0:6:a-- uv:2:4:a-- " + (normal ? "normal:5:5:a-- " : "") + "colour:7:19:--- "
            let answered = Self.attributes(f.vertexAttributes)
            metrics.set("specialized_normal\(normal ? 1 : 0)", answered)
            if answered != expected || Self.attributes(f.stageInputAttributes) != expected {
                wrong.append("specialized_attributes")
            }
        }

        let size = 32
        let source = try gpu.texture(.rgba8Unorm, size, size, usage: [.shaderRead])
        let destination = try gpu.texture(.rgba8Unorm, size, size, usage: [.shaderRead, .shaderWrite])
        try await gpu.upload([UInt8](repeating: 128, count: size * size * 4), to: source, bytesPerRow: size * 4)
        let blur = MPSImageGaussianBlur(device: gpu.device, sigma: 2)
        try await gpu.run { commands in
            blur.encode(commandBuffer: commands, sourceTexture: source, destinationTexture: destination)
        }

        let failed = FailedCounts.judged(
            before: before, after: GM20BFacade.counters(of: gpu.device), own: gpu.paths, &metrics
        )
        if !failed.1 { wrong.append(failed.0) }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
