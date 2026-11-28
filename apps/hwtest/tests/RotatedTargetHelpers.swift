import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

public enum RotatedCell: String, CaseIterable {
    case clear, draw, load, idleLoad, partLoad, pointLoad, edgeLoad, besideR4, points, fetch, position, derivative,
         fwidth, depth, resolve, beside, keptDepth, keptStencil, turns, frames, sized, tie, keptPair, keptPair16

    /// gpu.gm20b.turned_separate_pair's (TurnedSeparatePairTest), and this test's: every other
    public static let pairCells: [RotatedCell] = [.keptPair, .keptPair16]
    public static let targetCells = allCases.filter { !pairCells.contains($0) }

    public enum Kind { case native, informational }

    public var kind: Kind { [.tie, .edgeLoad].contains(self) ? .informational : .native }
}

public struct RotatedStates {
    public let plain, position, derivative, fwidth, fetch, discarded, depthTested, multisampled,
               pair: MTLRenderPipelineState
    public let lessEqual: MTLDepthStencilState

    public init(_ gpu: GPU) throws {
        let make = { (fragment: String) in
            try gpu.render(vertex: "rotated_vertex", fragment: fragment, format: .bgra8Unorm)
        }
        plain = try make("rotated_fragment")
        position = try make("rotated_position_fragment")
        derivative = try make("rotated_derivative_fragment")
        fwidth = try make("rotated_fwidth_fragment")
        fetch = try make("rotated_fetch_fragment")
        discarded = try make("rotated_discard_fragment")
        depthTested = try gpu.render(
            vertex: "rotated_vertex", fragment: "rotated_fragment", format: .bgra8Unorm, depth: .depth32Float
        )
        multisampled = try gpu.render(
            vertex: "rotated_vertex", fragment: "rotated_fragment", format: .bgra8Unorm, samples: 4
        )
        pair = try gpu.render(vertex: "rotated_vertex", fragment: "rotated_pair_fragment", format: .bgra8Unorm) {
            $0.colorAttachments[1].pixelFormat = .bgra8Unorm
        }
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .lessEqual
        d.isDepthWriteEnabled = true
        guard let state = gpu.device.makeDepthStencilState(descriptor: d) else {
            throw TestFailure("makeDepthStencilState returned nil")
        }
        lessEqual = state
    }
}

/// The private textures a cell's runs drew beside the target, in run order
public final class BesideTextures {
    public var textures: [MTLTexture] = []
    public var turnedRun = false
    public var second: (plane: RotatedCells.Plane, cells: RotatedCells)?
    public var plainSecond: MTLTexture?

    public init() {}
}

/// One rotation's cells: each encodes its draws into the turned texture and into a plain one of the frame, and compares
/// the plane's bytes with the plain texture's turned by Metal's store map
public struct RotatedCells {
    public let gpu: GPU
    public let states: RotatedStates
    public let beside = BesideTextures()
    public let turn: Int
    public let width, height: Int
    public let aligned: Bool

    public init(gpu: GPU, states: RotatedStates, turn: Int, width: Int, height: Int, aligned: Bool) {
        self.gpu = gpu
        self.states = states
        self.turn = turn
        self.width = width
        self.height = height
        self.aligned = aligned
    }

    public struct Plane {
        public let surface: IOSurface
        public let texture: MTLTexture
        public let width, height, rowBytes: Int
    }

    public struct Result {
        public let path: String
        public let referencePath: String
        public let wrong: Int
    }

    public var quarter: Bool { turn != 2 }

    /// Metal's store: frame texel (x, y) lands at this plane texel (rotated.rs source_map, Turn::Store)
    public func stored(_ x: Int, _ y: Int) -> (Int, Int) {
        let w = width, h = height
        switch turn {
        case 2: return (w - 1 - x, h - 1 - y)
        case 3: return (y, w - 1 - x)
        default: return (h - 1 - y, x)
        }
    }

