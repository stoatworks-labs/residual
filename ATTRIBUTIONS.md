# Attributions

Residual is built on other people's work. This file lists what that work is, who
did it, and what it is doing here.

> **Provisional.** Across the fleet this file is generated from master lists in
> `stoatworks-backend` by `scripts/sync-attributions.py`. Residual is not
> registered there yet, so this copy is hand-written. Register it before release
> — and note that the script's `--only` flag truncates the file rather than
> filtering it.

## Third-party code this project uses

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>
Licence: BSD-3-Clause
Copyright: FreeFrame

Vendored as a git submodule at `external/ffgl`, pinned to `b1afaf9`.

The plugin ABI itself. An FFGL effect is defined by this SDK's headers — there is
no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Windows only, from vcpkg, statically linked. The SDK's headers pull it in for the
OpenGL function pointers; macOS uses the system OpenGL framework instead.

### zlib

<https://zlib.net>
Licence: zlib
Copyright: Jean-loup Gailly and Mark Adler

Linked from the system, by the offline harness only, so that `rstest --out` can
write a PNG in fifty lines instead of vendoring an image library. It is not in
the plugin.

## Work from elsewhere in the fleet

### photofinish and tinsel

<https://github.com/stoatworks-labs/tinsel>
Licence: MIT
Copyright: Stoatworks Labs

`source/Diag.{h,cpp}` is tinsel's, by way of photofinish, with only its header
comment changed. The harness's PNG writer, CGL context and rig, `tools/sweep.py`
and `tools/verify.sh` follow photofinish's shape line for line where the job is
the same. `PassBuffer` was deliberately **not** copied — see `Buffer.h` for why
an integer render target cannot be an `ffglex::FFGLFBO`.

### macroblock

<https://github.com/stoatworks-labs/macroblock>
Licence: MIT
Copyright: Stoatworks Labs

The onset detector in `source/Onset.cpp` is macroblock's spectral-flux design —
positive change between raw frames against an adaptive floor — counted in frames
instead of seconds and primed on the first frame.

### The About block

`source/StoatworksAbout*.h` come from `stoatworks-backend/about`. They are
vendored into every plugin in the fleet by `scripts/sync-about.py`; this copy is
**hand-written and provisional**, because Residual is not registered in the
backend yet.

## Prior art, not code

The colour transform is **YCoCg-R**, the reversible integer form from Malvar and
Sullivan's 2003 JVT contribution, which H.264's High 4:4:4 profile uses for
lossless coding. The rest is the shape of every hybrid video codec since H.261:
block-matching motion estimation, closed-loop prediction from the reconstructed
frame, a quantised 8×8 DCT residual, and a GOP. Nothing was ported from any
codec's source; the implementation here is written from the arithmetic.
