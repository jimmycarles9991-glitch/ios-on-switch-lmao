import Foundation
import HWTestCore
import Metal

/// RenderBox's draw shape for an icon: an indexed triangle strip of four whose fragment fetches the colour it writes
/// (fetch_add_fragment over quad_vertex), into a cleared 32x32 BGRA8 target of private and of shared storage; once as
/// the four indices alone and once as two strips split by a restart index (0, 1, 2, 0xffff, 1, 2, 3). Each pixel is
/// covered once either way, so every pixel is the clear plus the value. `replay_scissor`: the four indices again with
/// back faces culled (the quad faces front) and a scissor of the target's middle 16x16 into a private target: where the
/// raw path draws it outside the pixel interlock its faces decide its pixels and it leaves it to m2v; only the
/// scissor's pixels take the sum, as Metal draws it, wherever it is drawn. A cell's `wrong` counts pixels more than a code off, `path` and `routes` are the command buffer's path and its
/// fetching draws' routes on the raw facade.
public enum FetchStrip {
    public struct Cell: Sendable {
        public let name, path, routes: String
        public let wrong: Int
        public var error = ""  // what its command buffer or readback threw, "" for nothing
    }

    static let side = 32
    static let indices: [UInt16] = [0, 1, 2, 3, 0, 1, 2, 0xffff, 1, 2, 3]
    static let want: [UInt8] = [96, 64, 128, 255]  // BGRA8 of the clear (0.25, 0.25, 0.25, 1) plus (0.25, 0, 0.125)
    static let clear: [UInt8] = [64, 64, 64, 255]
    static let derivatives: [UInt8] = [96, 128, 128, 255]  // BGRA8 of (dfdx * 2, |dfdy|, fwidth / 2, 1)
    static let scissor = MTLScissorRect(x: 8, y: 8, width: 16, height: 16)

    public static func cells(_ gpu: GPU) async throws -> [Cell] {
        let state = try gpu.render(fragment: "fetch_add_fragment", format: .bgra8Unorm)
        let indexBuffer = try gpu.buffer(indices)
        var out: [Cell] = []
        for storage in [MTLStorageMode.private, .shared] {
            for (arm, offset, count) in [("strip", 0, 4), ("strip_restart", 4, 7)] {
                let name = "\(arm)_\(storage == .private ? "private" : "shared")"
                out.append(
                    await guarded(
                        gpu, name, state, indexBuffer, offset: offset, count: count, storage: storage, culled: false))
            }
        }
        out.append(
            await guarded(
                gpu, "replay_scissor", state, indexBuffer, offset: 0, count: 4, storage: .private, culled: true))
        for storage in [MTLStorageMode.private, .shared] {
            let name = "derivative_\(storage == .private ? "private" : "shared")"
            do {
                let derivative = try gpu.render(fragment: "fetch_derivative_fragment", format: .bgra8Unorm)
                out.append(
                    await guarded(
                        gpu, name, derivative, indexBuffer, offset: 0, count: 4, storage: storage, culled: false,
                        want: derivatives))
            } catch {
                var failed = Cell(name: name, path: "-", routes: "-", wrong: side * side)
                failed.error = "\(error)"
                out.append(failed)
            }
        }
        return out
    }

    /// `cell`, an error recorded in the cell instead of thrown, so one arm's failure leaves the others' readings
    static func guarded(
        _ gpu: GPU, _ name: String, _ state: MTLRenderPipelineState, _ indexBuffer: MTLBuffer, offset: Int, count: Int,
        storage: MTLStorageMode, culled: Bool, want: [UInt8] = FetchStrip.want
    ) async -> Cell {
        do {
            return try await cell(
                gpu, name, state, indexBuffer, offset: offset, count: count, storage: storage, culled: culled,
                want: want)
        } catch {
            var failed = Cell(name: name, path: "-", routes: "-", wrong: side * side)
            failed.error = "\(error)"
            return failed
        }
    }

    static func cell(
        _ gpu: GPU, _ name: String, _ state: MTLRenderPipelineState, _ indexBuffer: MTLBuffer, offset: Int, count: Int,
        storage: MTLStorageMode, culled: Bool, want: [UInt8] = FetchStrip.want
    ) async throws -> Cell {
        let target = try gpu.texture(.bgra8Unorm, side, side, storage: storage)
        let before = RawFacade.counters(of: gpu.device)
        let path = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.25, green: 0.25, blue: 0.25, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(state)
            if culled {
                e.setFrontFacing(.counterClockwise)
                e.setCullMode(.back)
                e.setScissorRect(scissor)
            }
            var corners = GPU.fullScreen
            var value = SIMD4<Float>(0.25, 0, 0.125, 1)
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            e.setFragmentBytes(&value, length: 16, index: 0)
            e.drawIndexedPrimitives(
                type: .triangleStrip, indexCount: count, indexType: .uint16, indexBuffer: indexBuffer,
                indexBufferOffset: offset * 2)
            e.endEncoding()
        }.path
        let routes = RawFacade.fetchRoutes(before: before, after: RawFacade.counters(of: gpu.device))
        let bytes = try await gpu.read(target, bytesPerPixel: 4)
        let wrong = (0..<(side * side)).filter { p in
            let x = p % side, y = p / side
            let inside =
                !culled
                || (scissor.x..<scissor.x + scissor.width).contains(x)
                    && (scissor.y..<scissor.y + scissor.height).contains(y)
            let expected = inside ? want : clear
            return (0..<4).contains { abs(Int(bytes[p * 4 + $0]) - Int(expected[$0])) > 1 }
        }.count
        return Cell(name: name, path: path, routes: routes, wrong: wrong)
    }
}
