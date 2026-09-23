# residual

Datamosh built as a real codec, as an FFGL **effect** for Resolume Arena/Avenue:
block-matching motion estimation, closed-loop prediction, a quantised DCT
residual, a GOP, and controls that break one stage at a time. C++/GLSL, CMake
MODULE → universal `.bundle` (macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before changing the motion search, the colour transform, the
GOP logic or anything that reads or writes an integer texture.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build --parallel`
- Install into Arena: `cmake --install build`
- Render a frame offline: `./build/rstest --out /tmp/f.png --size 1920x1080 --frames 40`
- The datamosh: `--set "Drop I=2" --set "Residual Gain=0"`; the bloom: add `--set "Vector Hold=30"`
- Set anything by name: `--set "Block Size=0" --set "Search Range=32" --set "Q=0"`
- List parameters, with their kinds and real ranges: `./build/rstest --list`
- The demo card on its own: `./build/rstest --card /tmp/card.png`
- Footage through the plugin (the fleet `--pipe` format, how the project video is made):
  `ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - | ./build/rstest --pipe --size 1920x1080 --fps 30 --script cues.txt | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 30 -i - out.mp4`
- Cue sheet: `frame Name value` per line, `#` comments, host units (0..1 floats,
  real integer counts, option index, `1` presses an event). Floats and integers
  hold-then-ramp as plumbicon; options and booleans STEP; `Refresh 1` presses on
  that frame only; `frame Onset 1` injects one loud spectrum frame (drives Drop
  I = On Onset). Unknown names, About and Audio are refused. The codec's
  I-frames, drops, cuts, presses and onsets are printed to stderr at the end.

## Verify
- Everything: `tools/verify.sh` (fresh **universal** Release build + every
  check + the sweep + plist, codesign and oxbow, a few minutes)
- Every check in one process: `./build/rstest --transform --vectors --lossless
  --mosh --gop --drift --resize --onset --negative`
- Or through ctest, one per check: `ctest --test-dir build --output-on-failure`
- The pipe: `tools/verify.sh`'s `pipe` step round-trips 5 frames at Q 0 (source
  back, right way up), drops a trailing partial frame, refuses an unknown cue
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--frames N`)
- The cost: `./build/rstest --bench --frames 60`, and at the most expensive
  search: `--set "Block Size=0" --set "Search Range=32"`

Each check, one line each:

| | what it proves |
|---|---|
| `--transform` | YCoCg-R round-trips all 16.7M triples; the DCT basis is orthonormal; the float round trip is inside its derived bound. **No GL at all** |
| `--vectors` | a texture translated by (dx, dy) returns exactly (dx, dy) from every interior block; flat blocks return (0, 0); one past the range is not found |
| `--lossless` | Q 0 and gain 1 give the source back. **Bitwise** |
| `--mosh` | no I-frame, no residual: the decoded frame IS the previous decoded frame block-copied by the vectors. **Bitwise.** The headline |
| `--gop` | I-frames land exactly every GOP-th frame; an I-frame is a function of its source alone (**bitwise**); Drop I, Refresh and the scene-cut detector do what they say |
| `--drift` | closed-loop error grows by exactly one code a frame under the quantiser's dead zone, for 31 frames |
| `--resize` | a resize mid-run restarts with an I-frame and never shows a cleared reference |
| `--onset` | the first onset after a clip trigger drops an I-frame; the primed detector fires neither on frame 0 nor at the clock jump |
| `--negative` | seven perturbations of the model, asserted to **fail** |

## Notes
- **Every frame of codec state is an INTEGER texture** (`GL_RGBA8UI`, `GL_R8UI`,
  `GL_RGBA32I`) read by `texelFetch`. No filtering, no float-to-fixed conversion
  between a shader and a byte. That is what lets the claims above be bitwise, and
  it is why `Buffer` is its own class: `ffglex::FFGLFBO` uploads with
  `GL_RGBA`/`GL_UNSIGNED_BYTE`, which is `GL_INVALID_OPERATION` for an integer
  format.
- **A motion vector points to where the block came from**: `P(p) = ref(p + v)`,
  MPEG's convention. A frame sampled at `(x + vx, y + vy)` returns `(vx, vy)`.
  Level-0 vectors are stored in **half-pel units**.
- **The search is two passes per level**: one fragment per (block, candidate)
  writes a SAD; one fragment per block selects. One fragment per block doing the
  whole search ran at 23 ms per level at 720p — 3600 threads each walking 6400
  dependent fetches. `BLOCK` and `PAD` are `#define`s, one program per pair.
