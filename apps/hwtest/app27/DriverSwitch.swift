import notify

enum DriverSwitch {
    static let prefix = "com.brunostjohn.switch.debug."

    static func post(_ name: String) {
        notify_post(prefix + name)
    }

    static func reclaimIdle() {
        post("idle-reclaim")
    }
}
