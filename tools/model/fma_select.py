# RamkolFX
# Copyright (C) 2026 Roman Levin (Coldsteel48)
#
# This file is part of RamkolFX, dual-licensed under the GNU General
# Public License v3.0 (see LICENSE) or a separate commercial license
# (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
# terms of the Contributor License Agreement (see CLA.md).

"""Pick genre-head training tracks from the Free Music Archive.

Reads FMA's tracks.csv and genres.csv (fma_metadata.zip from
https://github.com/mdeff/fma) and writes selection.tsv: track id, class,
artist, title, license - the credits a CC BY license asks for. Only tracks whose license lets the trained
model ship under RamkolFX's commercial license: CC BY (any version), CC0
and the Public Domain Mark. NonCommercial and NoDerivatives are out, and
so is ShareAlike: whether a model is an adaptation of its training data
is unsettled, and ShareAlike would then bind the model to it.

Each track gets one class from its FMA genres (see CLASS_RULES); a track
that fits two classes is skipped as ambiguous, or with --multi kept with
both ("rock,blues": either is a right answer). "other" is music none of
the classes fits (experimental, ambient, world): the head learns that
too, so it can say "no genre" instead of forcing the nearest one.

    python -I tools/model/fma_select.py --meta fma_metadata --per-class 400 --out selection.tsv
"""

import argparse
import ast
import random
import re
import sys

import pandas as pd

# Class key (DetectedClassKey) -> FMA genre titles that make a track it.
# Matched against the track's genres including their ancestors
# (genres_all), most specific rules first via EXCLUDE.
CLASS_RULES = {
    "metal": ["Metal", "Death-Metal", "Black-Metal", "Thrash", "Sludge", "Grindcore"],
    "rock": ["Rock"],
    "pop": ["Pop"],
    "hiphop": ["Hip-Hop"],
    "rnb": ["Soul-RnB", "Funk"],
    "electronic": ["Electronic"],
    "jazz": ["Jazz"],
    "classical": ["Classical"],
    "blues": ["Blues"],
    "reggae": ["Reggae - Dub", "Reggae - Dancehall"],
    "acoustic": ["Folk", "Country", "Bluegrass"],
    "other": ["Experimental", "Instrumental", "International", "Easy Listening", "Ambient Electronic",
              "Old-Time / Historic"],
}

# Genres that take a track out of a class: crossovers that would teach
# the class the wrong sound.
EXCLUDE = {
    "rock": ["Metal", "Death-Metal", "Black-Metal", "Thrash", "Sludge", "Grindcore", "Industrial", "Noise-Rock",
             "Electro-Punk", "No Wave"],
    "pop": ["Experimental Pop"],
    "rnb": ["Disco"],
    "electronic": ["Ambient Electronic", "Chiptune", "Chip Music", "Glitch", "Breakcore - Hard"],
    "other": ["Reggae - Dub", "Reggae - Dancehall"],
}

# Never trained on: speech isn't a genre, and the head only sees music.
SKIP = ["Spoken"]


def commercial_ok(license_title):
    """CC BY, CC0 or the Public Domain Mark - nothing NonCommercial,
    NoDerivatives or ShareAlike, and not 'Sound Recording Common Law
    Protection' (a recording that is *not* free)."""
    t = license_title.lower()
    if any(w in t for w in ("noncommercial", "non-commercial", "noderiv", "no deriv", "share alike",
                            "sharealike", "share-alike", "common law")):
        return False
    return t.startswith("attribution") or t.startswith("creative commons attribution") or \
        t.startswith("cc0") or t.startswith("public domain mark")


