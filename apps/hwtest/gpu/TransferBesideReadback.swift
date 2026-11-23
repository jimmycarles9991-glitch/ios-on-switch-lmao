import Foundation
import HWTestCore
import Metal

/// gpu.path.transfer_beside_readback's work: one thread reads level 1 of a mipmapped 4096x4096 RGBA8 shared texture
/// with getBytes over and over (a shared mip level's copy goes through the copy engine on the driver), while the
/// caller writes level 1 of small mipmapped shared textures with replaceRegion and times each call
public enum TransferBesideReadback {
    public static let bigSide = 4096, smallSide = 128, smalls = 8, uploads = 64

    public struct Timed: Sendable {
        /// Each getBytes of the big level and each replaceRegion of a small one, in ms
        public var readsMs: [Double] = [], uploadsMs: [Double] = []
        /// Uploads that started while a read was in flight
        public var overlapped = 0
    }

    final class Reader: @unchecked Sendable {
        private let lock = NSLock()
        private var running = true, reading = false
        private var spans: [Double] = []
        private let finished = DispatchSemaphore(value: 0)
        /// Signalled as the first read begins
        let began = DispatchSemaphore(value: 0)

        func start(_ texture: MTLTexture) {
            let side = texture.width / 2, row = side * 4
            Thread { [self] in
                var bytes = [UInt8](repeating: 0, count: row * side)
                var first = true
                while isRunning {
                    set(reading: true)
                    if first {
                        began.signal()
                        first = false
                    }
                    let start = DispatchTime.now().uptimeNanoseconds
                    bytes.withUnsafeMutableBytes {
                        texture.getBytes(
                            $0.baseAddress!, bytesPerRow: row, from: MTLRegionMake2D(0, 0, side, side), mipmapLevel: 1
                        )
                    }
                    let ms = Double(DispatchTime.now().uptimeNanoseconds - start) / 1e6
                    lock.lock()
                    reading = false
                    spans.append(ms)
                    lock.unlock()
                }
                finished.signal()
            }.start()
        }

        var isRunning: Bool {
            lock.lock()
            defer { lock.unlock() }
            return running
        }

        var isReading: Bool {
            lock.lock()
            defer { lock.unlock() }
            return reading
        }

        private func set(reading now: Bool) {
            lock.lock()
            reading = now
            lock.unlock()
        }

        func stop() -> [Double] {
            lock.lock()
            running = false
            lock.unlock()
            finished.wait()
            return spans
        }
    }

    public static func run(_ device: MTLDevice) throws -> Timed {
        func texture(_ side: Int) throws -> MTLTexture {
            let d = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .rgba8Unorm, width: side, height: side, mipmapped: true
            )
            d.storageMode = .shared
            d.usage = [.shaderRead]
            guard let made = device.makeTexture(descriptor: d) else { throw TestFailure("no \(side)x\(side) texture") }
            return made
        }
        let big = try texture(bigSide)
        let small = try (0 ..< smalls).map { _ in try texture(smallSide) }
        let half = smallSide / 2
        let bytes = [UInt8](repeating: 0x5a, count: half * half * 4)
        let reader = Reader()
        reader.start(big)
        reader.began.wait()
        var out = Timed()
        for i in 0 ..< uploads {
            if reader.isReading { out.overlapped += 1 }
            let start = DispatchTime.now().uptimeNanoseconds
            bytes.withUnsafeBytes {
                small[i % smalls].replace(
                    region: MTLRegionMake2D(0, 0, half, half), mipmapLevel: 1, withBytes: $0.baseAddress!,
                    bytesPerRow: half * 4
                )
            }
            out.uploadsMs.append(Double(DispatchTime.now().uptimeNanoseconds - start) / 1e6)
        }
        out.readsMs = reader.stop()
        return out
    }
}
