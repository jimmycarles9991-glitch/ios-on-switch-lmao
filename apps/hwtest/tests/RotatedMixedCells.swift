import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// gpu.x1.rotated_target's passes whose attachments beside the turned target are turned another way, of another frame, or
/// not turned and of another size. Metal accepts each (measured on an M4 Max, raw/runtime/raw_area_load.h): the render area,
/// default viewport and scissor are the attachments' intersection, and every attachment lands by its own store map. A turned
/// attachment's Load reads its plane as a plane of the render area's frame would be read, so where its frame is past the area
/// each texel the pass stores is the frame's texel past it; the references load so ahead of those passes (loadAsMetal).
extension RotatedCells {
    /// The second plane's turn: never the target's (rotation 4 stores as rotation 1)
    var secondTurn: Int { turn == 2 ? 3 : 2 }

    var sizes: [(Int, Int)] { [(width + 32, height + 16), (width - 32, height - 24)] }

    /// A second plane of another turn, of the target's frame or a larger one, beside the target (in the plain run, a texture
    /// of that frame), cleared and drawn, then loaded and drawn again beside the target cleared again; the plane must hold the
    /// texture's bytes by its own store map
    func mixedTurns(larger: Bool) -> [Pass] {
        [{ secondCleared($0, $1, larger: larger) }, secondLoaded]
    }

    private func secondCleared(_ commands: MTLCommandBuffer, _ target: MTLTexture, larger: Bool) {
        let w = width + (larger ? 32 : 0), h = height + (larger ? 16 : 0)
        let second: MTLTexture
        if beside.turnedRun {
            let cells = RotatedCells(
                gpu: gpu, states: states, turn: secondTurn, width: w, height: h, aligned: aligned)
            guard let plane = try? cells.makePlane() else { return }
            fillSurface(plane, 0x5a)
            beside.second = (plane, cells)
            second = plane.texture
        } else {
            guard let texture = filled(w, h, 0x5a) else { return }
            beside.plainSecond = texture
            second = texture
        }
        pair(commands, target, second, load: .clear) { e in
            shapes(e, rect(4, 2, 60, 40), [1, 0, 0, 1], states.pair)
        }
    }

    // the target cleared again: a rotation-4 target's Load is the load cell's, which reads it transposed
    private func secondLoaded(_ commands: MTLCommandBuffer, _ target: MTLTexture) {
        guard let second = beside.turnedRun ? beside.second?.plane.texture : beside.plainSecond else { return }
        pair(commands, target, second, load: .load, targetLoad: .clear) { e in
            shapes(e, rect(30, 20, 90, 60), [0, 1, 0, 1], states.pair)
        }
    }

    /// Private textures larger and smaller than the frame beside the target, each cleared and drawn, then loaded and drawn
    /// again: both runs' textures must agree byte for byte
    func mixedSizes() -> [Pass] {
        sizes.indices.flatMap { i in [{ sizedCleared($0, $1, i) }, { sizedLoaded($0, $1, i) }] as [Pass] }
    }

    private func sizedCleared(_ commands: MTLCommandBuffer, _ target: MTLTexture, _ i: Int) {
        let (w, h) = sizes[i]
        guard let texture = filled(w, h, 0x33) else { return }
        // run() compares beside.textures[k] with [k + half]: the plain run's of each size first, in size order
        if beside.turnedRun { beside.textures.append(texture) } else { beside.textures.insert(texture, at: i) }
        pair(commands, target, texture, load: .clear) { e in
            shapes(e, rect(4, 2, 40, 30), [1, 0, 0, 1], states.pair)
        }
    }

    private func sizedLoaded(_ commands: MTLCommandBuffer, _ target: MTLTexture, _ i: Int) {
        guard let texture = beside.turnedRun ? beside.textures.last : beside.textures[i] else { return }
        pair(commands, target, texture, load: .load) { e in
            shapes(e, sizedLoadRect, [0, 0, 1, 1], states.pair)
        }
    }

    private var sizedLoadRect: [SIMD4<Float>] { rect(20, 10, 70, 50) }

    /// The references ahead of pass `n` of a cell, loaded as Metal loads the turned attachment they stand for
    func loadAsMetal(_ cell: RotatedCell, before n: Int, into reference: MTLTexture) async throws {
        switch (cell, n) {
        case (.frames, 1):
            guard let second = beside.plainSecond else { return }
            try await loadTurned(second, turn: secondTurn, area: (width, height), reached: nil)
        case (.sized, 1), (.sized, 3):
            let area = n == 1 ? (width, height) : sizes[1]
            try await loadTurned(
                reference, turn: turn, area: area, reached: reachedTiles(of: sizedLoadRect, area: area))
        case (_, 1):
            try await transposedLoad(cell, into: reference)
        default:
            break
        }
    }

