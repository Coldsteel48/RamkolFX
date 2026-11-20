# RamkolFX
# Copyright (C) 2026 Roman Levin (Coldsteel48)
#
# This file is part of RamkolFX, dual-licensed under the GNU General
# Public License v3.0 (see LICENSE) or a separate commercial license
# (see LICENSE-COMMERCIAL.md). Contributions are accepted only under the
# terms of the Contributor License Agreement (see CLA.md).

"""Export an EfficientAT AudioSet model (mn10_as by default) to the
RamkolFX classifier contract.

One float input "waveform" [1, N] at 32 kHz mono -> one output "scores"
[1, 527] of sigmoid probabilities, with the mel front end in the graph and
ramkolfx.* metadata (see daemon/src/detect/onnx_classifier.hpp).

Rebuilds data/models/audio_classifier.onnx (see data/models/README.md):

    git clone https://github.com/fschmid56/EfficientAT
    curl -LO https://github.com/fschmid56/EfficientAT/releases/download/v0.0.1/mn10_as_mAP_471.pt
    pip install torch torchaudio torchvision onnx onnxruntime numpy
    python -I tools/model/export_efficientat.py --repo EfficientAT \
        --weights mn10_as_mAP_471.pt --seconds 3.0 --out data/models/audio_classifier.onnx

Other sizes: --arch mn --width 2.0 (mn20_as), 4.0 (mn40_as, mn40_as_ext);
--arch dymn --width 1.0 / 2.0 (dymn10_as, dymn20_as). Weights are listed
in EfficientAT's models/*/model.py (pretrained_models).
"""

import argparse
import csv
import math
import sys
import types

import numpy as np
import torch
import torch.nn as nn

SAMPLE_RATE = 32000
N_FFT = 1024
WIN = 800
HOP = 320
N_MELS = 128
FMIN = 0.0
FMAX = SAMPLE_RATE // 2 - 2000 // 2  # AugmentMelSTFT's default (15 kHz)


def import_efficientat(repo, arch="mn"):
    # helpers.utils reads a CSV relative to the cwd on import; the model
    # only needs NAME_TO_WIDTH from it (we always pass width_mult).
    helpers = types.ModuleType("helpers")
    utils = types.ModuleType("helpers.utils")
    utils.NAME_TO_WIDTH = lambda name: 1.0
    helpers.utils = utils
    sys.modules["helpers"] = helpers
    sys.modules["helpers.utils"] = utils
    sys.path.insert(0, repo)
    if arch == "dymn":
        from models.dymn.model import get_model  # noqa: E402
    else:
        from models.mn.model import get_model  # noqa: E402
    from models.preprocess import AugmentMelSTFT  # noqa: E402
    return get_model, AugmentMelSTFT


def read_labels(path):
    with open(path, newline="") as f:
        rows = list(csv.reader(f))
    return [row[2] for row in rows[1:]]


def kaldi_mel_banks():
    import torchaudio
    banks, _ = torchaudio.compliance.kaldi.get_mel_banks(
        N_MELS, N_FFT, SAMPLE_RATE, FMIN, FMAX, vtln_low=100.0, vtln_high=-500.0, vtln_warp_factor=1.0)
    return torch.nn.functional.pad(banks, (0, 1))  # [128, 513]


