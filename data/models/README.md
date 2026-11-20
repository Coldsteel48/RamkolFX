# Bundled Auto-detect model

`audio_classifier.onnx` is the audio classifier behind Auto-detect, the
content-aware choice of EQ preset (see
[docs/content-detect-plan.md](../../docs/content-detect-plan.md)). The
daemon loads it through ONNX Runtime, which is not bundled: install
`onnxruntime` from your distribution (1.17 or newer). Without it,
Auto-detect shows as unavailable and everything else works as before.

## Source and license

- **Model:** EfficientAT `mn10_as`, a MobileNetV3 trained on AudioSet
  (mAP 0.471, 527 labels), by Florian Schmid et al.:
  <https://github.com/fschmid56/EfficientAT>.
- **Weights:** `mn10_as_mAP_471.pt` from that repository's v0.0.1 release.
- **License:** MIT; full text in [LICENSE-EfficientAT](LICENSE-EfficientAT).
  The MIT license is compatible with both of RamkolFX's licenses.
- **Paper:** F. Schmid, K. Koutini, G. Widmer, "Efficient Large-Scale
  Audio Tagging via Transformer-to-CNN Knowledge Distillation", ICASSP 2023.

## What was exported

[tools/model/export_efficientat.py](../../tools/model/export_efficientat.py)
builds the file from those weights; it says how to run it. The result
follows the contract in `daemon/src/detect/onnx_classifier.hpp`:

- **Format:** fp32 ONNX, opset 17, ~24 MB.
- **Input:** `waveform` `[1, 96000]`: 3 s of 32 kHz mono audio.
- **Output:** `scores` `[1, 527]`: AudioSet probabilities.
- **In-graph preprocessing:** EfficientAT's mel front end, as plain ops.
  The 3 s window's 300 mel frames are tiled to 500, because mn10 was
  trained on 10 s clips and saturates below ~400 frames.
- **Metadata:** `ramkolfx.sample_rate`, `ramkolfx.labels`,
  `ramkolfx.input_samples` and `ramkolfx.output`, plus the source.

SHA-256 of the file as shipped:
`df9a31966fcbf0bb2d46c559e76093e84c4bb73d10e59eee8adb98f39b52e5d5`

## Known limits

The model recognizes speech, music and film sound well, but its genre
scores on real music are small (mostly 0.02-0.1, even with long windows).
Auto-detect still picks the best-scoring genre for music, but genre
choices are a weak guess. A dedicated music-tagging model is planned.
