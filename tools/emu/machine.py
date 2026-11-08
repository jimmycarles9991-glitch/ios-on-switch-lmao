"""One run's tegrax1plus: the boot files where hekate loads them, its hand-off in memory, the card on SDMMC1."""

from dataclasses import dataclass
from pathlib import Path

from tools.emu import hekate

IRAM_STUB = 0x40010000  # where hekate itself runs; free once it has handed over
RAM = '4G'


@dataclass
class Inputs:
    qemu: Path
    package: Path  # a //:boot output: bootloader/ and switchroot/ios/
    regwrite: Path
    sd: Path | None
    qmp: Path
    kernel: Path | None = None
    icount: bool = True


def _load(path: str, address: int) -> list[str]:
    return ['-device', f'loader,file={path},addr={address:#x},force-raw=on']


def stage(run: Path, package: Path, regwrite: Path) -> list[str]:
    """Write what hekate would have put in memory into the run directory; the loader devices that place it."""
    ios = package / 'switchroot/ios'
    stub = regwrite.read_bytes()
    if len(stub) % 8:
        raise SystemExit(f'{regwrite}: the register table must follow the code 8-byte aligned (regwrite.S)')
    files = {
        'hekate.bin': (stub + hekate.register_table(hekate.BL31), IRAM_STUB),
        'tz-params.bin': (hekate.tz_params(uart=1), hekate.TZ_PARAMS),
        'bl31.bin': ((ios / 'bl31.bin').read_bytes(), hekate.BL31),
        'bl33.bin': (hekate.bl33_with_serial((ios / 'bl33.bin').read_bytes()), hekate.BL33),
        'bl33-env.bin': (hekate.bl33_env(package / 'bootloader/ini/ios.ini'), hekate.BL33_ENV_BLOB),
    }
    args = []
    for name, (data, address) in files.items():
        (run / name).write_bytes(data)
        args += _load(name, address)
    (run / 'fuse.bin').write_bytes(hekate.fuse_page())
    return args


def command(run: Path, inputs: Inputs) -> list[str]:
    args = [
        str(inputs.qemu),
        '-M', 'tegrax1plus',
        '-m', RAM,
        '-display', 'none',
        '-qmp', f'unix:{inputs.qmp},server=on,wait=off',
        '-serial', 'file:uart-a.log',
        '-serial', 'file:uart-b.log',
        '-serial', 'file:uart-c.log',
        '-serial', 'null',
        '-d', 'guest_errors,unimp',
        '-D', 'qemu-guest.log',
        '-object', 'secret,id=tegra.fuse.cache,file=fuse.bin,format=raw',
        '-global', f'driver=tegra.evp,property=cpu-reset-vector,value={IRAM_STUB:#x}',
        '-global', 'driver=tegra.gpu,property=method-log,value=gpu-methods.raw',
        '-global', 'driver=tegra.gpu,property=event-log,value=gpu-events.log',
    ]  # fmt: skip
    args += stage(run, inputs.package, inputs.regwrite)
    if inputs.icount:
        # Guest time follows executed instructions, one nanosecond each, so neither a loaded host nor TCG's own
        # slowness can expire a driver's timeout before the work it waits for has run. With MTTCG at load ~50, and
        # with shift=auto (guest time kept to the wall clock, so each instruction took 10-40 ns), drivers panicked on
        # interrupts that had not been lost: kernel work at priority 92-95 filled all four cores past TegraI2C's
        # deadline while its work loop (81) waited. shift=0 makes guest CPU time about the A57's.
        args += ['-icount', 'shift=0']
    if inputs.sd:
        args += ['-drive', f'if=sd,index=0,format=vmdk,file={inputs.sd},snapshot=on']
    return args
