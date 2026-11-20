# RamkolFX
# Copyright (C) 2026 Roman Levin (Coldsteel48)
#
# This file is part of RamkolFX, dual-licensed under the GNU General
# Public License v3.0 (see LICENSE) or a separate commercial license
# (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
# terms of the Contributor License Agreement (see CLA.md).

"""Train and judge a genre head on an EfficientAT model's embedding.

The AudioSet genre labels are thin, so instead of folding them, a small
classifier reads the model's pooled embedding (960 numbers for mn10)
and picks one of the 11 genres or "other" (music with no genre class).

  1. Embed every clip in windows like the daemon's (--seconds long, 2 s
     apart, silent ones skipped), through the same front end and tiling
     as export_efficientat.py; cached per model in --cache-dir.
  2. Train a softmax head (linear, or one hidden layer) on the training
     tracks' windows; a track with two genres is half one, half the other.
  3. Judge tracks by the mean of their windows' probabilities (the
     smoother's track-long average): held out by artist on the training
     set (5 folds), then trained on all of it and checked on the test
     set's genre and Music clips, next to the AudioSet fold's pick for
     the same windows.

    python -I tools/model/genre_head.py --repo EfficientAT --weights mn10_as_mAP_471.pt \\
        --selection selection-calib.tsv --clips fma/clips --testset ~/.local/share/ramkolfx/testset \\
        --cache-dir fma/embeddings --save head.pt
"""

import argparse
import pathlib
import subprocess
import sys

import numpy as np
import torch
import torch.nn as nn

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import export_efficientat as ex  # noqa: E402
from calibrate_genres import GENRES, read_fold  # noqa: E402

CLASSES = GENRES + ["other"]
TESTSET_FOLDERS = {"Rock": "rock", "Metal": "metal", "Pop": "pop", "Hip-Hop": "hiphop", "R&B": "rnb",
                   "Electronic": "electronic", "Jazz": "jazz", "Classical": "classical", "Blues": "blues",
                   "Reggae": "reggae", "Acoustic": "acoustic", "Music": "other"}


