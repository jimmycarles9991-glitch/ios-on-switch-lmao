import CoreGraphics
import Foundation
import HWTestCore
import Metal

/// An icon's mask the way iconservicesagent draws the App Library's icons: RenderBox (RBDevice over this process's
/// Metal device, RBDrawable into a BGRA8 texture) fills a white continuous-corner rounded rectangle. RenderBox draws it
/// as one indexed triangle strip of four through its uber_fragment, which computes the corner's coverage and blends by
/// fetching the colours it writes (traced on a Mac: uber_vertex with a stitched fragment, colours BGRA8, RG32Float and
/// BGRA8, Depth32Float_Stencil8, blending off). The reference is the same display list drawn by RenderBox on the CPU
/// (-[RBDisplayList renderInContext:options:]). A cell's `beyond` counts the corner pixels whose alpha is more than
/// `tolerance` codes from the reference (RenderBox's GPU and CPU differ by up to 36 on an M4 Max), `partial` the corner
/// pixels between 8 and 247, which an antialiased corner has and a stepped one lacks.
public enum RenderBoxCorner {
    public static let tolerance = 64

    public struct Cell: Sendable {
        public let side: Int
        public let storage: String
        public var beyond = 0, maxDiff = 0, partial = 0, referencePartial = 0
    }

    /// The classes and selectors the cells use, all present
    public static var available: Bool {
        let selectors: [(String, [String])] = [
            ("RBDevice", ["initWithDevice:"]),
            (
                "RBDrawable",
                [
                    "initWithDevice:", "setPixelFormat:", "setTexture:", "setSize:", "setScale:",
                    "setCompletedHandler:",
                    "renderDisplayList:flags:", "finish",
                ]
            ),
            ("RBDisplayList", ["drawShape:fill:alpha:blendMode:", "renderInContext:options:"]),
            ("RBShape", ["setRoundedRect:cornerRadius:cornerStyle:"]),
            ("RBFill", ["setCGColor:"]),
        ]
        return selectors.allSatisfy { name, methods in
            guard let type = NSClassFromString(name) as? NSObject.Type else { return false }
            return methods.allSatisfy { type.instancesRespond(to: NSSelectorFromString($0)) }
        }
    }

    /// How long a drawing may take to complete before the cell reports it never did
    static let completionLimit: TimeInterval = 5

    /// RenderBox's own drawing of a `side`-pixel icon mask with corner radius `radius` into a texture of `storage` on
    /// `gpu`'s device, against its CPU drawing
    public static func cell(_ gpu: GPU, side: Int, radius: Double, storage: MTLStorageMode) async throws -> Cell {
        let list = try displayList(side: side, radius: radius)
        let drawn = try await draw(gpu, list, side: side, storage: storage)
        return judge(
            drawn, cpuAlpha(list, side: side), side: side, radius: radius,
            storage: storage == .private ? "private" : "shared")
    }

    /// RenderBox's CPU drawing of the mask, its alpha by row
    public static func continuousMask(side: Int, radius: Double) -> [UInt8] {
        guard let list = try? displayList(side: side, radius: radius) else { return [] }
        return cpuAlpha(list, side: side)
    }

    /// `drawn`'s corners against `reference`'s, both alpha by row
    public static func judge(
        _ drawn: [UInt8], _ reference: [UInt8], side: Int, radius: Double, storage: String = ""
    )
        -> Cell
    {
        var cell = Cell(side: side, storage: storage)
        let corner = Int((radius * 1.5).rounded(.up)) + 1
        for (y, x) in cornerPixels(side: side, corner: corner) {
            let i = y * side + x
            let got = Int(drawn[i]), want = Int(reference[i])
            cell.maxDiff = max(cell.maxDiff, abs(got - want))
            if abs(got - want) > tolerance { cell.beyond += 1 }
            if (8...247).contains(got) { cell.partial += 1 }
            if (8...247).contains(want) { cell.referencePartial += 1 }
        }
        return cell
    }

    /// The pixels of the four `corner`-square corners of a `side` square, row and column
    public static func cornerPixels(side: Int, corner: Int) -> [(Int, Int)] {
        let c = min(corner, side / 2)
        var out: [(Int, Int)] = []
        for y in 0..<side where y < c || y >= side - c {
            for x in 0..<side where x < c || x >= side - c { out.append((y, x)) }
        }
        return out
    }

    /// Alpha of a `from`-pixel square image averaged over the area each pixel of a `to`-pixel square covers
    public static func reduce(_ alpha: [Double], from: Int, to: Int) -> [Double] {
        let scale = Double(from) / Double(to)
        var out = [Double](repeating: 0, count: to * to)
        for y in 0..<to {
            for x in 0..<to {
                let x0 = Double(x) * scale, x1 = x0 + scale, y0 = Double(y) * scale, y1 = y0 + scale
                var sum = 0.0
                for sy in Int(y0)..<min(from, Int(y1.rounded(.up))) {
                    let hy = min(y1, Double(sy + 1)) - max(y0, Double(sy))
                    for sx in Int(x0)..<min(from, Int(x1.rounded(.up))) {
                        let hx = min(x1, Double(sx + 1)) - max(x0, Double(sx))
                        sum += alpha[sy * from + sx] * hx * hy
                    }
                }
                out[y * to + x] = sum / (scale * scale)
            }
        }
        return out
    }

