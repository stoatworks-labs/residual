# Attributions

Residual is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### Diag and the harness shape — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Diag.{h,cpp} is tinsel's, by way of photofinish, with only its header comment changed; the harness's PNG writer, CGL context and rig, tools/sweep.py and tools/verify.sh follow photofinish's shape where the job is the same.

### Onset detector — Stoatworks macroblock

<https://github.com/stoatworks-labs/macroblock>  
Licence: MIT  
Copyright: Stoatworks Labs

source/Onset.cpp is macroblock's spectral-flux design, positive change between raw frames against an adaptive floor, counted in frames instead of seconds and primed on the first frame.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Inspirations

What this set out to be. No code, assets or binaries from any of these were used or examined — the debt is to the idea.

### Hybrid block-based video codecs

Block-matching motion estimation, closed-loop prediction from the reconstructed frame, a quantised 8x8 DCT residual and a GOP: the shape of every hybrid video codec since H.261. Nothing was ported from any codec's source; the implementation is written from the arithmetic.

## Standards and published specifications

What the implementation is measured against.

- **H. Malvar and G. Sullivan, "YCoCg-R: A Color Space with RGB Reversibility and Low Dynamic Range" (JVT-I014, 2003)** — The reversible integer colour transform, as used by H.264's High 4:4:4 profile for lossless coding, implemented from the contribution.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