- **Coarse levels match on a window twice their cell.** A 4×4 cell judged on 16
  samples picked wrong minima on odd shifts, two cells out, where the ±2 fine
  window never looks.
- **The GOP counter is 1 on the I-frame.** Started at 0, every I-frame landed a
  frame late.
- **I-frames reconstruct at unity whatever Residual Gain says.** Scaled, gain 0
  made Refresh paint black — and made `--mosh` pass vacuously, black
  block-copying to black. The negative control caught it; the check now asserts
  the reference is a picture.
- **The onset detector counts frames, not seconds.** Nothing time-like reaches a
  shader, a phase or a filter, so the clock trap the fleet measured (499 million
  ms) has nothing here to spring.
- **Reserved GLSL words**: `patch sample input output filter common active half
  layout flat`. `half` was the first thing this repo tripped on.
- **GLSL `%`, `/` and `>>` are not to be trusted on negative ints.** `fd2()`
  offsets by 65536 before shifting; the harness's `blockCopy` divides only even
  numbers.
- **The scene-cut readback is a CPU–GPU sync**, one `uint` per frame, costing
  about 1–2 ms. `--bench` prints the frame with and without it.
- **Commit before a mutation test.** `git checkout <file>` to undo one character
  reverts to the last commit, and took an hour's uncommitted rewrite with it
  here. BSD `sed` has no `0,/re/` range either: the first mutation never landed
  and `--mosh` passed honestly.
- **Parameter names must be unique** — `--set` and the sweep find them by name.
  Integer controls (`Search Range`, `GOP`, `Vector Hold`) are `FF_TYPE_INTEGER`
  with real ranges; `SetParamInfo` only clamps `FF_TYPE_STANDARD` defaults.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no
  host can instantiate the plugin at all.
- `residual_core` is an OBJECT library, not STATIC — the plugin registers itself
  from a file-scope constructor nothing references by name.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `RS01`, type `effect`, name `SW Residual`.
- Test hooks: `SetOpenLoopForTest`, `SetTieBreakForTest`,
  `SetReadbackForTest`, `Onset::SetPrimedForTest`, `ReadDecodedForTest`,
  `ReadVectorsForTest`, and the `LastXxxForTest` frame facts.

## Not done yet
- **Never loaded into Resolume on macOS.** The Windows CI build of v0.1.0 passed the
  Arena gate 8 of 9 on win-lab (Arena 7.27.1, llvmpipe) on 2026-09-23; the ninth read
  Search Range, Half Pel, Scene Cut, Scene Threshold and Drop I dead on the gate's
  still picture, and `tools/sweep.py` proves them live.
- No 4:2:0 chroma subsampling (Co/Cg are coded at full resolution with their own
  quantiser), no B-frames, no factory presets, no OpenFX port, no browser demo.
- Never run on Intel. CI runs the nine suites and the sweep on GitHub's GPU-less
  macOS runner (Apple's software rasteriser), green; the plugin also rendered on
  llvmpipe in the Arena gate.

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside
Resolume).

    ~/Library/Logs/residual/residual.YYYY-MM-DD.log       (macOS)
    %LOCALAPPDATA%\residual\logs\residual.YYYY-MM-DD.log  (Windows)

It records the build, the GL driver, which shader failed to compile if one did,
every resize (which restarts the GOP), and at frame 60 the host's clock, the
block grid and the motion-compensated SAD.
