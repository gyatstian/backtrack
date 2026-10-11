# third_party

## Purpose

Vendored third-party source. Currently only RNNoise, used by the optional
microphone noise suppression feature.

## Ownership

- Owns: the vendored tree and its provenance record, `rnnoise/README.md`.
- Does not own: the feature. The wrapper that uses it is
  `audio/RnnoiseDenoiser.cpp`, which follows `audio/AGENTS.md`.

## Local Contracts

- The only permitted local change to upstream code is the single patch already
  recorded in `rnnoise/README.md` (`RNNOISE_SILENCE_E_GATE` in `src/denoise.c`,
  findable by searching for "Backtrack"). Any second patch must be added there
  with the reason it is needed, or reverted.
- Do not restyle upstream files, do not fix their warnings, and do not upgrade
  them incidentally as part of another change.
- The `.c` files are listed individually in both build files (the CMake glob
  skips `third_party`, and the vcxproj has explicit entries). Adding or removing
  a file here is a two-file edit.
- The feature is off by default: `BACKTRACK_NOISE_SUPPRESSION` in CMake,
  `BacktrackNoiseSuppression` in the vcxproj. The library compiles as plain C11 at
  `/W3` so its benign `double` to `float` warnings stay out of the app's warning
  budget. Keep it that way.
- Provenance is documented in `rnnoise/README.md`: upstream commit, model
  provenance, and the local change. Update that file whenever the vendored copy
  changes; it is the only record of what is actually in this folder.

## Work Guidance

- The quantized int8 weights in `rnnoise_data.c` are generated, not hand
  written. Regenerate them upstream rather than editing them here.
- Anything Backtrack needs beyond the vendored API belongs in
  `audio/RnnoiseDenoiser.cpp`, which is where the level conditioning and
  buffering already live.

## Verification

Build with `BACKTRACK_NOISE_SUPPRESSION=ON` and run `AudioDenoiseTests`. The
default build excludes `RnnoiseDenoiser.cpp` from the sources entirely, so
verify both configurations.

## Child DOX Index

- `rnnoise` - no doc of its own; its `README.md` is the provenance record and is
  governed by this file.