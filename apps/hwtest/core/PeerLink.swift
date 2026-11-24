import Foundation

/// A helper process the tests of one app process share: started for the first test that asks, handed to every later
/// one while it lives, and started again only once it is lost. An actor because the loss arrives on the connection's
/// own queue while a test may be asking.
public actor PeerLink<Peer: AnyObject & Sendable> {
    private let start: @Sendable () async throws -> Peer
    private var peer: Peer?
    private var starting: Task<Peer, Error>?
    public private(set) var starts = 0

    public init(start: @escaping @Sendable () async throws -> Peer) {
        self.start = start
    }

    public func connected() async throws -> Peer {
        if let peer { return peer }
        if let starting { return try await starting.value }
        let task = Task { try await start() }
        starting = task
        defer { starting = nil }
        let started = try await task.value
        starts += 1
        peer = started
        return started
    }

    public func lost(_ gone: Peer) {
        if peer === gone { peer = nil }
    }
}
