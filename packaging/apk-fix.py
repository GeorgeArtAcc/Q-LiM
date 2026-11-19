#!/usr/bin/env python3
# Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
#
# 2.0.0: makes the .apk from nfpm acceptable for apk v3 (Alpine 3.23+): "v2 package format error".
# nfpm writes the mode of folder entries with Go's "this is a folder" flag (0x80000000) - it does not fit the 8-byte
# octal field, so it goes in as a binary (base-256) number. apk v2 let it pass, apk v3 checks that every number field
# is plain octal. Here: every tar header gets its plain mode (& 07777, the type is already in the type byte), the header
# checksum is written again, and because the data part changes, its sha256 ("datahash" in .PKGINFO) too.
# Usage: apk-fix.py FILE.apk   (rewritten in place; checked by reading it back)
import gzip, hashlib, re, sys, zlib


def split_gzip(data):
    out, pos = [], 0
    while pos < len(data):
        d = zlib.decompressobj(16 + zlib.MAX_WBITS)
        raw = d.decompress(data[pos:]) + d.flush()
        used = len(data) - pos - len(d.unused_data)
        out.append(raw)
        pos += used
    return out


def octal_field(value, length):
    s = ("%0" + str(length - 1) + "o") % value
    if len(s) > length - 1:
        raise ValueError("value too big for an octal field")
    return s.encode() + b"\0"


def fix_tar(raw):
    """Rewrites the mode of every header that is not plain octal; returns (new bytes, how many were fixed)."""
    b = bytearray(raw)
    p, fixed = 0, 0
    while p + 512 <= len(b):
        h = b[p:p + 512]
        if h == b"\0" * 512:
            p += 512
            continue
        mode = bytes(h[100:108])
        if mode[0] & 0x80:                                   # base-256 number
            val = int.from_bytes(mode[1:], "big") & 0o7777
            b[p + 100:p + 108] = octal_field(val, 8)
            b[p + 148:p + 156] = b" " * 8                    # the checksum is counted with spaces in its own field
            s = sum(b[p:p + 512])
            b[p + 148:p + 156] = ("%06o" % s).encode() + b"\0 "
            fixed += 1
        sz = h[124:136]
        if sz[0] & 0x80:
            raise ValueError("a binary size field - not expected")
        size = int(sz.strip(b"\0 ") or b"0", 8)
        p += 512 + ((size + 511) // 512) * 512
    return bytes(b), fixed


def main(path):
    data = open(path, "rb").read()
    parts = split_gzip(data)
    if len(parts) != 2:
        sys.exit("apk-fix: expected 2 parts (control, data), found %d" % len(parts))
    control, payload = parts
    payload, nd = fix_tar(payload)
    control, nc = fix_tar(control)
    data_gz = gzip.compress(payload, 9, mtime=0)
    digest = hashlib.sha256(data_gz).hexdigest()
    # the new datahash in .PKGINFO (same length -> the tar sizes do not change)
    m = re.search(rb"datahash = ([0-9a-f]{64})", control)
    if not m:
        sys.exit("apk-fix: no datahash in .PKGINFO")
    control = control[:m.start(1)] + digest.encode() + control[m.end(1):]
    # .PKGINFO's header checksum is unchanged (only the contents changed); write it all back
    control_gz = gzip.compress(control, 9, mtime=0)
    open(path, "wb").write(control_gz + data_gz)
    # check: no binary mode is left
    for part in split_gzip(open(path, "rb").read()):
        _, left = fix_tar(part)
        if left:
            sys.exit("apk-fix: binary modes left")
    print("apk-fix: %s - fixed %d data + %d control headers" % (path, nd, nc))


if __name__ == "__main__":
    main(sys.argv[1])
