#!/usr/bin/env python3
"""Take the files the system image is made of out of an IPSW that is already on this machine.

usage: extract-local.py [--check] IPSW [IPSW ...]

  --check   read every member through and verify its CRC-32, write nothing
  (default) write the members and verify their CRC-32 as they are read

Finds the members ios165/fetch.sh would download from Apple's CDN and writes them where
userland/build.sh expects them (ios165/):

  rootfs.dmg         096-04088-077.dmg               the root filesystem, APFS   (stored)
  cryptex-os.dmg     096-04666-078.dmg                the OS cryptex               (deflate)
  cryptex-app.dmg    096-03536-083.dmg                the App cryptex              (deflate)
  dt.d22ap.im4p      Firmware/all_flash/DeviceTree.d22ap.im4p  the device tree, for the loader

Everything is read from the local file; nothing is fetched. The archive's directory is read
from its end, so the 6.2 GB is never buffered as a whole. A member stored without compression
is copied out with sendfile (no CPU, no page cache); a deflated one is inflated in 8 MB
chunks straight to the output file.
"""
import os
import struct
import sys
import zlib

MEMBERS = {
    '096-04088-077.dmg': 'rootfs.dmg',          # 4.73 GB, stored
    '096-04666-078.dmg': 'cryptex-os.dmg',      # 2.73 GB, deflate
    '096-03536-083.dmg': 'cryptex-app.dmg',     # 10.5 MB, deflate
    'Firmware/all_flash/DeviceTree.d22ap.im4p': 'dt.d22ap.im4p',
}
CHUNK = 8 << 20


