# RamkolFX
# Copyright (C) 2026 Roman Levin (Coldsteel48)
#
# This file is part of RamkolFX, dual-licensed under the GNU General
# Public License v3.0 (see LICENSE) or a separate commercial license
# (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
# terms of the Contributor License Agreement (see CLA.md).

"""Fit one weight per genre for the AudioSet genre fold.

AudioSet's genre labels run at different levels (on real music they
mostly read 0.02-0.1, and some genres sit lower than others), so the
best-average pick in DetectionSmoother favours whichever genres the
model scores high in general - Pop and Blues lose to R&B. Scaling each
genre's folded score by a weight fixes the levels with 11 numbers, which
a few dozen tracks per genre can pin down; a trained head would need
hundreds.

Steps:
  1. Score every clip in 3 s windows, 2 s apart (the fast cadence), with
     the ONNX model; cached in --cache.
  2. Fold the scores into genre classes the way the daemon does: the
     label lists are read from daemon/src/content_detection.cpp
     (DefaultFold), max over each class's labels.
  3. A track's pick is the genre with the best weight * mean score, like
     the smoother's track-long average. Fit log-weights by coordinate
     search to maximise the mean per-genre accuracy (a track with two
     genres is right on either), shrunk towards 1 so a thin class can't
     pull its weight far.
  4. Report accuracy before and after, held out by artist (5 folds), and
     print the weights for content_detection.cpp.

    python -I tools/model/calibrate_genres.py --model data/models/audio_classifier.onnx \\
        --selection selection-calib.tsv --clips fma/clips --cache fma/windows.npz
"""

import argparse
import pathlib
import re
import subprocess
import sys

import numpy as np

REPO = pathlib.Path(__file__).resolve().parents[2]
GENRES = ["rock", "metal", "pop", "hiphop", "rnb", "electronic", "jazz", "classical", "blues", "reggae", "acoustic"]
# DetectedClass enumerator name in DefaultFold -> class key.
ENUM_KEYS = {"Rock": "rock", "Metal": "metal", "Pop": "pop", "HipHop": "hiphop", "RnB": "rnb",
             "Electronic": "electronic", "Jazz": "jazz", "Classical": "classical", "Blues": "blues",
             "Reggae": "reggae", "Acoustic": "acoustic"}
RATE = 32000
STEP = 2 * RATE


def read_fold():
    """Genre key -> AudioSet labels, from DefaultFold in the daemon."""
    src = (REPO / "daemon/src/content_detection.cpp").read_text(encoding="utf-8")
    block = src[src.index("DefaultFold = {"):]
    block = block[:block.index("};")]
    fold = {}
    for enum, labels in re.findall(r"\{DetectedClass::(\w+),\s*\{(.*?)\}\}", block, re.S):
        if enum in ENUM_KEYS:
            fold[ENUM_KEYS[enum]] = re.findall(r'"([^"]+)"', labels)
    missing = [g for g in GENRES if g not in fold]
    if missing:
        sys.exit(f"DefaultFold has no entry for {missing}")
    return fold


def score_windows(model, paths, cache):
    """Per-window model scores for each path, cached by file name."""
    import onnxruntime as ort
    done = {}
    if cache.exists():
        z = np.load(cache, allow_pickle=True)
        done = dict(zip(z["names"], z["scores"]))
    session = ort.InferenceSession(str(model), providers=["CPUExecutionProvider"])
    meta = session.get_modelmeta().custom_metadata_map
    labels = meta["ramkolfx.labels"].split("\n")
    n = int(meta["ramkolfx.input_samples"])
    todo = [p for p in paths if p.name not in done]
    for i, path in enumerate(todo, 1):
        pcm = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-i", str(path), "-ac", "1", "-ar", str(RATE),
                              "-f", "f32le", "-"], capture_output=True).stdout
        audio = np.frombuffer(pcm, dtype=np.float32)
        rows = []
        for start in range(0, max(0, len(audio) - n) + 1, STEP):
            w = audio[start:start + n]
            if len(w) < n or np.sqrt(np.mean(w * w)) < 0.001:  # the daemon skips silence
                continue
            rows.append(session.run(None, {"waveform": w[None, :]})[0][0])
        done[path.name] = np.array(rows, dtype=np.float32).reshape(-1, len(labels))
        if i % 100 == 0 or i == len(todo):
            print(f"  scored {i}/{len(todo)}")
    if todo:
        np.savez(cache, names=np.array(list(done.keys()), dtype=object),
                 scores=np.array(list(done.values()), dtype=object), labels=np.array(labels))
    return done, labels


def track_means(windows, labels, fold):
    """[genres] mean folded score per track (the smoother's genre average)."""
    index = {label: i for i, label in enumerate(labels)}
    cols = [[index[l] for l in fold[g] if l in index] for g in GENRES]
    return np.array([windows[:, c].max(axis=1).mean() if len(windows) else 0.0 for c in cols])


def accuracy(log_w, X, Y):
    """Mean per-genre accuracy: a track is right if its pick is one of its
    genres. X [tracks, genres] means, Y [tracks, genres] bool."""
    picks = np.argmax(X * np.exp(log_w), axis=1)
    right = Y[np.arange(len(Y)), picks]
    per = [right[Y[:, g]].mean() for g in range(Y.shape[1]) if Y[:, g].any()]
    return float(np.mean(per)), right


