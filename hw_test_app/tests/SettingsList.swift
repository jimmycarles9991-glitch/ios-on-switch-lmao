import UIKit

/// A list like Settings' root: inset grouped sections of rows with a symbol, a title, a value and a disclosure
/// indicator.
final class SettingsList: NSObject, UITableViewDataSource {
    static let rows: [(String, String, UIColor)] = [
        ("Wi-Fi", "wifi", .systemBlue), ("Bluetooth", "dot.radiowaves.left.and.right", .systemBlue),
        ("Notifications", "bell.badge.fill", .systemRed), ("Sounds", "speaker.wave.3.fill", .systemPink),
        ("Focus", "moon.fill", .systemIndigo), ("Screen Time", "hourglass", .systemIndigo),
        ("General", "gear", .systemGray), ("Control Centre", "switch.2", .systemGray),
        ("Display & Brightness", "sun.max.fill", .systemBlue), ("Wallpaper", "photo.fill", .systemTeal),
        ("Battery", "battery.100", .systemGreen), ("Privacy & Security", "hand.raised.fill", .systemBlue),
    ]

    private static let shared = SettingsList()

    static func make(_ frame: CGRect) -> UITableView {
        let table = UITableView(frame: frame, style: .insetGrouped)
        table.register(UITableViewCell.self, forCellReuseIdentifier: "row")
        table.dataSource = shared
        return table
    }

    func numberOfSections(in tableView: UITableView) -> Int { 6 }

    func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int { 10 }

    func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "row", for: indexPath)
        let (title, symbol, color) = Self.rows[(indexPath.section * 10 + indexPath.row) % Self.rows.count]
        var content = UIListContentConfiguration.valueCell()
        content.text = title
        content.secondaryText = indexPath.row % 3 == 0 ? "On" : nil
        content.image = UIImage(systemName: symbol)
        content.imageProperties.tintColor = color
        cell.contentConfiguration = content
        cell.accessoryType = .disclosureIndicator
        return cell
    }
}
