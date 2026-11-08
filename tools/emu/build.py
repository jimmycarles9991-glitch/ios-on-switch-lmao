"""bazel run //tools/emu:build: build the patched tegra_qemu into the shared cache, if needed, and print its path."""

from tools.emu import qemu


def main() -> None:
    print(qemu.ensure())


if __name__ == '__main__':
    main()
