import Darwin
import Foundation
import HWTestCore

/// The overlay's directories are union mounts over the sealed system's (/usr/libexec, /usr/sbin, /Applications):
/// readdir must list both layers' entries, as getattrlistbulk does, or CFBundle cannot find the sealed layer's
/// resources (dasd's and gamed's aborts on 27). Each resource bundle in /usr/libexec is looked up through NSBundle,
/// whose lookup lists the directory with readdir.
struct UnionDirectoryTest: HWTest {
    let id = "system.union_readdir"
    let name = "Overlay directories list the sealed system's entries too"
    let subsystem = Subsystem.system

    static let directories = ["/usr/libexec", "/usr/sbin", "/Applications"]

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
        var failures: [String] = []
        var listed = 0
        for directory in Self.directories {
            let key = directory.split(separator: "/").joined(separator: "_").lowercased()
            guard let read = Self.readdirNames(directory), let bulk = Self.bulkNames(directory) else { continue }
            listed += 1
            let missing = bulk.subtracting(read)
            metrics.set(key + "_readdir", read.count)
            metrics.set(key + "_bulk", bulk.count)
            metrics.set(key + "_missing", missing.count)
            if !missing.isEmpty {
                failures.append("\(directory): readdir misses \(missing.count), e.g. \(missing.sorted().prefix(3))")
            }
        }
        guard listed > 0 else { return .skip("the sandbox lets the app list none of \(Self.directories)", metrics) }
        let bundled = Self.bundleMisses("/usr/libexec")
        metrics.set("libexec_bundle_missing", bundled.count)
        if !bundled.isEmpty { failures.append("NSBundle cannot find \(bundled.prefix(3))") }
        return failures.isEmpty ? .pass(metrics) : .fail(failures.joined(separator: "; "), metrics)
    }

    static func readdirNames(_ path: String) -> Set<String>? {
        guard let directory = opendir(path) else { return nil }
        defer { closedir(directory) }
        var names = Set<String>()
        while let entry = readdir(directory) {
            let name = withUnsafePointer(to: &entry.pointee.d_name) {
                String(cString: UnsafeRawPointer($0).assumingMemoryBound(to: CChar.self))
            }
            if name != "." && name != ".." { names.insert(name) }
        }
        return names
    }

    /// Entry names through getattrlistbulk: each record is its length, the returned attribute set (five words) and
    /// the name's attrreference_t.
    static func bulkNames(_ path: String) -> Set<String>? {
        let fd = open(path, O_RDONLY | O_DIRECTORY)
        guard fd >= 0 else { return nil }
        defer { close(fd) }
        var request = attrlist()
        request.bitmapcount = u_short(ATTR_BIT_MAP_COUNT)
        request.commonattr =
            attrgroup_t(truncatingIfNeeded: ATTR_CMN_RETURNED_ATTRS) | attrgroup_t(truncatingIfNeeded: ATTR_CMN_NAME)
        var buffer = [UInt8](repeating: 0, count: 64 << 10)
        var names = Set<String>()
        while true {
            let count = buffer.withUnsafeMutableBytes { getattrlistbulk(fd, &request, $0.baseAddress, $0.count, 0) }
            guard count > 0 else { return count == 0 ? names : nil }
            buffer.withUnsafeBytes { bytes in
                var record = 0
                for _ in 0..<count {
                    let reference = record + 4 + MemoryLayout<attribute_set_t>.size
                    let offset = Int(bytes.loadUnaligned(fromByteOffset: reference, as: Int32.self))
                    let start = bytes.baseAddress!.advanced(by: reference + offset)
                    names.insert(String(cString: start.assumingMemoryBound(to: CChar.self)))
                    record += Int(bytes.loadUnaligned(fromByteOffset: record, as: UInt32.self))
                }
            }
        }
    }

    /// Resource bundles CFBundle looks up by name (Core ML models, Core Data models, bundles).
    static let resourceTypes: Set<String> = ["mlmodelc", "momd", "bundle"]

    /// The resource bundles in `path` NSBundle does not find.
    static func bundleMisses(_ path: String) -> [String] {
        guard let bundle = Bundle(path: path), let names = bulkNames(path) else { return [] }
        return names.sorted().filter { name in
            let url = URL(fileURLWithPath: name)
            let stem = url.deletingPathExtension().lastPathComponent
            return resourceTypes.contains(url.pathExtension) && !stem.isEmpty
                && bundle.path(forResource: stem, ofType: url.pathExtension) == nil
        }
    }
}
