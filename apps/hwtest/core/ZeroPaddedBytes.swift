public enum ZeroPaddedBytes {
    /// Reusable GPU staging pools return unzeroed memory, so zero the full span before writing any prefix.
    public static func fill(
        _ destination: UnsafeMutableRawPointer, length: Int, prefix: UnsafeRawBufferPointer? = nil
    ) {
        guard length > 0 else { return }
        destination.initializeMemory(as: UInt8.self, repeating: 0, count: length)
        if let prefix, let base = prefix.baseAddress, !prefix.isEmpty {
            destination.copyMemory(from: base, byteCount: min(length, prefix.count))
        }
    }
}
