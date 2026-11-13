import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// LIMITS 3D-G13: SET_CONSTANT_COLOR_RENDERING (0x0f40, colour words 0x0f44-0x0f50), the ROP writing one colour for
/// every covered sample without the pixel program's output; no open driver sets it on GM20B. Arms, one command buffer
/// each, the program drawing (0.2, 0.4, 0.6, 1) while the constant names (0.8, 0.1, 0.3, 1):
/// - shader: the method off, the program's colour (the reference route);
/// - float: the constant as float bits, as the 3D clear takes them for unorm targets;
/// - packed: the constant as one unorm8 code a word;
/// - blend: float bits with alpha 0.5 under source-over over the clear colour (whether the blend unit sees it).
/// The claim: the float arm's covered pixels read the constant exactly and its uncovered ones the clear. Then 32
/// full-target 1280x720 draws with the constant on against off give the pixel program's share of a solid fill.
/// gv100 names the draw-time class error CONSTANT_COLOR_RENDERING_WITHOUT_PRE_PS_COVERAGE (0x138): a refused arm is
/// recorded, not judged.
enum X1ConstantColourProbe {
    static let setConstantColour: UInt32 = 0x0f40
    static let size = 64
    static let program = SIMD4<Float>(0.2, 0.4, 0.6, 1)
    static let constant: [Float] = [0.8, 0.1, 0.3, 1]

    static func words(_ rgba: [UInt32]) -> [(UInt32, UInt32)] {
        [(setConstantColour, 1)] + rgba.enumerated().map { (setConstantColour + 4 + 4 * UInt32($0.offset), $0.element) }
    }

    static let off: [(UInt32, UInt32)] = [(setConstantColour, 0)]

    static func code(_ v: Float) -> UInt8 { UInt8((v * 255).rounded()) }

    /// BGRA8 bytes of an RGBA colour
    static func bgra(_ c: [Float]) -> [UInt8] { [code(c[2]), code(c[1]), code(c[0]), code(c[3])] }

    static func pass(_ target: MTLTexture) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        return pass
    }

    static func draw(
        _ state: MTLRenderPipelineState, _ target: MTLTexture, quad: [SIMD4<Float>], count: Int
    )
        -> (MTLCommandBuffer) -> Void
    {
        { commands in
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass(target))!
            encoder.setRenderPipelineState(state)
            for _ in 0..<count { GPU.draw(encoder, corners: quad, color: program) }
            encoder.endEncoding()
        }
    }

    struct Arm {
        let name: String
        let words: [(UInt32, UInt32)]
        let blend: Bool
        let expected: [UInt8]
    }

    static let arms: [Arm] = [
        Arm(name: "shader", words: [], blend: false, expected: bgra([0.2, 0.4, 0.6, 1])),
        Arm(name: "float", words: words(constant.map(\.bitPattern)), blend: false, expected: bgra(constant)),
        Arm(name: "packed", words: words(constant.map { UInt32(code($0)) }), blend: false, expected: bgra(constant)),
        Arm(
            name: "blend", words: words([0.4, 0.05, 0.15, 0.5].map { Float($0).bitPattern }), blend: true,
            expected: bgra([0.4, 0.05, 0.15, 0.5 + 0.5])),
    ]

    static let probe = X1Probe(
        id: "gpu.x1.constant_colour", name: "Constant-colour rendering: the ROP's colour without the pixel program",
        probe: "3D-G13", risky: true
    ) { gpu in
        var wrong: [String: Int] = [:]
        var metrics = Metrics()
        var paths: [String] = []
        var emitted = 0
        let quad = GPU.pixelQuad(8, 8, 40, 24, width: size, height: size)
        for arm in arms {
            let target = try gpu.texture(.bgra8Unorm, size, size)
            let state = try gpu.render(
                fragment: "solid_fragment", format: .bgra8Unorm,
                configure: arm.blend ? X1RawProbes.sourceOver : { _ in })
            do {
                if arm.words.isEmpty {
                    paths.append(try await gpu.run(draw(state, target, quad: quad, count: 1)).path)
                } else {
                    let drawn = try await RawMethods.draw(
                        gpu, before: arm.words, after: off, draw(state, target, quad: quad, count: 1))
                    paths.append(drawn.path)
                    emitted += drawn.emitted
                }
            } catch {
                metrics.set("\(arm.name)_refused", String(describing: error).prefix(80).description)
                continue
            }
            let bytes = try await gpu.read(target, bytesPerPixel: 4)
            var covered = 0, outside = 0
            for y in 0..<size {
                for x in 0..<size {
                    let inside = x >= 8 && x < 40 && y >= 8 && y < 24
                    let texel = Array(bytes.pixel(x, y, width: size))
                    let differs = zip(texel, arm.expected).contains { abs(Int($0) - Int($1)) > 1 }
                    if inside && differs { covered += 1 }
                    if !inside && texel != [0, 0, 0, 255] { outside += 1 }
                }
            }
            let centre = Array(bytes.pixel(16, 16, width: size)).map { String($0) }
            metrics.set("\(arm.name)_texel", centre.joined(separator: "/"))
            metrics.set("\(arm.name)_outside_wrong", outside)
            if arm.name == "float" || arm.name == "shader" {
                wrong["\(arm.name)_covered_wrong"] = covered
            } else {
                metrics.set("\(arm.name)_covered_wrong", covered)
            }
        }
        let big = try gpu.texture(.bgra8Unorm, 1280, 720)
        let state = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        var onMs = 0.0, offMs = 0.0
        for _ in 0..<3 {
            offMs += try await gpu.run(draw(state, big, quad: GPU.fullScreen, count: 32)).gpuMilliseconds
            if let on = try? await RawMethods.draw(
                gpu, before: words(constant.map(\.bitPattern)), after: off,
                draw(state, big, quad: GPU.fullScreen, count: 32))
            {
                onMs += on.gpuMilliseconds
                emitted += on.emitted
            }
        }
        metrics.set("shader_gpu_ms", offMs / 3, digits: 3)
        metrics.set("constant_gpu_ms", onMs / 3, digits: 3)
        return ProbeFinding.counted(wrong, paths: paths, metrics: metrics).emitting(emitted)
    }
}