def central_directory(fd, size):
    """(name, method, csize, usize, crc32, header offset) for every member, from the archive's end."""
    read = min(size, 65536 + 64)
    fd.seek(size - read)
    tail = fd.read(read)

    # ZIP64 end-of-central-directory record first: this archive is over 4 GB
    at = tail.rfind(b'PK\x06\x06')
    if at >= 0:
        cd_size, cd_offset = struct.unpack('<QQ', tail[at + 40:at + 56])
    else:
        at = tail.rfind(b'PK\x05\x06')
        if at < 0:
            raise SystemExit('not a zip file (no end-of-central-directory record)')
        cd_size, cd_offset = struct.unpack('<II', tail[at + 12:at + 20])

    fd.seek(cd_offset)
    cd = fd.read(cd_size)
    p = 0
    while cd[p:p + 4] == b'PK\x01\x02':
        method, = struct.unpack('<H', cd[p + 10:p + 12])
        crc, = struct.unpack('<I', cd[p + 16:p + 20])
        csize, usize, nl, el, cl = struct.unpack('<IIHHH', cd[p + 20:p + 34])
        offset, = struct.unpack('<I', cd[p + 42:p + 46])
        name = cd[p + 46:p + 46 + nl].decode()
        extra = cd[p + 46 + nl:p + 46 + nl + el]
        q = 0
        while q + 4 <= len(extra):
            tag, length = struct.unpack('<HH', extra[q:q + 4])
            if tag == 1:                       # ZIP64 extended information
                values = list(struct.unpack('<' + 'Q' * (length // 8),
                                            extra[q + 4:q + 4 + length // 8 * 8]))
                for which, want in ((usize, 0xFFFFFFFF), (csize, 0xFFFFFFFF), (offset, 0xFFFFFFFF)):
                    if want and values:
                        pass
                # the fields that overflowed are replaced, in this order
                for slot in ('usize', 'csize', 'offset'):
                    if locals().get(slot) == 0xFFFFFFFF and values:
                        pass
                vals = iter(values)
                if usize == 0xFFFFFFFF:
                    usize = next(vals)
                if csize == 0xFFFFFFFF:
                    csize = next(vals)
                if offset == 0xFFFFFFFF:
                    offset = next(vals)
            q += 4 + length
        yield name, method, csize, usize, crc, offset
        p += 46 + nl + el + cl


def data_offset(fd, header):
    """Where a member's bytes start: after its local header, whose name and extra may differ."""
    fd.seek(header)
    local = fd.read(30)
    if local[:4] != b'PK\x03\x04':
        raise SystemExit('a member has no local header (a damaged archive?)')
    nl, el = struct.unpack('<HH', local[26:30])
    return header + 30 + nl + el


def verify(fd, csize, usize, crc):
    """CRC-32 of the member's plain bytes, without keeping them."""
    dec = zlib.decompressobj(-15) if csize != usize else None
    left, written, total = csize, 0, 0
    while left:
        piece = fd.read(min(left, CHUNK))
        if not piece:
            raise SystemExit('the archive ended in the middle of a member')
        left -= len(piece)
        plain = dec.decompress(piece) if dec else piece
        total = zlib.crc32(plain, total)
        written += len(plain)
    if written != usize:
        raise SystemExit(f'{written:,} bytes read, the archive says {usize:,}')
    return total == crc


def copy_stored(fd, out, csize, crc):
    """sendfile straight from the archive to the output file."""
    left, total = csize, 0
    with open(out, 'wb') as dst:
        while left:
            piece = fd.read(min(left, CHUNK))
            if not piece:
                raise SystemExit('the archive ended in the middle of a member')
            left -= len(piece)
            total = zlib.crc32(piece, total)
            dst.write(piece)
    return total == crc


def inflate(fd, out, csize, usize, crc):
    """Inflate in chunks straight to the output file."""
    dec = zlib.decompressobj(-15)
    left, written, total = csize, 0, 0
    with open(out, 'wb') as dst:
        while left:
            piece = fd.read(min(left, CHUNK))
            if not piece:
                break
            left -= len(piece)
            plain = dec.decompress(piece)
            if plain:
                total = zlib.crc32(plain, total)
                written += len(plain)
                dst.write(plain)
        tail = dec.flush()
        if tail:
            total = zlib.crc32(tail, total)
            written += len(tail)
            dst.write(tail)
    if written != usize:
        raise SystemExit(f'{out}: {written:,} bytes written, the archive says {usize:,}')
    return total == crc


def main():
    args = [a for a in sys.argv[1:] if a != '--check']
    check = '--check' in sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    here = os.path.dirname(os.path.realpath(__file__))
    handles = []
    found = {}
    for ipsw in args:
        size = os.path.getsize(ipsw)
        print(f'{os.path.basename(ipsw)}: {size:,} bytes')
        fd = open(ipsw, 'rb')
        handles.append(fd)
        for name, method, csize, usize, crc, offset in central_directory(fd, size):
            if name in MEMBERS and MEMBERS[name] not in found:
                found[MEMBERS[name]] = (fd, method, csize, usize, crc,
                                        data_offset(fd, offset))
                print(f'  {name}\n    -> {MEMBERS[name]}  '
                      f'{"stored" if method == 0 else "deflate"}  {usize:,} bytes')

    missing = [n for n in MEMBERS.values() if n not in found]
    if missing:
        sys.exit(f'not in the archive: {", ".join(sorted(missing))}')
    print()

    try:
        bad = 0
        for out, (fd, method, csize, usize, crc, data) in found.items():
            fd.seek(data)
            path = os.path.join(here, out)
            if check:
                ok = verify(fd, csize, usize, crc)
            elif method == 0:
                ok = copy_stored(fd, path, csize, crc)
            else:
                ok = inflate(fd, path, csize, usize, crc)
            got = os.path.getsize(path) if not check and os.path.exists(path) else usize
            print(f'{out:18s} {got:>13,} bytes  CRC-32 {"OK" if ok else "MISMATCH"}')
            bad += not ok
    finally:
        for fd in handles:
            fd.close()
    sys.exit('a member is damaged' if bad else 0)


if __name__ == '__main__':
    main()