    public func makePlane() throws -> Plane? {
        let w = width, h = height
        let pw = quarter ? h : w, ph = quarter ? w : h
        let rowBytes = aligned ? (pw * 4 + 255) / 256 * 256 : (pw * 4 + 15) / 16 * 16
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: pw, .height: ph, .bytesPerElement: 4, .bytesPerRow: rowBytes, .allocSize: rowBytes * ph,
            .pixelFormat: 0x4247_5241,
        ]
        guard let surface = IOSurface(properties: properties) else { throw TestFailure("IOSurface creation failed") }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: w, height: h, mipmapped: false
        )
        descriptor.usage = .renderTarget
        descriptor.storageMode = .shared
        guard descriptor.responds(to: NSSelectorFromString("setRotation:")) else { return nil }
        descriptor.setValue(NSNumber(value: turn), forKey: "rotation")
        if descriptor.responds(to: NSSelectorFromString("setFramebufferOnly:")) {
            descriptor.setValue(NSNumber(value: true), forKey: "framebufferOnly")
        }
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            return nil
        }
        return Plane(surface: surface, texture: texture, width: pw, height: ph, rowBytes: rowBytes)
    }

    public func run(_ cell: RotatedCell, into plane: Plane, checked: Bool = true) async throws -> Result {
        let w = width, h = height
        let reference = try gpu.texture(.bgra8Unorm, w, h, usage: [.renderTarget, .shaderRead])
        try fill(plane, 0x5a)
        try await gpu.upload([UInt8](repeating: 0x5a, count: w * h * 4), to: reference, bytesPerRow: w * 4)
        var referencePath = ""
        var path = ""
        beside.textures.removeAll()
        beside.second = nil
        beside.plainSecond = nil
        for (n, pass) in passes(cell).enumerated() {
            if n > 0 { try await loadAsMetal(cell, before: n, into: reference) }
            beside.turnedRun = false
            let plain = try await gpu.run { pass($0, reference) }.path
            beside.turnedRun = true
            let turned = try await gpu.run { pass($0, plane.texture) }.path
            if referencePath.isEmpty || referencePath == "gm20b" { referencePath = plain }
            if path.isEmpty || path == "gm20b" { path = turned }
        }
        guard checked else { return Result(path: path, referencePath: referencePath, wrong: 0) }
        var wrongBeside = try await secondPlaneWrong()
        let pairs = beside.textures.count / 2
        for i in 0 ..< pairs {
            let a = try await gpu.read(beside.textures[i], bytesPerPixel: 4)
            let b = try await gpu.read(beside.textures[i + pairs], bytesPerPixel: 4)
            wrongBeside += zip(a, b).filter { $0 != $1 }.count
        }
        let expected = try await gpu.read(reference, bytesPerPixel: 4)
        guard plane.surface.lock(options: .readOnly, seed: nil) == 0 else {
            throw TestFailure("IOSurfaceLock for the readback failed")
        }
        defer { plane.surface.unlock(options: .readOnly, seed: nil) }
        // the plain run's 4x resolve into a private texture nothing reads as data takes the 2D engine, within one code
        // of Metal (resolve_rule.h resolve.twod); a turned target's takes the exact resolve pass
        let slack = cell == .resolve ? 1 : 0
        var wrong = 0
        for y in 0 ..< h {
            for x in 0 ..< w {
                let (px, py) = stored(x, y)
                let at = plane.surface.baseAddress + py * plane.rowBytes + px * 4
                let got = UnsafeRawBufferPointer(start: at, count: 4)
                if !zip(got, expected.pixel(x, y, width: w)).allSatisfy({ abs(Int($0) - Int($1)) <= slack }) {
                    wrong += 1
                }
            }
        }
        return Result(path: path, referencePath: referencePath, wrong: wrong + wrongBeside)
    }

    private func fill(_ plane: Plane, _ byte: UInt8) throws {
        guard plane.surface.lock(options: [], seed: nil) == 0 else { throw TestFailure("IOSurfaceLock failed") }
        memset(plane.surface.baseAddress, Int32(byte), plane.rowBytes * plane.height)
        plane.surface.unlock(options: [], seed: nil)
    }

    public typealias Pass = (MTLCommandBuffer, MTLTexture) -> Void

    private func passes(_ cell: RotatedCell) -> [Pass] {
        switch cell {
        case .clear:
            return [encode(load: .clear) { _ in }]
        case .draw:
            return [encode(load: .clear, drawn)]
        case .load:
            return [
                encode(load: .clear, drawn),
                encode(load: .load) { e in
                    shapes(e, rect(0, 0, width, height), [1, 1, 1, 1], states.discarded)
                    shapes(e, rect(10, 30, 40, 50), [0, 1, 1, 1], states.plain)
                },
            ]
        case .idleLoad:
            return [encode(load: .clear, drawn), encode(load: .load) { _ in }]
        case .partLoad:
            return [encode(load: .clear, drawn), partLoad()]
        case .pointLoad:
            return [encode(load: .clear, drawn), pointLoad()]
        case .edgeLoad:
            return [encode(load: .clear, drawn), edgeLoad()]
        case .besideR4:
            return [besideR4, besideR4Load]
        case .points:
            return [
                encode(load: .clear) { e in
                    let centres = [(5, 5), (20, 40), (90, 10), (50, 60)]
                    let points = centres.map { c in
                        SIMD4<Float>(
                            (Float(c.0) + 0.5) / Float(width) * 2 - 1,
                            1 - (Float(c.1) + 0.5) / Float(height) * 2, 0.5, 1
                        )
                    }
                    shapes(e, points, [1, 1, 0, 1], states.plain, primitive: .point, perColor: points.count)
                },
            ]
        case .fetch:
            return [
                encode(load: .clear) { e in
                    shapes(e, rect(4, 2, 40, 30), [1, 0, 0, 1], states.plain)
                    shapes(e, rect(20, 10, 70, 50), [0, 0, 1, 1], states.fetch)
                },
            ]
        case .position:
            return [encode(load: .clear) { e in shapes(e, rect(0, 0, 96, 64), [1, 1, 1, 1], states.position) }]
        case .derivative:
            return [encode(load: .clear) { e in shapes(e, rect(0, 0, 96, 64), [1, 1, 1, 1], states.derivative) }]
        case .fwidth:
            return [encode(load: .clear) { e in shapes(e, rect(0, 0, 96, 64), [1, 1, 1, 1], states.fwidth) }]
        case .depth:
            return [depthTested]
        case .resolve:
            return [resolved]
        case .beside:
            return [besideStored]
        case .keptDepth:
            return [keptDepth]
        case .turns:
            return mixedTurns(larger: false)
        case .frames:
            return mixedTurns(larger: true)
        case .sized:
            return mixedSizes()
        case .keptStencil:
            return [keptStencil]
        case .keptPair:
            return [keptPair(.depth32Float)]
        case .keptPair16:
            return [keptPair(.depth16Unorm)]
        case .tie:
            return [
                encode(load: .clear) { e in shapes(e, halfPixelRect(10.5, 5.5, 30.5, 20.5), [1, 1, 1, 1], states.plain)
                },
            ]
        }
    }

    /// Two rectangles, a scissored quad, and a front-facing and a back-facing rectangle under back-face culling
    private func drawn(_ e: MTLRenderCommandEncoder) {
        shapes(e, rect(4, 2, 20, 10), [1, 0, 0, 1], states.plain)
        shapes(e, rect(30, 40, 90, 60), [0, 1, 0, 1], states.plain)
        e.setScissorRect(MTLScissorRect(x: 50, y: 5, width: 20, height: 20))
        shapes(e, rect(0, 0, 96, 64), [0, 0, 1, 1], states.plain)
        e.setScissorRect(MTLScissorRect(x: 0, y: 0, width: 96, height: 64))
        e.setCullMode(.back)
        shapes(e, rect(72, 2, 92, 18), [1, 1, 0, 1], states.plain)
        shapes(e, rect(72, 22, 92, 36, reversed: true), [1, 0, 1, 1], states.plain)
        e.setCullMode(.none)
    }

    /// Three rectangles over a private depth texture of the frame, cleared and never stored: the nearest one wins where
    /// they overlap, whichever order they come in
    private func depthTested(_ commands: MTLCommandBuffer, _ target: MTLTexture) {
        guard let depth = try? gpu.texture(.depth32Float, width, height, usage: .renderTarget) else { return }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        pass.depthAttachment.texture = depth
        pass.depthAttachment.loadAction = .clear
        pass.depthAttachment.clearDepth = 1
        pass.depthAttachment.storeAction = .dontCare
        let e = commands.makeRenderCommandEncoder(descriptor: pass)!
        e.setDepthStencilState(states.lessEqual)
        let at = { (r: [SIMD4<Float>], z: Float) in r.map { SIMD4<Float>($0.x, $0.y, z, 1) } }
        shapes(e, at(rect(4, 2, 60, 40), 0.7), [1, 0, 0, 1], states.depthTested)
        shapes(e, at(rect(30, 20, 90, 60), 0.3), [0, 0, 1, 1], states.depthTested)
        shapes(e, at(rect(20, 10, 70, 50), 0.5), [0, 1, 0, 1], states.depthTested)
        e.endEncoding()
    }

    /// The draws into the target and, inverted, into a private texture of the frame beside it, stored: both runs' private
    /// textures must agree byte for byte
    private func besideStored(_ commands: MTLCommandBuffer, _ target: MTLTexture) {
        guard let beside = try? gpu.texture(.bgra8Unorm, width, height) else { return }
        self.beside.textures.append(beside)
        let pass = MTLRenderPassDescriptor()
        for (i, texture) in [target, beside].enumerated() {
            pass.colorAttachments[i].texture = texture
            pass.colorAttachments[i].loadAction = .clear
            pass.colorAttachments[i].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
            pass.colorAttachments[i].storeAction = .store
        }
        let e = commands.makeRenderCommandEncoder(descriptor: pass)!
        shapes(e, rect(4, 2, 60, 40), [1, 0, 0, 1], states.pair)
        shapes(e, rect(30, 20, 90, 60), [0, 1, 0, 1], states.pair)
        e.endEncoding()
    }

    /// A depth-only pass of the frame's private depth texture, stored, then a pass into the target loading it: the nearer
    /// rectangle of the first pass hides the second's where they overlap
    private func keptDepth(_ commands: MTLCommandBuffer, _ target: MTLTexture) {
        guard let depth = try? gpu.texture(.depth32Float, width, height, usage: .renderTarget),
              let depthOnly = try? gpu.render(
                  vertex: "rotated_vertex", fragment: "rotated_fragment", format: .invalid, depth: .depth32Float
              )
        else { return }
        let at = { (r: [SIMD4<Float>], z: Float) in r.map { SIMD4<Float>($0.x, $0.y, z, 1) } }
        let first = MTLRenderPassDescriptor()
        first.depthAttachment.texture = depth
        first.depthAttachment.loadAction = .clear
        first.depthAttachment.clearDepth = 1
        first.depthAttachment.storeAction = .store
        let e = commands.makeRenderCommandEncoder(descriptor: first)!
        e.setDepthStencilState(states.lessEqual)
        shapes(e, at(rect(10, 5, 50, 45), 0.25), [0, 0, 0, 1], depthOnly)
        e.endEncoding()
        let second = MTLRenderPassDescriptor()
        second.colorAttachments[0].texture = target
        second.colorAttachments[0].loadAction = .clear
        second.colorAttachments[0].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
        second.colorAttachments[0].storeAction = .store
        second.depthAttachment.texture = depth
        second.depthAttachment.loadAction = .load
        second.depthAttachment.storeAction = .dontCare
        let f = commands.makeRenderCommandEncoder(descriptor: second)!
        f.setDepthStencilState(states.lessEqual)
        shapes(f, at(rect(20, 10, 80, 55), 0.5), [1, 1, 0, 1], states.depthTested)
        f.endEncoding()
    }

    /// A pass into the target writing a private Depth32Float_Stencil8 texture of the frame beside it, stored: stencil 0xA5 at
    /// depth 0.25 in one rectangle, 0x5A at 0.5 in another behind it. A pass into the target loading it draws red where the
    /// stencil is 0xA5, green where 0x5A, then blue alone where the depth is past 0.375
    private func keptStencil(_ commands: MTLCommandBuffer, _ target: MTLTexture) {
        let zs = MTLPixelFormat.depth32Float_stencil8
        guard let zeta = try? gpu.texture(zs, width, height, usage: .renderTarget),
              let tested = try? gpu.render(
                  vertex: "rotated_vertex", fragment: "rotated_fragment", format: .bgra8Unorm, depth: zs, stencil: zs
              ),
              let blue = try? gpu.render(
                  vertex: "rotated_vertex", fragment: "rotated_fragment", format: .bgra8Unorm, depth: zs, stencil: zs,
                  configure: { $0.colorAttachments[0].writeMask = .blue }
              ),
              let writes = zetaState(.lessEqual, writes: true, stencil: .always, replace: true),
              let equal = zetaState(.always, writes: false, stencil: .equal, replace: false),
              let beyond = zetaState(.lessEqual, writes: false, stencil: .always, replace: false)
        else { return }
        let at = { (r: [SIMD4<Float>], z: Float) in r.map { SIMD4<Float>($0.x, $0.y, z, 1) } }
        let whole = rect(0, 0, width, height)
        for (n, load) in [MTLLoadAction.clear, .load].enumerated() {
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = zeta
            pass.depthAttachment.loadAction = load
            pass.depthAttachment.clearDepth = 1
            pass.depthAttachment.storeAction = n == 0 ? .store : .dontCare
            pass.stencilAttachment.texture = zeta
            pass.stencilAttachment.loadAction = load
            pass.stencilAttachment.clearStencil = 0
            pass.stencilAttachment.storeAction = n == 0 ? .store : .dontCare
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            if n == 0 {
                e.setDepthStencilState(writes)
                e.setStencilReferenceValue(0xA5)
                shapes(e, at(rect(10, 5, 50, 45), 0.25), [1, 1, 0, 1], tested)
                e.setStencilReferenceValue(0x5A)
                shapes(e, at(rect(20, 10, 80, 55), 0.5), [0, 1, 1, 1], tested)
            } else {
                e.setDepthStencilState(equal)
                e.setStencilReferenceValue(0xA5)
                shapes(e, at(whole, 0.9), [1, 0, 0, 1], tested)
                e.setStencilReferenceValue(0x5A)
                shapes(e, at(whole, 0.9), [0, 1, 0, 1], tested)
                e.setDepthStencilState(beyond)
                shapes(e, at(whole, 0.375), [0, 0, 1, 1], blue)
            }
            e.endEncoding()
        }
    }

    private func zetaState(
        _ depth: MTLCompareFunction, writes: Bool, stencil: MTLCompareFunction, replace: Bool
    ) -> MTLDepthStencilState? {
        let face = MTLStencilDescriptor()
        face.stencilCompareFunction = stencil
        face.depthStencilPassOperation = replace ? .replace : .keep
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = depth
        d.isDepthWriteEnabled = writes
        d.frontFaceStencil = face
        d.backFaceStencil = face
        return gpu.device.makeDepthStencilState(descriptor: d)
    }

    /// The draws into a 4x private texture of the frame, resolved into the target
    private func resolved(_ commands: MTLCommandBuffer, _ target: MTLTexture) {
        guard let samples = try? gpu.texture(.bgra8Unorm, width, height, usage: .renderTarget, samples: 4) else {
            return
        }
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = samples
        pass.colorAttachments[0].resolveTexture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
        pass.colorAttachments[0].storeAction = .multisampleResolve
        let e = commands.makeRenderCommandEncoder(descriptor: pass)!
        shapes(e, rect(4, 2, 40, 30), [1, 0, 0, 1], states.multisampled)
        shapes(e, halfPixelRect(20.3, 10.55, 70.7, 50.35), [0, 0, 1, 1], states.multisampled)
        e.endEncoding()
    }

    /// keptStencil's two passes over a depth texture of `depthFormat` and a Stencil8 texture of the frame
    func keptPair(_ depthFormat: MTLPixelFormat) -> Pass {
        { commands, target in
            guard let depth = try? gpu.texture(depthFormat, width, height, usage: .renderTarget),
                  let stencil = try? gpu.texture(.stencil8, width, height, usage: .renderTarget),
                  let tested = try? gpu.render(
                      vertex: "rotated_vertex", fragment: "rotated_fragment", format: .bgra8Unorm, depth: depthFormat,
                      stencil: .stencil8
                  ),
                  let blue = try? gpu.render(
                      vertex: "rotated_vertex", fragment: "rotated_fragment", format: .bgra8Unorm, depth: depthFormat,
                      stencil: .stencil8, configure: { $0.colorAttachments[0].writeMask = .blue }
                  ),
                  let writes = zetaState(.lessEqual, writes: true, stencil: .always, replace: true),
                  let equal = zetaState(.always, writes: false, stencil: .equal, replace: false),
                  let beyond = zetaState(.lessEqual, writes: false, stencil: .always, replace: false)
            else { return }
            let at = { (r: [SIMD4<Float>], z: Float) in r.map { SIMD4<Float>($0.x, $0.y, z, 1) } }
            let whole = rect(0, 0, width, height)
            for (n, load) in [MTLLoadAction.clear, .load].enumerated() {
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
                pass.colorAttachments[0].storeAction = .store
                pass.depthAttachment.texture = depth
                pass.depthAttachment.loadAction = load
                pass.depthAttachment.clearDepth = 1
                pass.depthAttachment.storeAction = n == 0 ? .store : .dontCare
                pass.stencilAttachment.texture = stencil
                pass.stencilAttachment.loadAction = load
                pass.stencilAttachment.clearStencil = 0
                pass.stencilAttachment.storeAction = n == 0 ? .store : .dontCare
                let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                if n == 0 {
                    e.setDepthStencilState(writes)
                    e.setStencilReferenceValue(0xA5)
                    shapes(e, at(rect(10, 5, 50, 45), 0.25), [1, 1, 0, 1], tested)
                    e.setStencilReferenceValue(0x5A)
                    shapes(e, at(rect(20, 10, 80, 55), 0.5), [0, 1, 1, 1], tested)
                } else {
                    e.setDepthStencilState(equal)
                    e.setStencilReferenceValue(0xA5)
                    shapes(e, at(whole, 0.9), [1, 0, 0, 1], tested)
                    e.setStencilReferenceValue(0x5A)
                    shapes(e, at(whole, 0.9), [0, 1, 0, 1], tested)
                    e.setDepthStencilState(beyond)
                    shapes(e, at(whole, 0.375), [0, 0, 1, 1], blue)
                }
                e.endEncoding()
            }
        }
    }

    public func encode(load: MTLLoadAction, _ draw: @escaping (MTLRenderCommandEncoder) -> Void) -> Pass {
        { commands, target in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = load
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            draw(encoder)
            encoder.endEncoding()
        }
    }

    public func rect(_ x0: Int, _ y0: Int, _ x1: Int, _ y1: Int, reversed: Bool = false) -> [SIMD4<Float>] {
        let c = GPU.pixelQuad(
            x0, y0, x1, y1, width: width, height: height
        )
        return reversed ? [c[0], c[2], c[1], c[1], c[2], c[3]] : [c[0], c[1], c[2], c[1], c[3], c[2]]
    }

    /// A rectangle whose edges run through pixel centres, where the rasteriser's tie rule decides
    private func halfPixelRect(_ x0: Float, _ y0: Float, _ x1: Float, _ y1: Float) -> [SIMD4<Float>] {
        let fx = { (x: Float) in x / Float(width) * 2 - 1 }
        let fy = { (y: Float) in 1 - y / Float(height) * 2 }
        let c = GPU.quad(x0: fx(x0), y0: fy(y0), x1: fx(x1), y1: fy(y1), z: 0.5)
        return [c[0], c[1], c[2], c[1], c[3], c[2]]
    }

    public func shapes(
        _ e: MTLRenderCommandEncoder, _ corners: [SIMD4<Float>], _ color: SIMD4<Float>,
        _ state: MTLRenderPipelineState, primitive: MTLPrimitiveType = .triangle, perColor: Int = 6
    ) {
        var corners = corners
        var colors = [color]
        var per = UInt32(perColor)
        e.setRenderPipelineState(state)
        e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
        e.setVertexBytes(&colors, length: MemoryLayout<SIMD4<Float>>.stride, index: 1)
        e.setVertexBytes(&per, length: 4, index: 2)
        e.drawPrimitives(type: primitive, vertexStart: 0, vertexCount: corners.count)
    }
}