def test_set_artists(clips_tsv):
    """Artists in the test set's clip list, lower-cased, so training never
    sees them (their names lead the search, 'Artist - Title')."""
    artists = set()
    try:
        for line in open(clips_tsv, encoding="utf-8"):
            if line.startswith("#") or "\t" not in line:
                continue
            source = line.split("\t")[2]
            if " - " in source:
                artists.add(source.split(" - ")[0].strip().lower())
    except FileNotFoundError:
        pass
    return artists


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--meta", required=True, help="unpacked fma_metadata directory")
    ap.add_argument("--per-class", type=int, default=400)
    ap.add_argument("--per-artist", type=int, default=6, help="at most this many tracks by one artist per class")
    ap.add_argument("--clips", default="tools/testset/clips.tsv", help="test set list; its artists are left out")
    ap.add_argument("--multi", action="store_true", help="keep tracks that fit several classes")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    tracks = pd.read_csv(f"{args.meta}/tracks.csv", index_col=0, header=[0, 1], low_memory=False)
    genres = pd.read_csv(f"{args.meta}/genres.csv", index_col=0)
    title_of = genres["title"].to_dict()
    missing = set()
    try:
        missing = set(pd.read_pickle(f"{args.meta}/not_found.pickle").get("audio", []))
    except Exception:
        pass
    excluded_artists = test_set_artists(args.clips)

    candidates = {cls: [] for cls in CLASS_RULES}
    skipped = {"license": 0, "ambiguous": 0, "no class": 0, "test-set artist": 0, "no audio": 0}
    for track_id, row in tracks.iterrows():
        license_title = row[("track", "license")]
        if not isinstance(license_title, str) or not commercial_ok(license_title):
            skipped["license"] += 1
            continue
        if track_id in missing:
            skipped["no audio"] += 1
            continue
        all_titles = {title_of.get(g) for g in ast.literal_eval(row[("track", "genres_all")] or "[]")}
        if any(s in all_titles for s in SKIP):
            continue
        classes = [cls for cls, wanted in CLASS_RULES.items()
                   if any(w in all_titles for w in wanted) and not any(x in all_titles for x in EXCLUDE.get(cls, []))]
        specific = [c for c in classes if c != "other"]
        # Metal is also Rock in FMA's tree; EXCLUDE took rock out already.
        if len(specific) == 1 or (args.multi and specific):
            cls = ",".join(specific)
        elif not specific and classes:
            cls = "other"
        else:
            skipped["ambiguous" if specific else "no class"] += 1
            continue
        artist = str(row[("artist", "name")])
        if artist.strip().lower() in excluded_artists:
            skipped["test-set artist"] += 1
            continue
        candidates.setdefault(cls, []).append((track_id, artist, str(row[("track", "title")]), license_title))

    # Every class gets up to --per-class tracks, rarest class first, so a
    # multi-class track spent on a rare class also counts for the other.
    rng = random.Random(args.seed)
    by_class = {cls: [] for cls in CLASS_RULES}
    for key, items in candidates.items():
        for item in items:
            for cls in key.split(","):
                by_class[cls].append((key,) + item)
    count = {cls: 0 for cls in CLASS_RULES}
    per_artist = {}
    chosen, taken = [], set()
    for cls in sorted(by_class, key=lambda c: len(by_class[c])):
        items = by_class[cls]
        rng.shuffle(items)
        for key, track_id, artist, title, license_title in items:
            if count[cls] >= args.per_class:
                break
            if track_id in taken or per_artist.get((cls, artist), 0) >= args.per_artist:
                continue
            taken.add(track_id)
            chosen.append((key, track_id, artist, title, license_title))
            for c in key.split(","):
                count[c] += 1
                per_artist[(c, artist)] = per_artist.get((c, artist), 0) + 1
    print(f"{'class':12} {'usable':>7} {'artists':>8} {'chosen':>7}")
    for cls, items in by_class.items():
        print(f"{cls:12} {len(items):7} {len({i[2] for i in items}):8} {count[cls]:7}")
    print("skipped:", ", ".join(f"{k} {v}" for k, v in skipped.items()))

    clean = lambda s: re.sub(r"[\t\n\r]+", " ", s)
    with open(args.out, "w", encoding="utf-8") as out:
        out.write("track_id\tclass\tartist\ttitle\tlicense\n")
        for cls, track_id, artist, title, license_title in sorted(chosen, key=lambda c: (c[0], c[1])):
            out.write(f"{track_id}\t{cls}\t{clean(artist)}\t{clean(title)}\t{clean(license_title)}\n")
    print(f"{len(chosen)} tracks -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