    typealias Init = @convention(c) (AnyObject, Selector) -> AnyObject
    typealias InitObject = @convention(c) (AnyObject, Selector, AnyObject) -> AnyObject
    typealias SetRoundedRect = @convention(c) (AnyObject, Selector, CGRect, Double, Int32) -> Void
    typealias SetCGColor = @convention(c) (AnyObject, Selector, CGColor) -> Void
    typealias DrawShape = @convention(c) (AnyObject, Selector, AnyObject, AnyObject, Float, Int32) -> Void
    typealias SetObject = @convention(c) (AnyObject, Selector, AnyObject?) -> Void
    typealias SetUInt = @convention(c) (AnyObject, Selector, UInt) -> Void
    typealias SetSize = @convention(c) (AnyObject, Selector, CGSize) -> Void
    typealias SetDouble = @convention(c) (AnyObject, Selector, Double) -> Void
    typealias Render = @convention(c) (AnyObject, Selector, AnyObject, UInt32) -> Void
    typealias Plain = @convention(c) (AnyObject, Selector) -> Void
    typealias RenderInContext = @convention(c) (AnyObject, Selector, CGContext, AnyObject?) -> Void

    static func method<T>(_ object: AnyObject, _ name: String, _: T.Type) throws -> (T, Selector) {
        let selector = NSSelectorFromString(name)
        guard object.responds(to: selector), let imp = object.method(for: selector) else {
            throw TestFailure("RenderBox lacks -\(name)")
        }
        return (unsafeBitCast(imp, to: T.self), selector)
    }

    static func make(_ name: String) throws -> AnyObject {
        guard let type = NSClassFromString(name) as? NSObject.Type else { throw TestFailure("no \(name)") }
        return type.init()
    }

    static func displayList(side: Int, radius: Double) throws -> AnyObject {
        let list = try make("RBDisplayList"), shape = try make("RBShape"), fill = try make("RBFill")
        let (rounded, roundedSel) = try method(shape, "setRoundedRect:cornerRadius:cornerStyle:", SetRoundedRect.self)
        rounded(shape, roundedSel, CGRect(x: 0, y: 0, width: side, height: side), radius, 1)
        let (color, colorSel) = try method(fill, "setCGColor:", SetCGColor.self)
        color(fill, colorSel, CGColor(red: 1, green: 1, blue: 1, alpha: 1))
        let (draw, drawSel) = try method(list, "drawShape:fill:alpha:blendMode:", DrawShape.self)
        draw(list, drawSel, shape, fill, 1, 0)
        return list
    }

    static func draw(_ gpu: GPU, _ list: AnyObject, side: Int, storage: MTLStorageMode) async throws -> [UInt8] {
        guard let deviceType = NSClassFromString("RBDevice") as? NSObject.Type,
            let drawableType = NSClassFromString("RBDrawable") as? NSObject.Type
        else { throw TestFailure("no RBDevice") }
        let initWith = NSSelectorFromString("initWithDevice:")
        let rbDevice = unsafeBitCast(deviceType.instanceMethod(for: initWith), to: InitObject.self)(
            deviceType.perform(NSSelectorFromString("alloc")).takeUnretainedValue(), initWith, gpu.device)
        let drawable = unsafeBitCast(drawableType.instanceMethod(for: initWith), to: InitObject.self)(
            drawableType.perform(NSSelectorFromString("alloc")).takeUnretainedValue(), initWith, rbDevice)
        let target = try gpu.texture(.bgra8Unorm, side, side, storage: storage)
        let (format, formatSel) = try method(drawable, "setPixelFormat:", SetUInt.self)
        format(drawable, formatSel, MTLPixelFormat.bgra8Unorm.rawValue)
        let (texture, textureSel) = try method(drawable, "setTexture:", SetObject.self)
        texture(drawable, textureSel, target)
        let (size, sizeSel) = try method(drawable, "setSize:", SetSize.self)
        size(drawable, sizeSel, CGSize(width: side, height: side))
        let (scale, scaleSel) = try method(drawable, "setScale:", SetDouble.self)
        scale(drawable, scaleSel, 1)
        let (completed, completedSel) = try method(drawable, "setCompletedHandler:", SetObject.self)
        let (render, renderSel) = try method(drawable, "renderDisplayList:flags:", Render.self)
        let (finish, finishSel) = try method(drawable, "finish", Plain.self)
        let once = Once()
        let completedInTime = await withCheckedContinuation { (done: CheckedContinuation<Bool, Never>) in
            let handler: @convention(block) () -> Void = { if once.first() { done.resume(returning: true) } }
            completed(drawable, completedSel, handler as AnyObject)
            render(drawable, renderSel, list, 0)
            finish(drawable, finishSel)
            DispatchQueue.global().asyncAfter(deadline: .now() + completionLimit) {
                if once.first() { done.resume(returning: false) }
            }
        }
        guard completedInTime else { throw TestFailure("RenderBox's drawing never completed") }
        let bytes = try await gpu.read(target, bytesPerPixel: 4)
        return (0..<(side * side)).map { bytes[$0 * 4 + 3] }
    }

    /// True for the first caller only, whichever thread it is on
    final class Once: @unchecked Sendable {
        private let lock = NSLock()
        private var taken = false

        func first() -> Bool {
            lock.lock()
            defer { lock.unlock() }
            let was = taken
            taken = true
            return !was
        }
    }

    static func cpuAlpha(_ list: AnyObject, side: Int) -> [UInt8] {
        var bytes = [UInt8](repeating: 0, count: side * side * 4)
        bytes.withUnsafeMutableBytes { raw in
            guard
                let context = CGContext(
                    data: raw.baseAddress, width: side, height: side, bitsPerComponent: 8, bytesPerRow: side * 4,
                    space: CGColorSpaceCreateDeviceRGB(),
                    bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue),
                let (draw, selector) = try? method(list, "renderInContext:options:", RenderInContext.self)
            else { return }
            draw(list, selector, context, nil)
        }
        return (0..<(side * side)).map { bytes[$0 * 4 + 3] }
    }
}
