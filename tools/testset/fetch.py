#!/usr/bin/env python3
# RamkolFX
# Copyright (C) 2026 Roman Levin (Coldsteel48)
#
# This file is part of RamkolFX, dual-licensed under the GNU General
# Public License v3.0 (see LICENSE) or a separate commercial license
# (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
# terms of the Contributor License Agreement (see CLA.md).

"""Fetches the Auto-detect test set (docs/content-detect-plan.md, phase 7).

Reads clips.tsv (next to this script) and downloads each clip's section
with yt-dlp into <testset>/<Class>/<name>.<ext>, where
ramkolfx-detect-score finds it. The clips are copyrighted, so they stay
outside the repo; only the list is versioned.

Clips already downloaded are skipped, so re-running after editing
clips.tsv fetches only what's new. Each clip gets a <name>.source.txt
naming the video a search resolved to: check it the first time, since a
search can land on a cover, a live take or a reaction video.

  tools/testset/fetch.py [--testset DIR] [--clips FILE] [--jobs N]
                         [--only NAME_OR_CLASS ...] [--prune] [--dry-run]

Needs yt-dlp and ffmpeg on PATH (pipx install yt-dlp).
"""

import argparse
import concurrent.futures
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

# Folder names ramkolfx-detect-score accepts: DetectedClassLabel in
# common/src/protocol.cpp.
CLASSES = [
    "Speech", "Movie", "Music", "Rock", "Metal", "Pop", "Hip-Hop", "R&B",
    "Electronic", "Jazz", "Classical", "Blues", "Reggae", "Acoustic", "Game",
]

def default_testset():
    if os.environ.get("RAMKOLFX_TESTSET"):
        return pathlib.Path(os.environ["RAMKOLFX_TESTSET"])
    data = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    return pathlib.Path(data) / "ramkolfx" / "testset"


class Clip:
    def __init__(self, cls, name, source, start, seconds, line):
        self.cls = cls
        self.name = name
        self.source = source
        self.start = start
        self.seconds = seconds
        self.line = line

    def url(self):
        if "://" in self.source:
            return self.source
        return "ytsearch1:" + self.source

    def source_txt(self, root):
        return root / self.cls / (self.name + ".source.txt")

    def path(self, root):
        """The downloaded clip, whatever its extension; None if not there."""
        for p in sorted((root / self.cls).glob(glob_escape(self.name) + ".*")):
            if not p.name.endswith(".source.txt"):
                return p
        return None


def glob_escape(name):
    return "".join("[" + ch + "]" if ch in "*?[" else ch for ch in name)


def audio_files(root):
    return [p for p in root.glob("*/*.*") if p.is_file() and not p.name.endswith(".source.txt")]


def read_clips(path):
    clips, errors, names = [], [], set()
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = [f.strip() for f in raw.split("\t")]
        where = f"{path.name}:{number}"
        if len(fields) != 5:
            errors.append(f"{where}: expected 5 tab-separated fields, got {len(fields)}")
            continue
        cls, name, source, start, seconds = fields
        if cls not in CLASSES:
            errors.append(f"{where}: unknown class '{cls}' (one of: {', '.join(CLASSES)})")
            continue
        if not name or "/" in name or name in names:
            errors.append(f"{where}: name '{name}' is empty, has a '/', or is used twice")
            continue
        try:
            start_s, seconds_s = float(start), float(seconds)
        except ValueError:
            errors.append(f"{where}: start and seconds must be numbers")
            continue
        names.add(name)
        clips.append(Clip(cls, name, source, start_s, seconds_s, number))
    return clips, errors


