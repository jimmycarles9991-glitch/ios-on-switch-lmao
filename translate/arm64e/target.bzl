"""The marker of a binary built for the Cortex-A57: a section __TEXT,__a64x_target (<switch/a64x.h>). xnu-12377's a64x
loader maps a marked image as is; an unmarked arm64 one has its whole text read and scanned at every load. Our iOS
user-space links (the toolchain's, ios_binary's, the Metal plug-in's) add it where that loader exists."""

def a64x_target_linkopts():
    """The linker flags that mark an image, under xnu-12377; none under a kernel without the loader."""
    return select({
        "//kernel:xnu_12377_121_6": ["-Wl,-sectcreate,__TEXT,__a64x_target,/dev/null"],
        "//conditions:default": [],
    })