def fit(X, Y, shrink, steps=np.linspace(-2.0, 2.0, 81), rounds=6):
    log_w = np.zeros(X.shape[1])
    for _ in range(rounds):
        for g in range(X.shape[1]):
            best, best_v = log_w[g], -1.0
            for v in steps:
                trial = log_w.copy()
                trial[g] = v
                score = accuracy(trial, X, Y)[0] - shrink * float(np.sum(trial ** 2))
                if score > best_v + 1e-12:
                    best, best_v = v, score
            log_w[g] = best
    return log_w


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--model", required=True)
    ap.add_argument("--selection", required=True, help="fma_select.py output (--multi)")
    ap.add_argument("--clips", required=True, help="fma_fetch.py output directory")
    ap.add_argument("--cache", required=True, help="window scores cache (.npz)")
    ap.add_argument("--shrink", type=float, default=0.002, help="pull towards weight 1 (log-weight 0)")
    ap.add_argument("--testset", help="also check on the test set's genre clips (~/.local/share/ramkolfx/testset)")
    args = ap.parse_args()

    fold = read_fold()
    rows = [l.split("\t") for l in open(args.selection, encoding="utf-8").read().splitlines()[1:]]
    rows = [r for r in rows if r[1] != "other"]
    clips = pathlib.Path(args.clips)
    paths = [clips / f"{r[0]}.mp3" for r in rows]
    windows, labels = score_windows(args.model, [p for p in paths if p.exists()], pathlib.Path(args.cache))

    X, Y, artists = [], [], []
    for r, p in zip(rows, paths):
        if p.name not in windows or len(windows[p.name]) == 0:
            continue
        X.append(track_means(windows[p.name], labels, fold))
        Y.append([g in r[1].split(",") for g in GENRES])
        artists.append(r[2])
    X, Y, artists = np.array(X), np.array(Y), np.array(artists)
    print(f"{len(X)} tracks; per genre: " + ", ".join(f"{g} {int(Y[:, i].sum())}" for i, g in enumerate(GENRES)))

    # Held out by artist: weights fit on 4/5 of the artists, scored on the rest.
    unique = np.unique(artists)
    rng = np.random.default_rng(1)
    rng.shuffle(unique)
    fold_of = {a: i % 5 for i, a in enumerate(unique)}
    folds = np.array([fold_of[a] for a in artists])
    right_before = np.zeros(len(X), bool)
    right_after = np.zeros(len(X), bool)
    for k in range(5):
        train, test = folds != k, folds == k
        w = fit(X[train], Y[train], args.shrink)
        right_before[test] = accuracy(np.zeros(len(GENRES)), X[test], Y[test])[1]
        right_after[test] = accuracy(w, X[test], Y[test])[1]
    print("\nheld out by artist (5 folds)")
    print(f"  {'genre':11} {'tracks':>6} {'before':>7} {'after':>7}")
    for i, g in enumerate(GENRES):
        m = Y[:, i]
        print(f"  {g:11} {int(m.sum()):6} {right_before[m].mean():7.0%} {right_after[m].mean():7.0%}")
    print(f"  {'mean':11} {'':6} {np.mean([right_before[Y[:, i]].mean() for i in range(len(GENRES))]):7.0%} "
          f"{np.mean([right_after[Y[:, i]].mean() for i in range(len(GENRES))]):7.0%}")

    w = fit(X, Y, args.shrink)
    print("\nweights fit on all tracks (for GenreWeight in content_detection.cpp):")
    for g, v in zip(GENRES, np.exp(w)):
        print(f"  {g:11} {v:.2f}")

    if args.testset:
        root = pathlib.Path(args.testset).expanduser()
        names = {"Rock": "rock", "Metal": "metal", "Pop": "pop", "Hip-Hop": "hiphop", "R&B": "rnb",
                 "Electronic": "electronic", "Jazz": "jazz", "Classical": "classical", "Blues": "blues",
                 "Reggae": "reggae", "Acoustic": "acoustic"}
        tpaths, ty = [], []
        for folder, key in names.items():
            for p in sorted((root / folder).glob("*")):
                if not p.name.endswith(".source.txt"):
                    tpaths.append(p)
                    ty.append([g == key for g in GENRES])
        twin, _ = score_windows(args.model, tpaths, pathlib.Path(args.cache).with_name("testset-windows.npz"))
        TX = np.array([track_means(twin[p.name], labels, fold) for p in tpaths])
        TY = np.array(ty)
        before = accuracy(np.zeros(len(GENRES)), TX, TY)[1]
        after = accuracy(w, TX, TY)[1]
        print("\ntest set genre clips (pick from the whole-clip mean; ramkolfx-detect-score has the real numbers)")
        for i, g in enumerate(GENRES):
            m = TY[:, i]
            print(f"  {g:11} {int(before[m].sum())}/{int(m.sum())} -> {int(after[m].sum())}/{int(m.sum())}")
        print(f"  {'all':11} {int(before.sum())}/{len(TY)} -> {int(after.sum())}/{len(TY)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
