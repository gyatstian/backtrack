# rnnoise (vendored)

Real-time noise suppression library, https://github.com/xiph/rnnoise (BSD-3-Clause,
see COPYING). Vendored for Backtrack's optional microphone noise suppression
(`audio.inputNoiseSuppression` setting).

## Provenance

- Source: xiph/rnnoise commit `70f1d256acd4b34a572f999a05c87bf00b67730d` (main branch).
- Model: official pre-trained weights tarball referenced by `model_version`
  (`sha256 0a8755f8e2d834eff6a54714ecc7d75f9932e845df35f8b59bc52a7cfe6e8b37`,
  contains checkpoints `rnnoise10Ga_12.pth` / `rnnoise10Gb_15.pth`).
- `src/rnnoise_data.c/.h`: generated from `rnnoise10Ga_12.pth` with the upstream
  converter (`torch/rnnoise/dump_rnnoise_weights.py --quantize`). The optional
  `*_weights_float` debug copies emitted alongside the int8 weights of quantized
  layers were removed (runtime uses the int8 + scale path; conv1/dense_out/
  vad_dense stay float as upstream requires). This keeps the committed source
  ~16 MB instead of ~79 MB and the linked binary at ~3.5 MB of weights.
- Only library sources are vendored; training tools, examples, and autotools
  scaffolding are omitted.

## Local changes

`src/denoise.c` carries one local patch (`RNNOISE_SILENCE_E_GATE`, search for
"Backtrack"). Upstream's silence gate constant (`E < 0.04`) dates from 2017
when band energies were unnormalized; with today's 1/N-normalized FFT it makes
the denoiser bypass any input quieter than roughly -9 dBFS RMS, which defeats
suppression for typical quiet microphones. The patched gate (1e-9) restores the
original "digital silence only" intent: true digital silence still yields
E == 0 exactly and short-circuits the model.

## Regenerating the model data

1. Download the tarball listed in upstream `model_version`, verify its SHA-256.
2. `python torch/rnnoise/dump_rnnoise_weights.py <pth> <outdir> --quantize`
   (needs PyTorch CPU; the weight-exchange package sits beside it).
3. Remove the `static const float <layer>_weights_float[...]` definitions
   (and their two preceding `#define WEIGHTS_<name>_...` lines) for every layer
   that also has an `_int8` array. Keep `conv1_weights_float`,
   `dense_out_weights_float`, `vad_dense_weights_float`.

## Notes

- Compiles warning-clean under MSVC `/std:c11` with no source modifications
  beyond the patch above.
- Generic scalar kernels only (~1% of one core for one stream on modern
  desktop CPUs). The SSE4.1/AVX2 kernels require runtime dispatch support that
  upstream wires through autotools; they are intentionally not built here.
- API used by Backtrack: `rnnoise_create(NULL)` (embedded default model),
  `rnnoise_process_frame` (480 samples @ 48 kHz mono), `rnnoise_destroy`.
