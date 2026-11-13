import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Depth/stencil states in passes with no depth or stencil attachment, as plain Metal draws them: Always tests nothing,
/// Never fails every fragment, any other compare (and a stencil test) tests a hidden depth of 1.0 and stencil of 0 that
/// the pass's later draws see, and each pass starts its own.
struct NativeHiddenDepthTest: NativeObjectTest {
    let id = "metal.native.hidden_depth"
    let name = "A pass without a depth attachment tests depth and stencil against hidden ones, per pass"

    static let red: SIMD4<Float> = [1, 0, 0, 1], green: SIMD4<Float> = [0, 1, 0, 1], blue: SIMD4<Float> = [0, 0, 1, 1]
    static let yellow: SIMD4<Float> = [1, 1, 0, 1], magenta: SIMD4<Float> = [1, 0, 1, 1]
    static let cyan: SIMD4<Float> = [0, 1, 1, 1], white: SIMD4<Float> = [1, 1, 1, 1]

    static func stencil(_ compare: MTLCompareFunction) -> MTLStencilDescriptor {
        let face = MTLStencilDescriptor()
        face.stencilCompareFunction = compare
        return face
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let size = 64
        let color = try gpu.texture(.rgba8Unorm, size, size)
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        let d = gpu.device
        let lessWrites = try NativeDepthStencilTest.state(d, depth: .less, write: true)
        let less = try NativeDepthStencilTest.state(d, depth: .less, write: false)
        let never = try NativeDepthStencilTest.state(d, depth: .never, write: false)
        let always = try NativeDepthStencilTest.state(d, depth: .always, write: false)
        let notEqual = try NativeDepthStencilTest.state(
            d, depth: .always, write: false, front: Self.stencil(.notEqual), back: Self.stencil(.notEqual))
        let equal = try NativeDepthStencilTest.state(
            d, depth: .always, write: false, front: Self.stencil(.equal), back: Self.stencil(.equal))
        var work = NativeWork(objects: [lessWrites, less, never, always, notEqual, equal])
        func pass(_ load: MTLLoadAction) -> MTLRenderPassDescriptor {
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = color
            pass.colorAttachments[0].loadAction = load
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            return pass
        }
        let timing = try await gpu.run { commands in
            let first = commands.makeRenderCommandEncoder(descriptor: pass(.clear))!
            first.setRenderPipelineState(pipeline)
            first.setDepthStencilState(lessWrites)
            GPU.draw(first, corners: GPU.quad(x0: -1, y0: -1, x1: 0, y1: 1, z: 0.5), color: Self.red)
            first.setDepthStencilState(less)
            GPU.draw(first, corners: GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: 0.7), color: Self.green)
            first.setDepthStencilState(never)
            GPU.draw(first, corners: GPU.quad(x0: -1, y0: 0, x1: 1, y1: 1, z: 0.2), color: Self.blue)
            first.setDepthStencilState(always)
            GPU.draw(first, corners: GPU.quad(x0: 0, y0: -1, x1: 1, y1: 0, z: 0.9), color: Self.yellow)
            first.endEncoding()
            let second = commands.makeRenderCommandEncoder(descriptor: pass(.load))!
            second.setRenderPipelineState(pipeline)
            second.setDepthStencilState(less)
            GPU.draw(second, corners: GPU.quad(x0: -1, y0: -1, x1: 0, y1: 1, z: 0.9), color: Self.magenta)
            second.setDepthStencilState(notEqual)
            second.setStencilReferenceValue(0)
            GPU.draw(second, corners: GPU.quad(x0: -1, y0: -1, x1: 1, y1: 1, z: 0.5), color: Self.cyan)
            second.setDepthStencilState(equal)
            GPU.draw(second, corners: GPU.quad(x0: 0, y0: 0, x1: 1, y1: 1, z: 0.5), color: Self.white)
            second.endEncoding()
        }
        work.paths.append(timing.path)
        let pixels = try await gpu.read(color, bytesPerPixel: 4)
        var wrong = [String: Int]()
        for y in 0..<size {
            for x in 0..<size {
                let (name, want): (String, [UInt8]) =
                    x < size / 2
                    ? ("left", [255, 0, 255, 255])
                    : y < size / 2
                        ? ("top_right", [255, 255, 255, 255])
                        : ("bottom_right", [255, 255, 0, 255])
                if Array(pixels.pixel(x, y, width: size)) != want { wrong[name, default: 0] += 1 }
            }
        }
        let total = wrong.values.reduce(0, +)
        metrics.set("wrong_pixels", total)
        metrics.set(
            "wrong",
            wrong.isEmpty
                ? "-"
                : wrong.sorted { $0.key < $1.key }.map { "\($0.key):\($0.value)" }
                    .joined(separator: ","))
        if total != 0 { work.failures.append("\(total) pixels miss what the hidden depth and stencil decide") }
        return work
    }
}
