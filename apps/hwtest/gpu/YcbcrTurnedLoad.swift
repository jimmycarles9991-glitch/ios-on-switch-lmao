import Foundation
import HWTestCore
import IOSurface
import Metal

/// gpu.gm20b.ycbcr_turned_load: a YCbCr 4:2:0 (500) target turned by rotation 1, 2 or 3 over plane 0 of a '420v' 128x64
/// (luma rows 128 at 0, chroma pairs 64x32 rows 128 at 8192), every byte SurfaceReads.pattern(i), drawn by a pass that
/// loads and stores it, one draw of a frame rectangle whose fragments are all discarded: what the store writes back of
/// the frame tiles the draw reaches, in plane bytes
public enum YcbcrTurnedLoad {
    public struct Box: Equatable, Sendable, CustomStringConvertible {
        public let x0, y0, x1, y1: Int

        public init(_ x0: Int, _ y0: Int, _ x1: Int, _ y1: Int) {
            (self.x0, self.y0, self.x1, self.y1) = (x0, y0, x1, y1)
        }

        func holds(_ x: Int, _ y: Int) -> Bool { x >= x0 && x <= x1 && y >= y0 && y <= y1 }

        public var description: String { "(\(x0),\(y0))-(\(x1),\(y1))" }
    }

    public struct Case: Sendable {
        public let key: String
        public let rotation: Int
        public let frameWidth, frameHeight: Int
        /// The draw's rectangle in frame pixels, and the plane box an M4 Max stores back for it
        public let rect: Box
        public let box: Box
    }

    public static let width = 128, height = 64, chromaOffset = 8192

    /// Rotations 1 to 3 and the plane box an M4 Max stores back for each
    public static let cases: [Case] = [
        Case(
            key: "r1_64x128", rotation: 1, frameWidth: 64, frameHeight: 128, rect: Box(0, 0, 4, 4),
            box: Box(96, 0, 127, 31)
        ),
        Case(
            key: "r1_56x120", rotation: 1, frameWidth: 56, frameHeight: 120, rect: Box(0, 0, 4, 4),
            box: Box(88, 0, 119, 31)
        ),
        Case(
            key: "r1_56x120_mid", rotation: 1, frameWidth: 56, frameHeight: 120, rect: Box(40, 40, 44, 44),
            box: Box(56, 32, 87, 55)
        ),
        Case(
            key: "r2_128x64", rotation: 2, frameWidth: 128, frameHeight: 64, rect: Box(0, 0, 4, 4),
            box: Box(96, 32, 127, 63)
        ),
        Case(
            key: "r2_120x56", rotation: 2, frameWidth: 120, frameHeight: 56, rect: Box(0, 0, 4, 4),
            box: Box(88, 24, 119, 55)
        ),
        Case(
            key: "r2_120x56_mid", rotation: 2, frameWidth: 120, frameHeight: 56, rect: Box(40, 40, 44, 44),
            box: Box(56, 0, 87, 23)
        ),
        Case(
            key: "r3_64x128", rotation: 3, frameWidth: 64, frameHeight: 128, rect: Box(0, 0, 4, 4),
            box: Box(0, 32, 31, 63)
        ),
        Case(
            key: "r3_56x120", rotation: 3, frameWidth: 56, frameHeight: 120, rect: Box(0, 0, 4, 4),
            box: Box(0, 24, 31, 55)
        ),
        Case(
            key: "r3_56x120_mid", rotation: 3, frameWidth: 56, frameHeight: 120, rect: Box(40, 40, 44, 44),
            box: Box(32, 0, 63, 23)
        ),
    ]

    public struct Reading: Sendable {
        public var made = false
        public var status = "-"
        /// Luma bytes that changed, their bounding box, those outside the case's box; chroma bytes that changed and
        /// those outside the box's chroma rows and bytes; bytes past both planes that changed
        public var lumaChanged = 0
        public var changedBox: Box?
        public var lumaOutside = 0
        public var chromaChanged = 0
        public var chromaOutside = 0
        public var tailChanged = 0
    }

    public static func observe(_ gpu: GPU, _ c: Case) async throws -> Reading {
        let planes = [
            SurfaceReads.Plane(at: 0, width, height, bpe: 1, rows: width),
            SurfaceReads.Plane(at: chromaOffset, width / 2, height / 2, bpe: 2, rows: width),
        ]
        guard
            let surface = SurfaceReads.surface(
                "420v", width, height, planes: planes, allocation: width * height * 3 / 2
            )
        else { throw TestFailure("no '420v' 128x64") }
        var reading = Reading()
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: MTLPixelFormat(rawValue: 500)!, width: c.frameWidth, height: c.frameHeight, mipmapped: false
        )
        descriptor.usage = [.renderTarget, .shaderRead]
        guard descriptor.responds(to: NSSelectorFromString("setRotation:")) else {
            throw TestFailure("MTLTextureDescriptor answers no setRotation:")
        }
        descriptor.setValue(NSNumber(value: c.rotation), forKey: "rotation")
        descriptor.setValue(NSNumber(value: true), forKey: "framebufferOnly")
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            return reading
        }
        reading.made = true
        let before = SurfaceReads.bytes(surface)
        reading.status = try await loaded(gpu, texture, c)
        guard !reading.status.hasPrefix("error:") else { return reading }
        let after = SurfaceReads.bytes(surface)
        var x0 = Int.max, y0 = Int.max, x1 = -1, y1 = -1
        for y in 0 ..< height {
            for x in 0 ..< width where after[y * width + x] != before[y * width + x] {
                reading.lumaChanged += 1
                if !c.box.holds(x, y) { reading.lumaOutside += 1 }
                (x0, y0, x1, y1) = (min(x0, x), min(y0, y), max(x1, x), max(y1, y))
            }
        }
        if x1 >= 0 { reading.changedBox = Box(x0, y0, x1, y1) }
        let chroma = Box(c.box.x0 & ~1, c.box.y0 / 2, c.box.x1 | 1, c.box.y1 / 2)
        for y in 0 ..< (height / 2) {
            for x in 0 ..< width where after[chromaOffset + y * width + x] != before[chromaOffset + y * width + x] {
                reading.chromaChanged += 1
                if !chroma.holds(x, y) { reading.chromaOutside += 1 }
            }
        }
        let end = chromaOffset + width * height / 2
        reading.tailChanged = (end ..< after.count).filter { after[$0] != before[$0] }.count
        return reading
    }

    static func loaded(_ gpu: GPU, _ texture: MTLTexture, _ c: Case) async throws -> String {
        let pipeline = try gpu.render(
            vertex: "surface_stores_rect", fragment: "ycbcr_turned_load_discard", format: texture.pixelFormat
        )
        let w = Float(c.frameWidth), h = Float(c.frameHeight)
        var rect = SIMD4<Float>(
            Float(c.rect.x0) / w * 2 - 1, 1 - Float(c.rect.y0) / h * 2, Float(c.rect.x1) / w * 2 - 1,
            1 - Float(c.rect.y1) / h * 2
        )
        return try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            encoder.setVertexBytes(&rect, length: 16, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }.path
    }
}
