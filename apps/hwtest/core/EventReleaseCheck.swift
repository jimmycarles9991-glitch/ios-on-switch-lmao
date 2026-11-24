public enum EventReleaseCheck {
    public struct WaiterOutcome: Sendable {
        public let waited: Bool
        public let done: Bool
        public let wrongBytes: Int

        public init(waited: Bool, done: Bool, wrongBytes: Int) {
            self.waited = waited
            self.done = done
            self.wrongBytes = wrongBytes
        }
    }

    /// fma_loop reads and writes one float4 per thread.
    public static func fmaLoopBufferBytes(threads: Int) -> Int {
        precondition(threads >= 0)
        return threads * MemoryLayout<SIMD4<Float>>.stride
    }

    public static func waitersPassed(_ rounds: [WaiterOutcome], expectedWaited: Int) -> Bool {
        let waited = rounds.filter(\.waited)
        return waited.count == expectedWaited && waited.allSatisfy { $0.done && $0.wrongBytes == 0 }
    }
}
