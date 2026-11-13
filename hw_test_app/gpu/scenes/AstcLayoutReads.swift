import Foundation
import HWTestCore
import Metal

/// What one read of gpu.astc.layouts' image found: blocks not where they were written (unread ones included), where
/// each block that was read had been written, and whether it was measured at all (a no-copy texture sampled off raw on
/// the facade reads nothing).
struct AstcLayoutReading {
    var wrong = 0
    var unread = 0
    var pairs: [(displayed: Int, written: Int)] = []
    var measured = true

    /// The address bit mapping of the misplaced blocks (BlockLinearMap.bitMap), worked out only for the cases reported.
    var map: String {
        guard wrong > 0 else { return "identity" }
        let map = BlockLinearMap.bitMap(pairs, bits: 4...13)
        return unread > 0 ? map + ",unread\(unread)" : map
    }
}

extension AstcLayoutRun {
    static let yLog2 = BlockLinearMap.yLog2(texelHeight: grid.texelHeight, rows: grid.down)

    /// Every texel sampled at its centre; a block is right when all its 16 texels decode to its own colour.
    func sample(_ texture: MTLTexture, noCopy: Bool) async throws -> AstcLayoutReading {
        let (width, height) = (Self.grid.texelWidth, Self.grid.texelHeight)
        let out = try gpu.buffer(width * height * 16)
        let timing = try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(sampler)
            encoder.setTexture(texture, index: 0)
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: width, height: height, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1))
            encoder.endEncoding()
        }
        if noCopy && onRaw && timing.path != "raw" { return AstcLayoutReading(measured: false) }
        let texels = out.contents().bindMemory(to: SIMD4<Float>.self, capacity: width * height)
        return Self.judge { x, y in
            let first = texels[y * 4 * width + x * 4]
            let uniform = (0..<16).allSatisfy { texels[(y * 4 + $0 / 4) * width + x * 4 + $0 % 4] == first }
            return uniform ? Self.grid.source(of: first) : nil
        }
    }

    /// The blocks the copy engine reads out of layer `slice` of `texture`, in rows.
    func copyEngine(_ texture: MTLTexture, slice: Int = 0) async throws -> AstcLayoutReading {
        let out = try gpu.buffer(Self.grid.bytes.count)
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: texture, sourceSlice: slice, sourceLevel: 0, sourceOrigin: MTLOrigin(),
                sourceSize: MTLSize(width: Self.grid.texelWidth, height: Self.grid.texelHeight, depth: 1), to: out,
                destinationOffset: 0, destinationBytesPerRow: Self.grid.rowBytes, destinationBytesPerImage: 0)
            blit.endEncoding()
        }
        return Self.blocks([UInt8](UnsafeRawBufferPointer(start: out.contents(), count: Self.grid.bytes.count)))
    }

    /// The blocks `texture.getBytes` reads: a no-copy texture's caller's memory, through the layout.
    func readBack(_ texture: MTLTexture) -> AstcLayoutReading {
        var bytes = [UInt8](repeating: 0, count: Self.grid.bytes.count)
        bytes.withUnsafeMutableBytes {
            texture.getBytes($0.baseAddress!, bytesPerRow: Self.grid.rowBytes, from: Self.region, mipmapLevel: 0)
        }
        return Self.blocks(bytes)
    }

    /// Blocks in rows, as the grid wrote them.
    static func blocks(_ bytes: [UInt8]) -> AstcLayoutReading {
        judge { x, y in
            let at = (y * grid.across + x) * AstcBlockGrid.blockBytes
            return AstcBlockGrid.decode(bytes[at..<(at + AstcBlockGrid.blockBytes)]).flatMap(grid.source(of:))
        }
    }

    static func judge(_ found: (Int, Int) -> (x: Int, y: Int)?) -> AstcLayoutReading {
        var reading = AstcLayoutReading()
        let address = { (x: Int, y: Int) in
            BlockLinearMap.offset(xB: x * AstcBlockGrid.blockBytes, y: y, rowBytes: grid.rowBytes, yLog2: yLog2)
        }
        for y in 0..<grid.down {
            for x in 0..<grid.across {
                guard let source = found(x, y) else {
                    reading.unread += 1
                    reading.wrong += 1
                    continue
                }
                if source.x != x || source.y != y { reading.wrong += 1 }
                reading.pairs.append((address(x, y), address(source.x, source.y)))
            }
        }
        return reading
    }
}
