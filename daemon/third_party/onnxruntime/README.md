# ONNX Runtime C API header

`onnxruntime_c_api.h` from ONNX Runtime 1.21.0 (MIT, see `LICENSE`),
unmodified. It's the only part of ONNX Runtime the build uses: the daemon
dlopens `libonnxruntime.so` at run time and requests C API version 17, so
any installed runtime from 1.17 on works and builds need no ORT at all
(see `daemon/src/detect/onnx_classifier.cpp` and
`docs/content-detect-plan.md`).

1.21 is the oldest runtime on the target distros (Debian 13), and its
header is self-contained; newer headers include further files. To update,
replace the file with a newer release's header. Calls added after API 17
need `RequestedOrtApiVersion` raised, which drops support for runtimes
older than that.
