"""Build xnu-12377 objects only when the selected IPSW uses that XNU version."""

def selected_only():
    return select({
        "//kernel:xnu_12377_121_6": [],
        "//conditions:default": ["@platforms//:incompatible"],
    })
