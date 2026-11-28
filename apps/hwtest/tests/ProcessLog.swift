import Foundation
import OSLog

/// This process's own log, as a test that judges a driver line reads it
public enum ProcessLog {
    public static let metal = "com.brunostjohn.switch.TegraGM20BMetal", refusal = "com.brunostjohn.switch.refusal"

    /// The lines of `subsystem` and `category` since `start` that contain `text`; nil when the log cannot be read
    public static func lines(
        _ subsystem: String, _ category: String, containing text: String, since start: Date
    ) -> [String]? {
        guard let store = try? OSLogStore(scope: .currentProcessIdentifier),
              let entries = try? store.getEntries(
                  at: store.position(date: start),
                  matching: NSPredicate(
                      format: "subsystem == %@ AND category == %@ AND composedMessage CONTAINS %@", subsystem, category,
                      text
                  )
              )
        else { return nil }
        return entries.compactMap { $0 as? OSLogEntryLog }.filter {
            $0.date >= start && $0.subsystem == subsystem && $0.category == category
                && $0.composedMessage.contains(text)
        }.map(\.composedMessage)
    }
}