class Embedder(nn.Module):
    """export_efficientat.Classifier, returning the pooled embedding and
    the AudioSet probabilities."""

    def __init__(self, net, frames, context_frames=500):
        super().__init__()
        self.front = ex.MelFrontEnd()
        self.net = net
        self.repeats = max(1, -(-context_frames // frames))
        self.context = max(frames, context_frames)

    def forward(self, waveform):  # [B, N]
        spec = self.front(waveform)
        if self.repeats > 1:
            spec = torch.cat([spec] * self.repeats, dim=-1)[..., :self.context]
        logits, features = self.net(spec.unsqueeze(1))
        return features, torch.sigmoid(logits)


def load_embedder(args):
    get_model, _ = ex.import_efficientat(args.repo, args.arch)
    net = get_model(width_mult=args.width, pretrained_name=None, head_type="mlp")
    net.load_state_dict(torch.load(args.weights, map_location="cpu", weights_only=True))
    net.eval()
    n = int(round(args.seconds * ex.SAMPLE_RATE))
    return Embedder(net, 1 + (n - 1) // ex.HOP).eval(), n


def embed(model, n, path):
    pcm = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-i", str(path), "-ac", "1", "-ar",
                          str(ex.SAMPLE_RATE), "-f", "f32le", "-"], capture_output=True).stdout
    audio = np.frombuffer(pcm, dtype=np.float32)
    windows = [audio[s:s + n] for s in range(0, max(0, len(audio) - n) + 1, 2 * ex.SAMPLE_RATE)]
    windows = [w for w in windows if len(w) == n and np.sqrt(np.mean(w * w)) >= 0.001]
    if not windows:
        return np.zeros((0, 1)), np.zeros((0, 527))
    with torch.no_grad():
        feats, probs = [], []
        for i in range(0, len(windows), 16):
            f, p = model(torch.from_numpy(np.stack(windows[i:i + 16])))
            feats.append(f.reshape(len(windows[i:i + 16]), -1).numpy())
            probs.append(p.numpy())
    return np.concatenate(feats), np.concatenate(probs)


def embed_all(model, n, items, cache):
    """items: [(key, path)] -> {key: (features, probs)}, cached."""
    done = {}
    if cache.exists():
        z = np.load(cache, allow_pickle=True)
        done = {k: (f, p) for k, f, p in zip(z["keys"], z["feats"], z["probs"])}
    todo = [(k, p) for k, p in items if k not in done]
    for i, (key, path) in enumerate(todo, 1):
        done[key] = embed(model, n, path)
        if i % 100 == 0 or i == len(todo):
            print(f"  embedded {i}/{len(todo)}", flush=True)
    if todo:
        keys = list(done)
        np.savez(cache, keys=np.array(keys, dtype=object),
                 feats=np.array([done[k][0] for k in keys], dtype=object),
                 probs=np.array([done[k][1] for k in keys], dtype=object))
    return done


def make_head(dim, hidden):
    if hidden:
        return nn.Sequential(nn.Linear(dim, hidden), nn.ReLU(), nn.Dropout(0.3), nn.Linear(hidden, len(CLASSES)))
    return nn.Linear(dim, len(CLASSES))


def train(feats, targets, track_of, hidden, epochs, decay, seed=0):
    """Windows feats [W, D] with per-window target distributions [W, C].
    Each track weighs the same, and each class the same in total."""
    torch.manual_seed(seed)
    mean, std = feats.mean(0), feats.std(0) + 1e-6
    x = torch.from_numpy((feats - mean) / std).float()
    y = torch.from_numpy(targets).float()
    windows_per_track = np.bincount(track_of)[track_of]
    class_mass = targets.T @ (1.0 / windows_per_track)  # tracks per class
    class_weight = np.where(class_mass > 0, class_mass.sum() / (len(class_mass) * np.maximum(class_mass, 1e-9)), 0)
    w = torch.from_numpy((targets @ class_weight) / windows_per_track).float()
    head = make_head(x.shape[1], hidden)
    opt = torch.optim.AdamW(head.parameters(), lr=1e-3, weight_decay=decay)
    for _ in range(epochs):
        perm = torch.randperm(len(x))
        head.train()
        for i in range(0, len(x), 256):
            idx = perm[i:i + 256]
            loss = -(w[idx] * (y[idx] * torch.log_softmax(head(x[idx]), 1)).sum(1)).sum() / w[idx].sum()
            opt.zero_grad()
            loss.backward()
            opt.step()
    head.eval()
    return head, mean, std


def track_probs(head, mean, std, feats):
    with torch.no_grad():
        x = torch.from_numpy((feats - mean) / std).float()
        return torch.softmax(head(x), 1).mean(0).numpy()


def fold_pick(probs, labels, fold):
    """The AudioSet fold's pick: best mean of max-over-labels per genre."""
    index = {l: i for i, l in enumerate(labels)}
    means = [probs[:, [index[l] for l in fold[g] if l in index]].max(1).mean() for g in GENRES]
    return int(np.argmax(means))


def report(title, picks, truth):
    """picks: [tracks] class index; truth: [tracks, C] bool."""
    right = truth[np.arange(len(picks)), picks]
    print(title)
    per = []
    for c, name in enumerate(CLASSES):
        m = truth[:, c]
        if m.any():
            per.append(right[m].mean())
            print(f"  {name:11} {int(right[m].sum()):3}/{int(m.sum()):<3} {right[m].mean():5.0%}")
    print(f"  {'all':11} {int(right.sum()):3}/{len(right):<3} {right.mean():5.0%}   mean per class {np.mean(per):.0%}")
    return right


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--repo", required=True)
    ap.add_argument("--weights", required=True)
    ap.add_argument("--arch", default="mn")
    ap.add_argument("--width", type=float, default=1.0)
    ap.add_argument("--seconds", type=float, default=3.0)
    ap.add_argument("--selection", required=True)
    ap.add_argument("--clips", required=True)
    ap.add_argument("--testset", required=True)
    ap.add_argument("--cache-dir", required=True)
    ap.add_argument("--hidden", type=int, default=0, help="hidden units (0: linear head)")
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--decay", type=float, default=0.05)
    ap.add_argument("--save", help="write the head trained on everything here")
    args = ap.parse_args()

    torch.set_num_threads(max(1, torch.get_num_threads()))
    model, n = load_embedder(args)
    tag = f"{pathlib.Path(args.weights).stem}_{args.seconds:g}s"
    cache_dir = pathlib.Path(args.cache_dir)
    cache_dir.mkdir(parents=True, exist_ok=True)

    rows = [l.split("\t") for l in open(args.selection, encoding="utf-8").read().splitlines()[1:]]
    clips = pathlib.Path(args.clips)
    train_items = [(r[0], clips / f"{r[0]}.mp3") for r in rows if (clips / f"{r[0]}.mp3").exists()]
    data = embed_all(model, n, train_items, cache_dir / f"fma-{tag}.npz")
    root = pathlib.Path(args.testset).expanduser()
    test_items = [(f"{folder}/{p.name}", p) for folder in TESTSET_FOLDERS for p in sorted((root / folder).glob("*"))
                  if not p.name.endswith(".source.txt")]
    test = embed_all(model, n, test_items, cache_dir / f"testset-{tag}.npz")

    # Training tracks.
    keys, truth, artists = [], [], []
    for r in rows:
        if r[0] in data and len(data[r[0]][0]):
            keys.append(r[0])
            truth.append([c in r[1].split(",") for c in CLASSES])
            artists.append(r[2])
    truth, artists = np.array(truth), np.array(artists)
    print(f"{len(keys)} training tracks, {sum(len(data[k][0]) for k in keys)} windows, "
          f"embedding {data[keys[0]][0].shape[1]}")

    def stack(idx):
        feats = np.concatenate([data[keys[i]][0] for i in idx])
        track_of = np.concatenate([[j] * len(data[keys[i]][0]) for j, i in enumerate(idx)])
        dist = truth[idx] / truth[idx].sum(1, keepdims=True)
        return feats, dist[track_of], track_of

    unique = np.unique(artists)
    np.random.default_rng(1).shuffle(unique)
    fold_of = {a: i % 5 for i, a in enumerate(unique)}
    folds = np.array([fold_of[a] for a in artists])
    picks = np.zeros(len(keys), int)
    for k in range(5):
        tr, te = np.where(folds != k)[0], np.where(folds == k)[0]
        head, mean, std = train(*stack(tr), args.hidden, args.epochs, args.decay)
        for i in te:
            picks[i] = int(np.argmax(track_probs(head, mean, std, data[keys[i]][0])))
    report("FMA, held out by artist (head)", picks, truth)

    head, mean, std = train(*stack(np.arange(len(keys))), args.hidden, args.epochs, args.decay)
    fold = read_fold()
    labels = ex.read_labels(f"{args.repo}/metadata/class_labels_indices.csv")
    tkeys = [k for k, _ in test_items if len(test[k][0])]
    ttruth = np.array([[TESTSET_FOLDERS[k.split("/")[0]] == c for c in CLASSES] for k in tkeys])
    head_picks = np.array([int(np.argmax(track_probs(head, mean, std, test[k][0]))) for k in tkeys])
    fold_picks = np.array([fold_pick(test[k][1], labels, fold) for k in tkeys])
    report("\ntest set, AudioSet fold (whole-clip mean; can't say 'other')", fold_picks, ttruth)
    right = report("\ntest set, head trained on all FMA tracks", head_picks, ttruth)
    for k, ok, p in zip(tkeys, right, head_picks):
        if not ok:
            print(f"    x {k}: {CLASSES[p]}")
    if args.save:
        torch.save({"head": head.state_dict(), "mean": mean, "std": std, "hidden": args.hidden,
                    "classes": CLASSES, "model": tag}, args.save)
        print(f"\nhead saved to {args.save}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
