import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.x1.rotated_target's rotation-4 loads whose draws reach some 32x32 frame tiles and not others. Metal loads the plane
/// transposed and stores it only in the tiles a draw's primitives touch, rounded out to tiles by the scissor (measured on an
/// M4 Max, raw/runtime/raw_reach.h): the reference loads pass 1's frame flipped top to bottom in those tiles alone.
extension RotatedCells {
    var tilesX: Int { (width + 31) / 32 }

    /// The tiles (y * tilesX + x) a rotation-4 load cell's second pass reaches; nil where no pass loads one transposed
    func reachedTiles(_ cell: RotatedCell) -> Set<Int>? {
        guard turn == 4 else { return nil }
        switch cell {
        case .load: return Set(0..<(tilesX * ((height + 31) / 32)))
        case .partLoad, .pointLoad: return [1, tilesX + 2]
        case .edgeLoad: return [tilesX + 1, tilesX + 2]
        case .besideR4: return [1]
        default: return nil
        }
    }

    /// The references ahead of a cell's second pass: in each tile that pass reaches, a rotation-4 frame's texel (x, y) is pass
    /// 1's (x, H - 1 - y); the target's where it is of rotation 4, the plain texture of besideR4's second plane always
    func transposedLoad(_ cell: RotatedCell, into reference: MTLTexture) async throws {
        if let tiles = reachedTiles(cell) {
            try await transpose(reference, tiles, tilesX)
        }
        if cell == .besideR4, let second = beside.plainSecond {
            try await transpose(second, [1], (second.width + 31) / 32)
        }
    }

    private func transpose(_ texture: MTLTexture, _ tiles: Set<Int>, _ perRow: Int) async throws {
        let w = texture.width, h = texture.height
        let first = try await gpu.read(texture, bytesPerPixel: 4)
        var loaded = first
        for y in 0..<h {
            for x in 0..<w where tiles.contains((y / 32) * perRow + x / 32) {
                let to = (y * w + x) * 4, from = ((h - 1 - y) * w + x) * 4
                loaded[to..<(to + 4)] = first[from..<(from + 4)]
            }
        }
        try await gpu.upload(loaded, to: texture, bytesPerRow: w * 4)
    }

    /// Tiles (1, 0) and (2, 1): a rectangle inside the first, a full-frame draw scissored into the second, and draws that
    /// reach nothing: a full-frame one culled as back-facing, and rectangles just past the frame's right and bottom edges
    /// (inside a tile that overhangs the 90x60 frame)
    func partLoad() -> Pass {
        encode(load: .load) { e in
            shapes(e, rect(40, 4, 60, 24), [1, 0, 1, 1], states.plain)
            shapes(e, rect(width + 1, 10, width + 5, 20), [1, 1, 1, 1], states.discarded)
            shapes(e, rect(10, height + 1, 20, height + 5), [1, 1, 1, 1], states.discarded)
            e.setScissorRect(MTLScissorRect(x: 68, y: 36, width: 10, height: 10))
            shapes(e, rect(0, 0, width, height), [1, 1, 1, 1], states.discarded)
            e.setScissorRect(MTLScissorRect(x: 0, y: 0, width: width, height: height))
            e.setCullMode(.back)
            shapes(e, rect(0, 0, width, height, reversed: true), [1, 1, 1, 1], states.discarded)
            e.setCullMode(.none)
        }
    }

    /// The same tiles by a point (three pixels wide) and a line whose fragments are discarded (a turned plane's line
    /// rasteriser may pick other end pixels), which the raw path reaches at the frame's resolution
    func pointLoad() -> Pass {
        encode(load: .load) { e in
            let ndc = { (x: Float, y: Float) in
                SIMD4<Float>(x / Float(width) * 2 - 1, 1 - y / Float(height) * 2, 0.5, 1)
            }
            shapes(e, [ndc(50.5, 10.5)], [1, 1, 0, 1], states.plain, primitive: .point, perColor: 1)
            shapes(e, [ndc(70, 40.5), ndc(80, 40.5)], [1, 1, 1, 1], states.discarded, primitive: .line, perColor: 2)
        }
    }

    /// A rectangle from tile edge to tile edge, (32, 32) to (64, 64), whose fragments are discarded: Metal reaches the tiles
    /// past its far edges too
    func edgeLoad() -> Pass {
        encode(load: .load) { e in
            shapes(e, rect(32, 32, 64, 64), [1, 1, 1, 1], states.discarded)
        }
    }

    /// A second plane of rotation 4 and a larger frame beside the target (in the plain run, a texture of that frame), cleared
    /// and drawn; besideR4Load then loads both
    func besideR4(_ commands: MTLCommandBuffer, _ target: MTLTexture) {
        let w = width + 32, h = height + 16
        let second: MTLTexture
        if beside.turnedRun {
            let cells = RotatedCells(gpu: gpu, states: states, turn: 4, width: w, height: h, aligned: aligned)
            guard let plane = try? cells.makePlane() else { return }
            fillSurface(plane, 0x5a)
            beside.second = (plane, cells)
            second = plane.texture
        } else {
            guard let texture = try? gpu.texture(.bgra8Unorm, w, h, storage: .shared) else { return }
            let bytes = [UInt8](repeating: 0x5a, count: w * h * 4)
            texture.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: bytes, bytesPerRow: w * 4)
            beside.plainSecond = texture
            second = texture
        }
        pair(commands, target, second, load: .clear) { e in
            shapes(e, rect(4, 2, 60, 40), [1, 0, 0, 1], states.pair)
        }
    }

    /// Both loaded, a rectangle reaching tile (1, 0) alone: the second plane's stand-in takes that tile transposed, the rest
    /// by its store map's inverse
    func besideR4Load(_ commands: MTLCommandBuffer, _ target: MTLTexture) {
        guard let second = beside.turnedRun ? beside.second?.plane.texture : beside.plainSecond else { return }
        pair(commands, target, second, load: .load) { e in
            shapes(e, rect(40, 4, 60, 24), [0, 1, 0, 1], states.pair)
        }
    }
}
