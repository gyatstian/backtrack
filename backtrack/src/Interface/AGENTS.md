# Interface

## Purpose

Third-party SDK headers the app compiles against. Headers only; nothing here is
linked, and no source file lives in this folder.

## Ownership

- Owns: nothing authored here. This is a vendored copy of upstream headers.
- Does not own: the wrappers. `encoder/NvencEncoder.cpp` and
  `encoder/AmfEncoder.cpp` are where these headers are used.

## Local Contracts

- Do not edit, reformat, or "fix" anything in this folder. Upstream headers carry
  their own licenses (NVIDIA Video Codec SDK, AMD AMF Apache/MIT) and must stay
  byte-identical to what was vendored so a later upgrade is a clean replacement.
- `nvEncodeAPI.h`, `nvcuvid.h`, and `cuviddec.h` are the NVIDIA Video Codec SDK.
  These are the only files that may legitimately be absent; the build then sets
  `BACKTRACK_HAS_NVENC=0` and the NVENC backend compiles to a stub. Do not add a
  stub header to make the build succeed artificially, and do not commit a copy
  that is not licensed for redistribution.
- `AMF/` is the bundled AMD AMF public headers (Apache/MIT, headers only), so the
  AMD path builds with no external download. `AMF_SDK_PATH` can point the build
  at a different copy; when it does, the parent directory is added to the include
  path because these headers use `AMF/...` prefixes.
- Include paths for these headers come only from the build files. A source file
  must never include an SDK header directly outside of `src/encoder`.

## Work Guidance

- To upgrade a vendored SDK, replace the files wholesale and note the version in
  the commit, then build both with and without the headers present.
- A bug in a vendor header is worked around in the wrapper, with a comment
  naming the header and the version. Never patch the vendored copy.

## Verification

Build with the headers present (default) and with `AMF_SDK_PATH` pointed
elsewhere or `NV_CODEC_SDK_PATH` unset, and confirm the compile flags flip to
`BACKTRACK_HAS_AMF=0` / `BACKTRACK_HAS_NVENC=0` and the app still starts.

## Child DOX Index

None. `AMF/` is upstream content and needs no doc of its own.