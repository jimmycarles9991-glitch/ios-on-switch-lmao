"""The prelink's own options: //kernel:kernel uses them, and //install:release_inputs carries them so a Mac without
Xcode can prelink the released kernel the same way. //kernel:kernel_12377 takes the same base_roots."""

load("//kernel/rules:kernel.bzl", "kernel_prelink")

PRELINK = {
    "base_roots": [
        "com.apple.driver.AppleMobileFileIntegrity",
        "com.apple.filesystems.apfs",
        "com.apple.security.sandbox",
        "com.apple.AppleFSCompression.AppleFSCompressionTypeZlib",
        "com.apple.iokit.IOMobileGraphicsFamily",
        "com.apple.iokit.IOSurface",
        "com.apple.iokit.IOHIDFamily",
        "com.apple.driver.DiskImages",
        "com.apple.driver.DiskImages.FileBackingStore",
        "com.apple.driver.DiskImages.KernelBacked",
        "com.apple.driver.DiskImages.ReadWriteDiskImage",
    ],
    "early_security_init": ["com.apple.driver.AppleMobileFileIntegrity=0xfffffff005ab4ae0"],
    "slide": "-0x1000000",
}

def kernel_prelink_switch(name, **kwargs):
    kernel_prelink(name = name, **(PRELINK | kwargs))