    /// The tiles (y * tiles a row + x) of a `area` render area that a primitive whose clip-space corners are `corners` reaches
    /// under the default viewport: its box, far edges included, rounded out to 32x32 tiles
    func reachedTiles(of corners: [SIMD4<Float>], area: (Int, Int)) -> Set<Int> {
        let xs = corners.map { (Double($0.x) + 1) / 2 * Double(area.0) }
        let ys = corners.map { (1 - Double($0.y)) / 2 * Double(area.1) }
        let perRow = (area.0 + 31) / 32, rows = (area.1 + 31) / 32
        let tile = { (v: Double, n: Int) in min(max(Int(floor(v / 32)), 0), n - 1) }
        var out = Set<Int>()
        for ty in tile(ys.min()!, rows)...tile(ys.max()!, rows) {
            for tx in tile(xs.min()!, perRow)...tile(xs.max()!, perRow) { out.insert(ty * perRow + tx) }
        }
        return out
    }

    /// `texture` as a pass of a `area` render area loads and stores the turned attachment of its frame (`reached`: the tiles
    /// the pass stores, nil for every tile): frame texel (x, y) of the tiles over the area takes texel (min(x + dx, W - 1),
    /// min(y + dy, H - 1)), dx = W - area width for turns 2 and 3, dy = H - area height for turns 1 and 2; rotation 4's
    /// (x, H - 1 - y)
    func loadTurned(_ texture: MTLTexture, turn: Int, area: (Int, Int), reached: Set<Int>?) async throws {
        let w = texture.width, h = texture.height
        let dx = turn == 2 || turn == 3 ? w - area.0 : 0, dy = turn == 1 || turn == 2 ? h - area.1 : 0
        let perRow = (area.0 + 31) / 32
        let first = try await gpu.read(texture, bytesPerPixel: 4)
        var loaded = first
        for y in 0..<min((area.1 + 31) / 32 * 32, h) {
            for x in 0..<min((area.0 + 31) / 32 * 32, w) where reached?.contains((y / 32) * perRow + x / 32) ?? true {
                let (sx, sy) = turn == 4 ? (x, h - 1 - y) : (min(x + dx, w - 1), min(y + dy, h - 1))
                let to = (y * w + x) * 4, from = (sy * w + sx) * 4
                loaded[to..<(to + 4)] = first[from..<(from + 4)]
            }
        }
        try await gpu.upload(loaded, to: texture, bytesPerRow: w * 4)
    }

    /// The second plane's frame texels that are not the plain texture's (each through the second plane's store map)
    func secondPlaneWrong() async throws -> Int {
        guard let (plane, cells) = beside.second, let plain = beside.plainSecond else { return 0 }
        let expected = try await gpu.read(plain, bytesPerPixel: 4)
        guard plane.surface.lock(options: .readOnly, seed: nil) == 0 else {
            throw TestFailure("IOSurfaceLock for the second plane's readback failed")
        }
        defer { plane.surface.unlock(options: .readOnly, seed: nil) }
        var wrong = 0
        for y in 0..<cells.height {
            for x in 0..<cells.width {
                let (px, py) = cells.stored(x, y)
                let got = UnsafeRawBufferPointer(
                    start: plane.surface.baseAddress + py * plane.rowBytes + px * 4, count: 4)
                if !zip(got, expected.pixel(x, y, width: cells.width)).allSatisfy({ $0 == $1 }) { wrong += 1 }
            }
        }
        return wrong
    }

    func fillSurface(_ plane: Plane, _ byte: UInt8) {
        guard plane.surface.lock(options: [], seed: nil) == 0 else { return }
        memset(plane.surface.baseAddress, Int32(byte), plane.rowBytes * plane.height)
        plane.surface.unlock(options: [], seed: nil)
    }

    private func filled(_ w: Int, _ h: Int, _ byte: UInt8) -> MTLTexture? {
        guard let texture = try? gpu.texture(.bgra8Unorm, w, h, storage: .shared) else { return nil }
        let bytes = [UInt8](repeating: byte, count: w * h * 4)
        texture.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: bytes, bytesPerRow: w * 4)
        return texture
    }

    func pair(
        _ commands: MTLCommandBuffer, _ target: MTLTexture, _ second: MTLTexture, load: MTLLoadAction,
        targetLoad: MTLLoadAction? = nil, _ draw: (MTLRenderCommandEncoder) -> Void
    ) {
        let pass = MTLRenderPassDescriptor()
        for (i, texture) in [target, second].enumerated() {
            pass.colorAttachments[i].texture = texture
            pass.colorAttachments[i].loadAction = i == 0 ? targetLoad ?? load : load
            pass.colorAttachments[i].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
            pass.colorAttachments[i].storeAction = .store
        }
        let e = commands.makeRenderCommandEncoder(descriptor: pass)!
        draw(e)
        e.endEncoding()
    }
}
