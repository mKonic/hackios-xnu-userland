#!/usr/bin/env python3
"""Copy iOS's launchd with more time for an app extension to check in.

usage: launchd_watchdog.py LAUNCHD OUT [SECONDS]

launchd kills an app extension that has not checked in 5 seconds after it was spawned ("extension check-in
timeout", reason namespace 7 code 3; boot-arg exitlog=1 shows them). Under QEMU a SwiftUI widget or a wallpaper
poster takes longer than that to load, so every one of them was killed before it could answer, spawned again
and killed again: the home screen's widgets stayed white, the wallpaper black, and loading them over and over
took a third of the processors.

The 5 is launchd's default for ExtensionWatchdogTimeout, which its own settings (the plist in its
__TEXT,__config) do not set, so launchd uses the int its __DATA holds, beside SIGTERMTimeout's 3. This writes
OUT as LAUNCHD with that int changed (default 60), after making sure it is the launchd this was worked out on
(iOS 16.5, 20F66): the settings' size and both defaults where they should be; any other launchd is copied as
it is. OUT is a new file: LAUNCHD may be a hard link into the extracted rootfs. The kernel enforces code
signatures on the shared cache only, so the changed page is not refused.
"""
import shutil
import struct
import sys

CONFIG = (0x6085f, 0x2056)              # __TEXT,__config: file offset, size
WATCHDOG = 442368 + 0x1790              # __DATA's file offset + ExtensionWatchdogTimeout's default
SIGTERM = WATCHDOG - 4                  # SIGTERMTimeout's default


def main():
    launchd, out = sys.argv[1], sys.argv[2]
    seconds = int(sys.argv[3]) if len(sys.argv) > 3 else 60
    data = bytearray(open(launchd, 'rb').read())
    config = data[CONFIG[0]:CONFIG[0] + CONFIG[1]]
    if (len(data) > WATCHDOG + 4 and config.startswith(b'<?xml') and b'ExtensionWatchdog' in config and
            struct.unpack_from('<ii', data, SIGTERM) == (3, 5)):
        struct.pack_into('<i', data, WATCHDOG, seconds)
        print(f'launchd_watchdog.py: extensions have {seconds} s to check in')
    else:
        print('launchd_watchdog.py: not the launchd expected, copied as it is', file=sys.stderr)
    with open(out + '.new', 'wb') as f:
        f.write(data)
    shutil.copymode(launchd, out + '.new')
    shutil.move(out + '.new', out)


if __name__ == '__main__':
    main()