def fetch(clip, root):
    """Downloads one clip; returns (clip, error or None, resolved title)."""
    folder = root / clip.cls
    folder.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ramkolfx-testset-") as tmp:
        tmp = pathlib.Path(tmp)
        end = clip.start + clip.seconds
        cmd = [
            "yt-dlp", "--quiet", "--no-warnings", "--no-playlist", "--no-progress",
            "-f", "bestaudio/best",
            "--download-sections", f"*{clip.start:g}-{end:g}",
            "-o", str(tmp / "clip.%(ext)s"),
            "--print", "after_move:%(webpage_url)s\t%(title)s\t%(duration_string)s",
            clip.url(),
        ]
        result = subprocess.run(cmd, capture_output=True, text=True)
        downloaded = [p for p in tmp.iterdir() if p.name.startswith("clip.")]
        if result.returncode != 0 or not downloaded:
            message = (result.stderr.strip().splitlines() or ["no file downloaded"])[-1]
            return clip, message, None
        info = result.stdout.strip().splitlines()[-1] if result.stdout.strip() else ""
        # Kept as downloaded (Opus or AAC, usually): no second lossy
        # encode, and a tenth of the size of FLAC. The scorer decodes
        # anything ffmpeg reads.
        old = clip.path(root)
        if old:
            old.unlink()
        shutil.move(str(downloaded[0]), folder / (clip.name + downloaded[0].suffix))
    url, _, rest = info.partition("\t")
    title, _, duration = rest.partition("\t")
    clip.source_txt(root).write_text(
        f"query\t{clip.source}\nurl\t{url}\ntitle\t{title}\nduration\t{duration}\n"
        f"section\t{clip.start:g}-{clip.start + clip.seconds:g} s\n",
        encoding="utf-8")
    return clip, None, title


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--testset", type=pathlib.Path, default=default_testset(),
                        help="where clips go (default: %(default)s)")
    parser.add_argument("--clips", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parent / "clips.tsv",
                        help="clip list (default: clips.tsv next to this script)")
    parser.add_argument("--jobs", "-j", type=int, default=4, help="parallel downloads")
    parser.add_argument("--only", nargs="+", metavar="NAME_OR_CLASS",
                        help="fetch only these clips or classes (re-fetches them)")
    parser.add_argument("--prune", action="store_true",
                        help="delete clips in the test set that clips.tsv no longer lists")
    parser.add_argument("--dry-run", action="store_true", help="list what would be fetched")
    args = parser.parse_args()

    clips, errors = read_clips(args.clips)
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        return 2

    root = args.testset
    if args.prune:
        listed = {c.path(root) for c in clips}
        for path in sorted(audio_files(root)):
            if path not in listed:
                print(f"{'would delete' if args.dry_run else 'deleted'} {path}")
                if not args.dry_run:
                    path.unlink()
                    (path.parent / (path.stem + ".source.txt")).unlink(missing_ok=True)

    if args.only:
        wanted = set(args.only)
        todo = [c for c in clips if c.name in wanted or c.cls in wanted]
    else:
        todo = [c for c in clips if not c.path(root)]
    present = sum(1 for c in clips if c.path(root))
    print(f"{len(clips)} clips listed, {present} already in {root}, {len(todo)} to fetch")
    if args.dry_run:
        for c in todo:
            print(f"  {c.cls:10} {c.name:40} {c.url()}  [{c.start:g}+{c.seconds:g} s]")
        return 0
    if not todo:
        return 0
    for tool in ("yt-dlp", "ffmpeg"):
        if not shutil.which(tool):
            print(f"{tool} not found on PATH" + (" (pipx install yt-dlp)" if tool == "yt-dlp" else ""),
                  file=sys.stderr)
            return 1

    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for clip, error, title in pool.map(lambda c: fetch(c, root), todo):
            if error:
                failed.append(clip)
                print(f"x {clip.cls:10} {clip.name}: {error}")
            else:
                print(f"+ {clip.cls:10} {clip.name}  <- {title}")
    print(f"\n{len(todo) - len(failed)} fetched, {len(failed)} failed")
    if failed:
        print("failed (fix their line in clips.tsv, then re-run): " +
              ", ".join(f"{c.name} (line {c.line})" for c in failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
