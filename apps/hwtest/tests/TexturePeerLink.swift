import Foundation
import HWTestCore
import IOSurface
import Metal

/// The texture peer (peer/texpeer.mm) of the whole app process: started for the first test that asks and held until
/// the app or the peer ends. The peer exits when its app connection goes, and on 27 launchd starts it for the app's
/// Mach message no sooner than its throttle interval after the last start: a connection each test opened and closed
/// made the next test wait that interval out (boot 2: 9.4 s and 9.0 s). Each test drops what it had the peer keep.
enum TexturePeerLink {
    static let link = PeerLink<TexturePeerAcceptor> { try await start() }

    static func connection() async throws -> NSXPCConnection {
        guard let connection = try await link.connected().connection else {
            throw TestFailure("the texture peer's connection went")
        }
        return connection
    }

    /// The judgement every peer test adds: the run's tests share the one peer this process started.
    static func startedOnce(_ metrics: inout Metrics) async -> (String, Bool) {
        let starts = await link.starts
        metrics.set("peer_starts", starts)
        return ("the texture peer this process started once serves it (\(starts) started)", starts == 1)
    }

    private static func start() async throws -> TexturePeerAcceptor {
        let acceptor = TexturePeerAcceptor { gone in
            gone.close()
            Task { await link.lost(gone) }
        }
        let endpoint = acceptor.listener.endpoint
        if let why = await NativeSharedTexturePeerTest.offThread({ NativeSharedTexturePeerTest.startPeer(endpoint) }) {
            acceptor.close()
            throw TestFailure("no texture peer: \(why)")
        }
        let connected = await NativeSharedTexturePeerTest.offThread {
            acceptor.accepted.wait(timeout: .now() + 20) == .success
        }
        guard connected, acceptor.connection != nil else {
            acceptor.close()
            throw TestFailure("the texture peer never connected")
        }
        return acceptor
    }
}

/// The app's anonymous listener's side: the peer connects to it, and its connection is the one the app writes through.
final class TexturePeerAcceptor: NSObject, NSXPCListenerDelegate, SharedTexturePeerHost, @unchecked Sendable {
    let accepted = DispatchSemaphore(value: 0)
    let listener = NSXPCListener.anonymous()
    private let lost: @Sendable (TexturePeerAcceptor) -> Void
    private let lock = NSLock()
    private var held: NSXPCConnection?

    init(lost: @escaping @Sendable (TexturePeerAcceptor) -> Void) {
        self.lost = lost
        super.init()
        listener.delegate = self
        listener.resume()
    }

    var connection: NSXPCConnection? {
        lock.lock()
        defer { lock.unlock() }
        return held
    }

    func close() {
        connection?.invalidate()
        listener.invalidate()
    }

    func listener(_ listener: NSXPCListener, shouldAcceptNewConnection connection: NSXPCConnection) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        guard held == nil else { return false }
        let interface = NSXPCInterface(with: SharedTexturePeer.self)
        let classes = NSSet(object: MTLSharedTextureHandle.self) as! Set<AnyHashable>
        interface.setClasses(
            classes, for: #selector(SharedTexturePeer.write(_:reply:)), argumentIndex: 0, ofReply: false
        )
        let surfaces = NSSet(object: IOSurface.self) as! Set<AnyHashable>
        interface.setClasses(
            surfaces, for: #selector(SharedTexturePeer.renderSurface(_:keep:reply:)), argumentIndex: 0,
            ofReply: false
        )
        interface.setClasses(
            surfaces, for: #selector(SharedTexturePeer.makeSurfaceWidth(_:height:keep:reply:)), argumentIndex: 0,
            ofReply: true
        )
        connection.remoteObjectInterface = interface
        connection.exportedInterface = NSXPCInterface(with: SharedTexturePeerHost.self)
        connection.exportedObject = self
        connection.invalidationHandler = { [weak self] in if let self { lost(self) } }
        connection.interruptionHandler = { [weak self] in if let self { lost(self) } }
        connection.resume()
        held = connection
        return true
    }

    func ready(_ pid: Int32) { accepted.signal() }
}
