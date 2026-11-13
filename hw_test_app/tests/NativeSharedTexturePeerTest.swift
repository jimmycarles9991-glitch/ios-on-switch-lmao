import Darwin
import Foundation
import HWTestCore
import HWTestGPU
import Metal
import SwitchTexturePattern
import XPC

/// What the texture peer (peer/texpeer.mm) does for the app: opens a handle with its own device and writes every texel
/// of it on the GPU, answering "ok pid=<pid> registry=<id> path=<its command buffer's path> shareable=<0|1>
/// footprint_kb=<now> peak_kb=<its peak>" once that work completed, or "fail=<why> ...".
@objc protocol SharedTexturePeer {
    func write(_ handle: MTLSharedTextureHandle, reply: @escaping (String) -> Void)
}

/// The peer's first message, which brings its connection to the app's listener (an NSXPC connection reaches the listener
/// only with one)
@objc protocol SharedTexturePeerHost {
    func ready(_ pid: Int32)
}

/// The app's anonymous listener's side: the peer connects to it, and its connection is the one the app writes through.
final class TexturePeerAcceptor: NSObject, NSXPCListenerDelegate, SharedTexturePeerHost, @unchecked Sendable {
    let accepted = DispatchSemaphore(value: 0)
    private let lock = NSLock()
    private var held: NSXPCConnection?

    var connection: NSXPCConnection? {
        lock.lock()
        defer { lock.unlock() }
        return held
    }

    func listener(_ listener: NSXPCListener, shouldAcceptNewConnection connection: NSXPCConnection) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        guard held == nil else { return false }
        let interface = NSXPCInterface(with: SharedTexturePeer.self)
        let classes = NSSet(object: MTLSharedTextureHandle.self) as! Set<AnyHashable>
        interface.setClasses(
            classes, for: #selector(SharedTexturePeer.write(_:reply:)), argumentIndex: 0, ofReply: false)
        connection.remoteObjectInterface = interface
        connection.exportedInterface = NSXPCInterface(with: SharedTexturePeerHost.self)
        connection.exportedObject = self
        connection.resume()
        held = connection
        return true
    }

    func ready(_ pid: Int32) { accepted.signal() }
}

/// Shared textures across two processes: the app makes one of each shape the raw path now shares (a mipmapped 2D
/// texture, a 2D array, a cube and a cube array, a 3D texture, a 4-sample texture, a 1D texture and a mipmapped ASTC
/// one and a one-level ASTC one, each where the device makes the private texture of its descriptor) and sends each handle to the texture peer, a
/// process of its own with its own device and raw channel, which opens it with newSharedTextureWithHandle: and writes
/// every level, slice and sample on the GPU (blits; a render pass into a mipmapped one's level 1 and into the
/// multisampled one). The app then reads every texel back in its own texture of the same pages: no texel was copied
/// between the processes. iOS 17 has the BlueTool stand-in start the peer, iOS 27 launchd for its Mach service.
struct NativeSharedTexturePeerTest: NativeObjectTest {
    let id = "metal.native.shared_texture_peer"
    let name = "Shared textures of every shape written by a second process and read back by the app"

    struct Shape {
        let name: String
        let descriptor: MTLTextureDescriptor
    }

    static func descriptor(
        _ type: MTLTextureType, _ format: MTLPixelFormat, _ width: Int, _ height: Int = 1, depth: Int = 1,
        levels: Int = 1, layers: Int = 1, samples: Int = 1, usage: MTLTextureUsage = .shaderRead
    ) -> MTLTextureDescriptor {
        let d = MTLTextureDescriptor()
        d.textureType = type
        d.pixelFormat = format
        d.width = width
        d.height = height
        d.depth = depth
        d.mipmapLevelCount = levels
        d.arrayLength = layers
        d.sampleCount = samples
        d.usage = usage
        d.storageMode = .private
        return d
    }

    static let shapes: [Shape] = [
        Shape(
            name: "chain",
            descriptor: descriptor(.type2D, .rgba8Unorm, 64, 48, levels: 7, usage: [.shaderRead, .renderTarget])),
        Shape(name: "array", descriptor: descriptor(.type2DArray, .rgba8Unorm, 40, 24, layers: 3)),
        Shape(name: "cube", descriptor: descriptor(.typeCube, .rgba8Unorm, 32, 32)),
        Shape(name: "cubearray", descriptor: descriptor(.typeCubeArray, .rgba8Unorm, 16, 16, layers: 2)),
        Shape(name: "volume", descriptor: descriptor(.type3D, .rgba8Unorm, 20, 12, depth: 9)),
        Shape(
            name: "samples4",
            descriptor: descriptor(.type2DMultisample, .rgba8Unorm, 32, 24, samples: 4, usage: .renderTarget)),
        Shape(name: "line", descriptor: descriptor(.type1D, .rgba8Unorm, 300)),
        Shape(name: "astc", descriptor: descriptor(.type2D, .astc_4x4_ldr, 100, 60, levels: 7)),
        Shape(name: "astc1", descriptor: descriptor(.type2D, .astc_4x4_ldr, 64, 40)),
    ]

