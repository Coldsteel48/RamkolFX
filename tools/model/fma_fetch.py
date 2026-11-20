# RamkolFX
# Copyright (C) 2026 Roman Levin (Coldsteel48)
#
# This file is part of RamkolFX, dual-licensed under the GNU General
# Public License v3.0 (see LICENSE) or a separate commercial license
# (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
# terms of the Contributor License Agreement (see CLA.md).

"""Fetch the tracks fma_select.py picked, without the whole archive.

fma_large.zip holds a 30 s clip of every FMA track (100 GB). The mirror
serves byte ranges, so this reads the zip's directory and then only the
members listed in selection.tsv, into <out>/<track id>.mp3. Tracks
already there are skipped.

    python -I tools/model/fma_fetch.py --selection selection.tsv --out fma/clips
"""

import argparse
import bz2
import concurrent.futures
import io
import pathlib
import sys
import urllib.request
import zipfile
import zlib

URL = "https://os.unil.cloud.switch.ch/fma/fma_large.zip"


def fetch_range(url, start, end):
    """Bytes start..end (inclusive) of url, retried on network errors."""
    req = urllib.request.Request(url, headers={"Range": f"bytes={start}-{end}"})
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                return r.read()
        except OSError:
            if attempt == 3:
                raise


class HttpRangeFile(io.RawIOBase):
    """A read-only, seekable file over HTTP range requests, with a small
    block cache - enough for zipfile to read the directory at the end."""

    BLOCK = 1 << 20

    def __init__(self, url):
        self.url = url
        req = urllib.request.Request(url, method="HEAD")
        with urllib.request.urlopen(req, timeout=60) as r:
            self.size = int(r.headers["Content-Length"])
        self.pos = 0
        self.cache = {}

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self.pos

    def seek(self, offset, whence=io.SEEK_SET):
        base = {io.SEEK_SET: 0, io.SEEK_CUR: self.pos, io.SEEK_END: self.size}[whence]
        self.pos = max(0, base + offset)
        return self.pos

    def fetch(self, start, end):
        return fetch_range(self.url, start, end)

    def block(self, index):
        if index not in self.cache:
            if len(self.cache) > 64:
                self.cache.clear()
            start = index * self.BLOCK
            self.cache[index] = self.fetch(start, min(self.size, start + self.BLOCK) - 1)
        return self.cache[index]

    def read(self, n=-1):
        if n is None or n < 0:
            n = self.size - self.pos
        n = min(n, self.size - self.pos)
        out = bytearray()
        while n > 0:
            block = self.block(self.pos // self.BLOCK)
            offset = self.pos % self.BLOCK
            chunk = block[offset:offset + n]
            out += chunk
            self.pos += len(chunk)
            n -= len(chunk)
        return bytes(out)

    def readinto(self, b):
        data = self.read(len(b))
        b[:len(data)] = data
        return len(data)


def member_of(track_id):
    name = f"{track_id:06d}"
    return f"fma_large/{name[:3]}/{name}.mp3"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--selection", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", "-j", type=int, default=8)
    args = ap.parse_args()

    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    ids = []
    for line in open(args.selection, encoding="utf-8").read().splitlines()[1:]:
        track_id = int(line.split("\t")[0])
        if not (out / f"{track_id}.mp3").exists():
            ids.append(track_id)
    print(f"{len(ids)} tracks to fetch into {out}")
    if not ids:
        return 0

    print("reading the archive's directory...")
    index = zipfile.ZipFile(HttpRangeFile(URL))
    infos = {i.filename: i for i in index.infolist()}
    print(f"{len(infos)} members")

    def fetch_one(track_id):
        info = infos.get(member_of(track_id))
        if info is None:
            return track_id, "not in the archive"
        if info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED, zipfile.ZIP_BZIP2):
            return track_id, f"compression method {info.compress_type} not handled"
        # The member's local header (its name and extra field can differ
        # in length from the directory's), then the data right after it.
        header = fetch_range(URL, info.header_offset, info.header_offset + 29)
        name_len = int.from_bytes(header[26:28], "little")
        extra_len = int.from_bytes(header[28:30], "little")
        start = info.header_offset + 30 + name_len + extra_len
        data = fetch_range(URL, start, start + info.compress_size - 1)
        if len(data) != info.compress_size:
            return track_id, "short read"
        if info.compress_type == zipfile.ZIP_DEFLATED:
            data = zlib.decompressobj(-zlib.MAX_WBITS).decompress(data)
        elif info.compress_type == zipfile.ZIP_BZIP2:  # what fma_large.zip uses
            data = bz2.decompress(data)
        if len(data) != info.file_size or zlib.crc32(data) != info.CRC:
            return track_id, "corrupt (size or CRC mismatch)"
        tmp = out / f"{track_id}.mp3.part"
        tmp.write_bytes(data)
        tmp.rename(out / f"{track_id}.mp3")
        return track_id, None

    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for done, (track_id, error) in enumerate(pool.map(fetch_one, ids), 1):
            if error:
                failed += 1
                print(f"x {track_id}: {error}")
            if done % 100 == 0:
                print(f"  {done}/{len(ids)}")
    print(f"{len(ids) - failed} fetched, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
