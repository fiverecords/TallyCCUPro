#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Build the gzip copies of the TallyCCU Pro web pages for the SD card.

For every .html file it writes '<name>.html.gz' (deflate level 9, fixed
header: mtime 0, no file name) next to the original. Copy both to the SD
card: the firmware sends the .gz to browsers that accept gzip and the
plain file to the rest.

A .gz that does not match its .html is served as is, so regenerate (or
delete) the .gz every time a page changes. --check verifies that.
Uploading, renaming or deleting a page from the SD manager (sdcard.html)
deletes its .gz, and the plain page is served: upload the .html first,
then its .gz.

Usage:
  python3 tools/gzip_sdcard.py                 # all .html in sdcard/
  python3 tools/gzip_sdcard.py E:\\            # straight on the SD card
  python3 tools/gzip_sdcard.py sdcard/index.html
  python3 tools/gzip_sdcard.py --check         # exit 1 if any is stale

Standard library only (Python 3.6+).
"""

import argparse
import gzip
import os
import struct
import sys
import zlib

EXTENSIONS = ('.html',)
GZ_EXTENSIONS = tuple(e + '.gz' for e in EXTENSIONS)
DEFAULT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           os.pardir, 'sdcard')


def gzip_bytes(data):
    """gzip -9 -n equivalent with a fixed header (mtime 0, OS unknown),
    so the output does not depend on the clock or the platform."""
    co = zlib.compressobj(9, zlib.DEFLATED, -zlib.MAX_WBITS, 9)
    body = co.compress(data) + co.flush()
    header = b'\x1f\x8b\x08\x00' + struct.pack('<I', 0) + b'\x02\xff'
    trailer = struct.pack('<II', zlib.crc32(data) & 0xffffffff,
                          len(data) & 0xffffffff)
    return header + body + trailer


def gz_matches(src_data, gz_path):
    """True if gz_path decompresses to exactly src_data. Compares content,
    not bytes, since other zlib builds may compress differently."""
    try:
        with open(gz_path, 'rb') as f:
            return gzip.decompress(f.read()) == src_data
    except (OSError, EOFError, zlib.error):
        return False


def collect(paths):
    """(sources, orphans, errors) for the given files and directories.
    Orphans are .gz files in a scanned directory without their source."""
    sources, orphans, errors = [], [], []
    for path in paths:
        if os.path.isdir(path):
            names = sorted(os.listdir(path))
            found = [n for n in names if n.lower().endswith(EXTENSIONS)
                     and os.path.isfile(os.path.join(path, n))]
            sources += [os.path.join(path, n) for n in found]
            have = {n.lower() for n in found}     # FAT ignores case
            orphans += [os.path.join(path, n) for n in names
                        if n.lower().endswith(GZ_EXTENSIONS)
                        and n[:-3].lower() not in have]
        elif os.path.isfile(path):
            if path.lower().endswith('.gz'):
                errors.append(f"{path}: give the source file, not the .gz")
            else:
                sources.append(path)
        else:
            errors.append(f"{path}: not found")
    return sources, orphans, errors


def write_atomic(path, data):
    tmp = path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, path)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Write <file>.gz next to each .html for the SD card.")
    ap.add_argument('paths', nargs='*',
                    help="files or directories (default: the repo's sdcard/)")
    ap.add_argument('--check', action='store_true',
                    help="write nothing; exit 1 if a .gz is missing or stale")
    ap.add_argument('--force', action='store_true',
                    help="rewrite .gz files that are already up to date")
    ap.add_argument('-q', '--quiet', action='store_true',
                    help="only print problems")
    args = ap.parse_args(argv)

    paths = args.paths or [os.path.normpath(DEFAULT_DIR)]
    sources, orphans, errors = collect(paths)
    for e in errors:
        print(f"error: {e}", file=sys.stderr)
    if errors:
        return 2
    if not sources:
        print("error: no .html files found in " + ", ".join(paths),
              file=sys.stderr)
        return 2

    bad = 0
    for src in sources:
        gz_path = src + '.gz'
        with open(src, 'rb') as f:
            data = f.read()
        current = gz_matches(data, gz_path)

        if args.check:
            if not current:
                bad += 1
                state = 'stale' if os.path.exists(gz_path) else 'missing'
                print(f"{state}: {gz_path}")
            elif not args.quiet:
                print(f"ok: {gz_path}")
            continue

        if current and not args.force:
            if not args.quiet:
                print(f"up to date: {gz_path}")
            continue
        packed = gzip_bytes(data)
        write_atomic(gz_path, packed)
        if not args.quiet:
            ratio = len(data) / len(packed) if packed else 0
            print(f"wrote {gz_path}: {len(data)} -> {len(packed)} B "
                  f"({ratio:.1f}x)")

    for gz_path in orphans:
        # The firmware would still serve it to gzip browsers
        print(f"orphan (no source, delete it): {gz_path}")
        if args.check:
            bad += 1

    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