    static func slices(_ t: MTLTexture) -> Int {
        switch t.textureType {
        case .typeCube: return 6
        case .typeCubeArray: return 6 * t.arrayLength
        case .type1DArray, .type2DArray: return t.arrayLength
        default: return 1
        }
    }

    /// The peer's process, started for the app's listener: nil, or why it was not
    static func startPeer(_ endpoint: NSXPCListenerEndpoint) -> String? {
        typealias Create = @convention(c) (UnsafePointer<CChar>, DispatchQueue?, UInt64) -> xpc_connection_t
        let get = NSSelectorFromString("_endpoint")
        guard let symbol = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "xpc_connection_create_mach_service"),
            endpoint.responds(to: get), let xpc = endpoint.perform(get)?.takeUnretainedValue()
        else { return "no XPC endpoint or Mach service lookup" }
        let ios = ProcessInfo.processInfo.operatingSystemVersion.majorVersion
        let service = ios >= 27 ? "com.brunostjohn.switch.texpeer" : "com.apple.BlueTool"
        let connection = unsafeBitCast(symbol, to: Create.self)(service, nil, 0)
        xpc_connection_set_event_handler(connection) { _ in }
        xpc_connection_resume(connection)
        defer { xpc_connection_cancel(connection) }
        let message = xpc_dictionary_create(nil, nil, 0)
        xpc_dictionary_set_string(message, "kMsgId", "SwitchTexturePeer")
        xpc_dictionary_set_value(message, "endpoint", xpc as! xpc_object_t)
        let reply = xpc_connection_send_message_with_reply_sync(connection, message)
        guard xpc_get_type(reply) == XPC_TYPE_DICTIONARY else { return "\(service) did not answer" }
        let result = xpc_dictionary_get_int64(reply, "result")
        return result == 0 ? nil : "\(service) answered \(result)"
    }

    static func offThread<T: Sendable>(_ body: @escaping @Sendable () -> T) async -> T {
        await withCheckedContinuation { continuation in
            DispatchQueue.global().async { continuation.resume(returning: body()) }
        }
    }

    static func write(_ connection: NSXPCConnection, _ handle: MTLSharedTextureHandle) async -> String? {
        let answer = HandlerValue<String?>(nil)
        let done = DispatchSemaphore(value: 0)
        let proxy = connection.remoteObjectProxyWithErrorHandler { _ in done.signal() } as? SharedTexturePeer
        proxy?.write(handle) {
            answer.value = $0
            done.signal()
        }
        let arrived = await offThread { done.wait(timeout: .now() + 20) == .success }
        return arrived ? answer.value : nil
    }

    /// The answer's fields after its first word
    static func fields(_ answer: String) -> [String: String] {
        Dictionary(
            answer.split(separator: " ").dropFirst().compactMap { field -> (String, String)? in
                let parts = field.split(separator: "=", maxSplits: 1)
                return parts.count == 2 ? (String(parts[0]), String(parts[1])) : nil
            }, uniquingKeysWith: { first, _ in first })
    }

    /// Level `level` of slice `slice` (every plane of a 3D one), its rows packed: texels, or a block-compressed one's blocks
    static func readBack(
        _ gpu: GPU, _ t: MTLTexture, level: Int, slice: Int, paths: inout [String]
    ) async throws -> [UInt8] {
        let blocks = t.pixelFormat == .astc_4x4_ldr
        let w = max(t.width >> level, 1), h = max(t.height >> level, 1)
        let d = t.textureType == .type3D ? max(t.depth >> level, 1) : 1
        let row = blocks ? (w + 3) / 4 * 16 : w * 4, rows = blocks ? (h + 3) / 4 : h
        let out = try gpu.buffer(row * rows * d)
        let timing = try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: t, sourceSlice: slice, sourceLevel: level, sourceOrigin: MTLOrigin(),
                sourceSize: MTLSize(width: w, height: h, depth: d), to: out, destinationOffset: 0,
                destinationBytesPerRow: row, destinationBytesPerImage: d > 1 ? row * rows : 0)
            blit.endEncoding()
        }
        paths.append(timing.path)
        return [UInt8](UnsafeRawBufferPointer(start: out.contents(), count: row * rows * d))
    }

    /// The bytes the peer wrote into `level` of `slice`, as readBack returns them
    static func expected(_ t: MTLTexture, level: Int, slice: Int) -> [UInt8] {
        let blocks = t.pixelFormat == .astc_4x4_ldr
        let w = max(t.width >> level, 1), h = max(t.height >> level, 1)
        let d = t.textureType == .type3D ? max(t.depth >> level, 1) : 1
        let across = blocks ? (w + 3) / 4 : w, down = blocks ? (h + 3) / 4 : h, unit = blocks ? 16 : 4
        let uniform =
            t.sampleCount > 1
            || switch_texpeer_rendered(UInt32(t.textureType.rawValue), UInt32(t.mipmapLevelCount), UInt32(level))
        var out: [UInt8] = []
        out.reserveCapacity(across * down * d * unit)
        for z in 0..<d {
            for y in 0..<down {
                for x in 0..<across {
                    for c in 0..<unit {
                        let (l, s, xx, yy) = (UInt32(level), UInt32(d > 1 ? z : slice), UInt32(x), UInt32(y))
                        out.append(
                            blocks
                                ? switch_texpeer_block(l, s, xx, yy, UInt32(c))
                                : uniform
                                    ? switch_texpeer_texel(l, 0, 0, 0, UInt32(c))
                                    : switch_texpeer_texel(l, s, xx, yy, UInt32(c)))
                    }
                }
            }
        }
        return out
    }

    /// A multisampled texture's samples resolved by a pass that loads them, into a one-sample texture of its format
    static func resolved(_ gpu: GPU, _ t: MTLTexture, paths: inout [String]) async throws -> MTLTexture {
        let one = try gpu.texture(t.pixelFormat, t.width, t.height)
        let timing = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = t
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .multisampleResolve
            pass.colorAttachments[0].resolveTexture = one
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        paths.append(timing.path)
        return one
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        var work = NativeWork()
        var made: [(String, MTLTexture, MTLSharedTextureHandle)] = []
        var unmade: [String] = []
        for shape in Self.shapes {
            guard device.makeTexture(descriptor: shape.descriptor) != nil else {
                unmade.append(shape.name)
                continue
            }
            guard let texture = device.makeSharedTexture(descriptor: shape.descriptor),
                let handle = texture.makeSharedTextureHandle()
            else {
                work.failures.append(
                    "no shared \(shape.name) texture or handle, where the device makes the private one")
                continue
            }
            work.objects.append(texture)
            made.append((shape.name, texture, handle))
        }
        metrics.set("shapes", made.isEmpty ? "-" : made.map(\.0).joined(separator: ","))
        metrics.set("unmade", unmade.isEmpty ? "-" : unmade.joined(separator: ","))

        let acceptor = TexturePeerAcceptor()
        let listener = NSXPCListener.anonymous()
        listener.delegate = acceptor
        listener.resume()
        defer {
            acceptor.connection?.invalidate()
            listener.invalidate()
        }
        let endpoint = listener.endpoint
        if let why = await Self.offThread({ Self.startPeer(endpoint) }) { throw TestFailure("no texture peer: \(why)") }
        let connected = await Self.offThread { acceptor.accepted.wait(timeout: .now() + 20) == .success }
        guard connected, let connection = acceptor.connection else {
            throw TestFailure("the texture peer never connected")
        }
        metrics.set("peer_pid", Int(connection.processIdentifier))
        if connection.processIdentifier == getpid() { work.failures.append("the peer is this process") }

        var peerPaths: [String] = []
        var peerPeak = "-"
        for (name, texture, handle) in made {
            guard let answer = await Self.write(connection, handle) else {
                work.failures.append("the peer did not answer for the \(name) texture")
                continue
            }
            let fields = Self.fields(answer)
            peerPaths.append(fields["path"] ?? "-")
            peerPeak = fields["peak_kb"] ?? peerPeak
            if !answer.hasPrefix("ok ") { work.failures.append("the peer's \(name) write: \(answer)") }
            if fields["path"] != "raw" {
                work.failures.append("the peer's \(name) command buffer went \(fields["path"] ?? "-")")
            }
            if fields["registry"] != String(device.registryID, radix: 16) {
                work.failures.append("the peer's device is registry \(fields["registry"] ?? "-")")
            }
            if fields["shareable"] != "1" { work.failures.append("the peer's \(name) texture is not shareable") }
            var source = texture
            if texture.sampleCount > 1 { source = try await Self.resolved(gpu, texture, paths: &work.paths) }
            var wrong = 0
            for level in 0..<source.mipmapLevelCount {
                for slice in 0..<(source.textureType == .type3D ? 1 : Self.slices(source)) {
                    let got = try await Self.readBack(gpu, source, level: level, slice: slice, paths: &work.paths)
                    let want = Self.expected(texture, level: level, slice: slice)
                    wrong += zip(got, want).filter { $0 != $1 }.count + abs(got.count - want.count)
                }
            }
            metrics.set("wrong_\(name)", wrong)
            if wrong != 0 { work.failures.append("\(wrong) bytes of the \(name) texture are not the peer's") }
        }
        metrics.set("peer_paths", peerPaths.isEmpty ? "-" : peerPaths.joined(separator: ","))
        metrics.set("peer_peak_kb", peerPeak)
        return work
    }
}
