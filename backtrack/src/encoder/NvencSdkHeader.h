#pragma once

// The build files define BACKTRACK_HAS_NVENC from header detection. A suite
// that compiles NVENC sources without those defines falls back to the vendored
// copy, so the flag means the same thing everywhere.
#ifndef BACKTRACK_HAS_NVENC
#if __has_include("Interface/nvEncodeAPI.h")
#define BACKTRACK_HAS_NVENC 1
#else
#define BACKTRACK_HAS_NVENC 0
#endif
#endif