class MelFrontEnd(nn.Module):
    """AugmentMelSTFT in eval mode, as plain ops: pre-emphasis, centred
    reflect-padded STFT (Hann 800 inside a 1024 FFT, hop 320), power,
    Kaldi mel banks, log, (x + 4.5) / 5. The DFT is a strided conv with a
    windowed cos/sin basis, which every execution provider runs."""

    def __init__(self):
        super().__init__()
        window = torch.hann_window(WIN, periodic=False, dtype=torch.float64)
        left = (N_FFT - WIN) // 2
        padded = torch.zeros(N_FFT, dtype=torch.float64)
        padded[left:left + WIN] = window
        bins = N_FFT // 2 + 1
        n = torch.arange(N_FFT, dtype=torch.float64)
        k = torch.arange(bins, dtype=torch.float64)
        angle = 2.0 * math.pi * k[:, None] * n[None, :] / N_FFT
        basis = torch.cat([torch.cos(angle) * padded, -torch.sin(angle) * padded], dim=0)
        self.register_buffer("basis", basis.float().unsqueeze(1))  # [1026, 1, 1024]
        self.register_buffer("mel", kaldi_mel_banks().float())  # [128, 513]
        self.register_buffer("pre", torch.tensor([[[-0.97, 1.0]]]))
        self.bins = bins

    def forward(self, wave):  # [1, N]
        x = nn.functional.conv1d(wave.unsqueeze(1), self.pre)  # [1, 1, N-1]
        x = nn.functional.pad(x, (N_FFT // 2, N_FFT // 2), mode="reflect")
        spec = nn.functional.conv1d(x, self.basis, stride=HOP)  # [1, 1026, T]
        re, im = spec[:, :self.bins], spec[:, self.bins:]
        power = re * re + im * im  # [1, 513, T]
        mel = torch.matmul(self.mel, power)  # [1, 128, T]
        return ((mel + 0.00001).log() + 4.5) / 5.0


class Classifier(nn.Module):
    """mn10 was trained on 10 s clips; below ~400 mel frames (4 s) its
    zero-padded borders dominate the last feature map and the head
    saturates. A shorter window is tiled along time to context_frames
    first, which costs the same as that much audio but answers sooner."""

    def __init__(self, net, frames, context_frames):
        super().__init__()
        self.front = MelFrontEnd()
        self.net = net
        self.repeats = max(1, -(-context_frames // frames))
        self.context = max(frames, context_frames)

    def forward(self, waveform):
        spec = self.front(waveform)  # [1, 128, T]
        if self.repeats > 1:
            spec = torch.cat([spec] * self.repeats, dim=-1)[..., :self.context]
        logits, _ = self.net(spec.unsqueeze(1))
        return torch.sigmoid(logits)


def test_signals(n, rng):
    t = np.arange(n) / SAMPLE_RATE
    chord = sum(0.15 * np.sin(2 * np.pi * f * t) for f in (220.0, 277.18, 329.63, 440.0))
    noise = rng.standard_normal(n) * 0.1
    return [chord.astype(np.float32), noise.astype(np.float32),
            (chord * (0.5 + 0.5 * np.sin(2 * np.pi * 2 * t)) + 0.02 * noise).astype(np.float32)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--arch", choices=["mn", "dymn"], default="mn")
    ap.add_argument("--width", type=float, default=1.0, help="width multiplier: 1.0 = mn10/dymn10, 4.0 = mn40")
    ap.add_argument("--name", default="mn10_as (mAP 0.471)", help="model name for the metadata")
    ap.add_argument("--weights", required=True)
    ap.add_argument("--seconds", type=float, default=1.0)
    ap.add_argument("--context-frames", type=int, default=500)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    get_model, AugmentMelSTFT = import_efficientat(args.repo, args.arch)
    labels = read_labels(f"{args.repo}/metadata/class_labels_indices.csv")
    assert len(labels) == 527, len(labels)

    if args.arch == "dymn":
        net = get_model(width_mult=args.width, pretrained_name=None)
    else:
        net = get_model(width_mult=args.width, pretrained_name=None, head_type="mlp")
    state = torch.load(args.weights, map_location="cpu", weights_only=True)
    net.load_state_dict(state)
    net.eval()
    n = int(round(args.seconds * SAMPLE_RATE))
    frames = 1 + (n - 1) // HOP
    model = Classifier(net, frames, args.context_frames).eval()

    reference_mel = AugmentMelSTFT(n_mels=N_MELS, sr=SAMPLE_RATE, win_length=WIN, hopsize=HOP, n_fft=N_FFT).eval()

    rng = np.random.default_rng(7)
    signals = test_signals(n, rng)

    # The rewritten front end must match EfficientAT's own preprocessing.
    with torch.no_grad():
        worst_mel = worst_score = 0.0
        for s in signals:
            w = torch.from_numpy(s)[None, :]
            ref_spec = reference_mel(w)
            tiled = torch.cat([ref_spec] * model.repeats, dim=-1)[..., :model.context]
            ref = torch.sigmoid(net(tiled.unsqueeze(1))[0])
            ours_spec = model.front(w)
            ours = model(w)
            worst_mel = max(worst_mel, (ref_spec - ours_spec).abs().max().item())
            worst_score = max(worst_score, (ref - ours).abs().max().item())
    print(f"front end vs EfficientAT: max mel diff {worst_mel:.2e}, max score diff {worst_score:.2e}")
    assert worst_score < 1e-3, "front end doesn't match the reference"

    example = torch.from_numpy(signals[0])[None, :]
    torch.onnx.export(model, (example,), args.out, input_names=["waveform"], output_names=["scores"],
                      opset_version=17, dynamo=False, do_constant_folding=True)

    import onnx
    m = onnx.load(args.out)
    meta = {
        "ramkolfx.sample_rate": str(SAMPLE_RATE),
        "ramkolfx.labels": "\n".join(labels),
        "ramkolfx.input_samples": str(n),
        "ramkolfx.output": "probabilities",
        "ramkolfx.source": f"EfficientAT {args.name}, https://github.com/fschmid56/EfficientAT, MIT",
        "ramkolfx.context_frames": str(model.context),
    }
    del m.metadata_props[:]
    for key, value in meta.items():
        entry = m.metadata_props.add()
        entry.key, entry.value = key, value
    m.producer_name = "ramkolfx-export"
    m.doc_string = (f"EfficientAT {args.name} AudioSet tagger (Schmid et al., MIT license) with an in-graph "
                    "mel front end; input: 32 kHz mono waveform [1, N]; output: 527 AudioSet probabilities.")
    onnx.checker.check_model(m)
    onnx.save(m, args.out)

    import onnxruntime as ort
    opts = ort.SessionOptions()
    opts.intra_op_num_threads = 1
    opts.inter_op_num_threads = 1
    sess = ort.InferenceSession(args.out, opts, providers=["CPUExecutionProvider"])
    worst = 0.0
    with torch.no_grad():
        for s in signals:
            ours = model(torch.from_numpy(s)[None, :]).numpy()
            onnx_out = sess.run(None, {"waveform": s[None, :]})[0]
            worst = max(worst, float(np.abs(ours - onnx_out).max()))
    print(f"onnx vs torch: max score diff {worst:.2e}")
    assert worst < 1e-3

    import time
    x = signals[2][None, :]
    for _ in range(3):
        sess.run(None, {"waveform": x})
    times = []
    for _ in range(30):
        t0 = time.thread_time()
        sess.run(None, {"waveform": x})
        times.append((time.thread_time() - t0) * 1000)
    times.sort()
    print(f"{args.seconds:.2f} s window, N={n}, {frames} frames -> {model.context}: CPU ms/run p50 {times[15]:.2f} p95 {times[28]:.2f}")
    top = np.argsort(-sess.run(None, {"waveform": x})[0][0])[:3]
    print("top labels on the test chord:", [(labels[i], round(float(sess.run(None, {'waveform': x})[0][0][i]), 3)) for i in top])


if __name__ == "__main__":
    main()
