import Foundation

/// The Core Animation states gpu.gm20b.fetch_rung2/3 draw (pack/ca/fetchfree.py makes their fetch-free forms; their
/// lines are the pack corpus's, tagged with each IPSW's build). `key` names the state's metrics ([a-z0-9_]),
/// `program` the iterated program chip_facts.h kIteratedProven names it by, `passes` its length.
enum FetchRungStates {
    struct State {
        let label: String
        let key: String
        let program: String
        let passes: Int
        /// judged by its codes alone: its time and its draws' coverage say nothing of the program
        var codesOnly = false
    }

    static let fetching: [Int: [State]] = [
        2: [State(label: "PBGRAXm_TimgBaddA2Xhf_Isrc", key: "add", program: "add", passes: 0)],
        3: [
            State(label: "PBGRAXm_TimgBmpyA2Xhf_Isrc", key: "mpy", program: "mpy", passes: 2),
            State(label: "PBGRAXm_TimgBpdiA2Xhf_Isrc", key: "pdi", program: "pdi", passes: 2),
            State(label: "PBGRAXm_TimgBlbnA2Xhf_Isrc", key: "lbn", program: "lbn", passes: 4),
            State(label: "PBGRAXm_TimgBpldA2Xhf_Isrc", key: "pld", program: "pld", passes: 5),
            State(label: "PBGRAXm_TimgBdrkA2Xhf_Isrc", key: "drk", program: "drk", passes: 5),
            State(label: "PBGRAXm_TimgBltnA2Xhf_Isrc", key: "ltn", program: "ltn", passes: 5),
            State(label: "PBGRAXm_TimgBsdsA2Xhf_Isrc", key: "sds", program: "sds", passes: 5),
            State(label: "PBGRAXm_TimgBdifA2Xhf_Isrc", key: "dif", program: "dif", passes: 8),
            State(label: "PBGRAXm_TimgBlltA2Xhf_Isrc", key: "llt", program: "llt", passes: 9),
            State(label: "PBGRAXm_TimgBovlA2Xhf_Isrc", key: "ovl", program: "ovl", passes: 9),
            State(label: "PBGRAXm_TimgBpinA2Xhf_Isrc", key: "pin", program: "pin", passes: 9),
            State(label: "PBGRAXm_TimgBsbtA2Xhf_Isrc", key: "sbt", program: "sbt", passes: 9),
            State(label: "PBGRAXm_TimgBhltA2Xhf_Isrc", key: "hlt", program: "hlt", passes: 10),
            State(label: "PBGRAXm_TimgBsltA2Xhf_Isrc", key: "slt", program: "slt", passes: 11),
            State(label: "PBGRAXm_TimgBdsoA2Xhf_Isrc", key: "dso", program: "dso", passes: 13),
            State(label: "PBGRAXm_TimgBlsoA2Xhf_Isrc", key: "lso", program: "lso", passes: 14),
            State(label: "PBGRAXm_TimgBdlcA2Xhf_Isrc", key: "dlc", program: "dlc", passes: 28),
            State(label: "PBGRAXmw_TimgBvcmA3Xhf_Isrc", key: "vcm", program: "vcm", passes: 38),
            State(label: "PBGRAXm_TimgBlumA2Xhf_Isrc", key: "lum", program: "lum", passes: 42),
            State(label: "PBGRAXm_TimgBcliA2Xhf_Isrc", key: "cli", program: "cli", passes: 54),
            State(label: "PBGRAXm_TimgBsatA2Xhf_Isrc", key: "sat", program: "sat", passes: 64),
            // its texture function's colour leaves [0, 1]: the form's SET_PS_SATURATE clamps it ahead of the program
            State(label: "PBGRAXm_TmuaBmpyA2Xhfn_IsrcCcl_Isup", key: "mua", program: "mpy", passes: 2),
            // a texture function whose lanes the host gate's inputs all discard: compared here only
            State(label: "PBGRAXm_TkfhBmpyA2Xhf_Irsd", key: "kfh", program: "mpy", passes: 2, codesOnly: true),
            State(label: "PBGRAXm_TimgBlltA2Xhfu_Isrc", key: "lltu", program: "llt.u", passes: 5),
            State(label: "PBGRAXm_TimgBsbtA2Xhfu_Isrc", key: "sbtu", program: "sbt.u", passes: 5),
            State(label: "PBGRAXm_TimgBdivA2Xhfu_Isrc", key: "divu", program: "div.u", passes: 7),
            State(label: "PBGRAXm_TimgBcdgA2Xhfu_Isrc", key: "cdgu", program: "cdg.u", passes: 9),
            State(label: "PBGRAXm_TimgBdlcA2Xhfu_Isrc", key: "dlcu", program: "dlc.u", passes: 10),
            // RGBA16Float targets under unbounded range, compared in halves: 24B119's, then 21H461's
            State(label: "PRGhAXm_TdgdBlltA2Xhfu_Isrc", key: "lltuh", program: "llt.u", passes: 5),
            State(label: "PRGhAXm_TdgdBsbtA2Xhfu_Isrc", key: "sbtuh", program: "sbt.u", passes: 5),
            State(label: "PRGhAXm_TdgdBdivA2Xhfu_Isrc", key: "divuh", program: "div.u", passes: 7),
            State(label: "PRGhAXm_TdgdBcdgA2Xhfu_Isrc", key: "cdguh", program: "cdg.u", passes: 9),
            State(label: "PRGhAXm_TdgdBdlcA2Xhfu_Isrc", key: "dlcuh", program: "dlc.u", passes: 10),
            State(label: "PRGhAXm_TdgdBcbnA2Xhfu_Isrc", key: "cbnuh", program: "cbn.u", passes: 11),
            State(label: "PRGhAXmw_TimgBlltA3Xhfcu_Isrc", key: "lltuh", program: "llt.u", passes: 5),
            State(label: "PRGhAXmw_TimgBsbtA3Xhfcu_Isrc", key: "sbtuh", program: "sbt.u", passes: 5),
            State(label: "PRGhAXmw_TimgBdivA3Xhfcu_Isrc", key: "divuh", program: "div.u", passes: 7),
            State(label: "PRGhAXmw_TimgBcdgA3Xhfcu_Isrc", key: "cdguh", program: "cdg.u", passes: 9),
            State(label: "PRGhAXmw_TimgBcbnA3Xhfcu_Isrc", key: "cbnuh", program: "cbn.u", passes: 11),
        ],
    ]

    static let plain: [Int: [(label: String, key: String)]] = [
        2: [("PBGRAXm_TimgBclrA2Xhf_Isrc", "clr"), ("PBGRAXm_TimgBclrA2Xhf_IsrcM1", "clr_m1")],
    ]

    /// Whether the pack holds no fetch-free form of the state on this IPSW: 21H461's plus darker
    /// (pack/iterated/table.py BEYOND_BOUND_ON)
    static func formless(_ state: State) -> Bool {
        state.program == "pdi" && ProcessInfo.processInfo.operatingSystemVersion.majorVersion < 27
    }

    /// The program chip_facts.h names a state's by on this IPSW: QuartzCore 21H461's colour matrix has no clamp pair,
    /// so its form is vcm.rows (pack/fetchfree_link.py)
    static func program(_ state: State) -> String {
        state.program == "vcm" && ProcessInfo.processInfo.operatingSystemVersion.majorVersion < 27
            ? "vcm.rows" : state.program
    }
}
