# residual — orientation for another LLM (or a newcomer)

**What it is:** an FFGL 2.1 effect for Resolume Arena/Avenue that is a real video
codec — block-matching motion estimation, closed-loop prediction, a quantised DCT
residual, a GOP — with controls that break one stage of it at a time, which is
what datamosh is. C++17 + GLSL 4.10, CMake, universal macOS `.bundle` and a
Windows `.dll`. MIT. Intended home `github.com/stoatworks-labs/residual`.

`CLAUDE.md` is the command reference — build, render, verify. This file is the
*why*: read it before touching the motion search, the colour transform, the GOP
logic, or anything that reads or writes an integer texture.

---

## The one idea

**Datamosh is not a glitch. It is a decoder doing exactly its job with the wrong
information.** A P-frame is a vector field and a residual applied to the last
frame the decoder reconstructed. Every datamosh look is one term of that loop
removed:

    decoded = blockcopy( previous DECODED, vectors ) + gain * Q( source - prediction )

- drop the I-frame at a cut: the loop keeps predicting the new scene from the
  old picture;
- gain 0: nothing ever corrects the prediction;
- hold the vectors: the pixels keep going the way they were going — the bloom;
- scale the vectors (0 to 4x): everything moves too far, or not at all;
- Q high: blocks, and a loop that slowly loses the picture between I-frames.

Nothing is drawn. The controls that produce the looks are the codec's own
controls with one of them set to a value no encoder would choose.

### What makes it a codec and not an imitation

- **The prediction is from the decoded frame, not the source.** That is what
  "closed-loop" means, it is what makes drift accumulate the way it does in a
  real decoder, and `--drift`'s negative control (predict from the source) is
  the proof that the distinction is load-bearing.
- **The residual is a quantised 8×8 DCT in Y/Co/Cg**, not a blend.
- **The vectors come from a real search on luma**, hierarchical, with a
  deterministic tie-break, the same as an encoder's — and the harness proves
  they are exact where they can be.
- **A GOP, a scene-cut detector, and a decision every frame.**

### What is deliberately not here

4:2:0 chroma subsampling (Co and Cg are coded at full resolution with their own
quantiser — the chroma-block look of a real mosh is a v0.2), B-frames, a
deblocking filter, rate control, presets, OFX, a browser demo.

---

## The shape of the code

    source/Codec.*       the arithmetic, no GL: block grid, pyramid, quantiser step,
                         YCoCg-R, the DCT basis, a CPU intra reference and a CPU
                         block copy for the harness
    source/Controls.*    0..1 host parameters to physical units, and the enums
    source/Onset.*       the onset detector, counted in frames, primed
    source/Buffer.*      an integer or float render target. NOT PassBuffer -- see below
    source/Shaders.cpp   the passes, as GLSL. Two things mirror Codec.h and are marked
    source/Residual.*    the plugin: parameters, buffers, the passes, the decision
    source/Diag.*        a log file, for the shader that will not compile (tinsel's)
    tools/rstest/        the offline harness
    tools/sweep.py       no control is silently dead
    tools/verify.sh      all of it

Per frame, in order: copy (host texture → RGBA8UI), luma, three downsamples,
the motion search (two passes per pyramid level, several at the coarsest), the
SAD reduction and its one-uint readback, the I/P decision on the CPU, predict,
dctRow, dctCol (quantise), idctCol, idctRow (reconstruct), composite.

**Every frame of state is an integer texture and every read is a texelFetch.**
`GL_RGBA8UI` for the two source copies, the two decoded copies and the
prediction; `GL_R8UI` for the luma pyramid; `GL_RGBA32I` for vectors; `GL_R32I`
for candidate SADs; `GL_R32UI` for the reduction. Only the two DCT intermediates
are float, and their error has a derived bound (`codec::dctRoundTripBound`,
0.25 codes against the 0.5 the reconstruction's rounding absorbs). That is why
the harness can say **bitwise** and mean it, on any rasteriser: the GL
specification's float-to-fixed conversion returns "one of the two" nearest
integers with round-to-nearest merely "preferred", and a normalised 8-bit target
would have put that between every shader and every byte asserted on.

---

## Decisions taken without asking

The brief said decide and write it down.

**A motion vector points to where the block came from.** `P(p) = ref(p + v)`,
MPEG's convention. So a frame sampled at `(x + vx, y + vy)` — which moves the
picture by `-v` on screen — returns `(vx, vy)`. The first version of `--vectors`
had the sign the other way and every vector came back exactly negated, which is
the cheapest possible way to learn a convention.

**Level-0 vectors are stored in half-pel units**, `(2vx, 2vy)` plus 0 or 1, so
the predict pass has one representation whether Half Pel is on or not. Coarser
levels store whole level pixels.

**Coarse levels match on a window twice the cell.** The coarsest level has 4×4
cells (8 → 4; 16 → 8 → 4; 32 → 16 → 8 → 4). A 4×4 cell judged on its own 16
samples of a half-resolution picture picked wrong minima on odd shifts, two
cells out, where the fine level's ±2 window never looks: 581 of 31,992 blocks
wrong on the first run. Matching each coarse cell on the 2×-window around it
(`PAD` = half the cell) took that to 0 of 29,961. The block being *coded* is
still the block; only the support the coarse search judges it on is wider. The
harness's flat-block margin grew by B/2 to match.

**Zero is always a candidate**, evaluated by the select pass itself, whether or
not the window around the coarse vector contains it. Standard practice, and it
is half of what makes "flat blocks return (0, 0)" true at every level.

**The tie-break is a total order**: lower SAD, then smaller |vx|+|vy|, then
lower y, then lower x. Not "first wins" — that would depend on chunk order and
candidate order, and a deterministic vector field is what the harness asserts
on. With the tie-break off (`SetTieBreakForTest(false)`) the last equal
candidate wins and every flat block on a flat frame comes back non-zero.

**I-frames reconstruct at unity whatever Residual Gain says.** An I-frame is all
residual; scaled by gain 0 it is a black frame, and the operator's Refresh
button has to produce a picture. This was also a bug the negative controls
caught — see the audit below.

**The GOP counter is 1 on an I-frame**, so frame G after it reads G and is due.
Started at 0 every I-frame landed a frame late: `--gop` found it on its first
run (I-frames at 0, 7, 14 for GOP 6).

**A dropped I-frame still ends the GOP.** The encoder made one; the decoder never
saw it; the next comes a GOP later. So Drop I = All means no I-frame ever, and
Drop I = Next drops one and the cadence carries on from where it would have
been.

**Drop I = Next arms on selection**, not on every frame the host restates it.
Choosing it again after it has spent itself arms it again; leaving the mode
disarms. On Onset arms the same latch from the detector.

**The frame after a resize is an I-frame whatever Drop I says**, and the log
says so. Reallocation clears the buffer that was the reference and there is
nothing to predict from. `--resize` renders under Drop I = All and gain 0 —
the harshest possible settings, where the only way a pixel gets a value is from
the reference — and requires every pixel after the resize to be the picture.

**The scene-cut decision is on the CPU, from one readback.** The alternative —
a decision texture on the GPU, read back a frame late — splits the GOP state
across two sides a frame apart. The readback is a synchronisation point and
costs about 1 ms; `--bench` prints the frame with and without it, and
`SetReadbackForTest` exists for that measurement alone. The detector judges
only frames whose vectors are fresh: under Vector Hold the held SAD belongs to
another frame.

**The onset detector counts frames, not seconds.** Resolume delivers one
spectrum per frame, so a frame is the finest thing it can resolve anyway;
between 30 and 60 fps its time constants move by two, which for "did a hit just
land" is nothing. The gain is that **nothing time-like reaches a shader, a phase
or a filter**: the clock trap the fleet measured (499 million ms, where a float
resolves 0.03 s) has nothing here to spring. `SetTime` is recorded for one line
in the log and used for nothing else. The detector is primed on the first
frame and has no Reset(), so a clip retrigger leaves it as it was.

**Alpha is not coded.** The decoded frame carries the current source's alpha.

**`Buffer` is not `PassBuffer`.** `ffglex::FFGLFBO::Initialise` uploads its
colour texture with `GL_RGBA` and `GL_UNSIGNED_BYTE`/`GL_FLOAT`; for an integer
internal format that pair is `GL_INVALID_OPERATION` and the texture is never
allocated. So the integer buffers needed their own class, and it keeps
PassBuffer's discipline — reallocate only on change, clear on allocation with
`glClearBuffer*iv` (glClear's float clear colour is undefined on an integer
target), `GL_NEAREST`, no depth attachment, restore rather than zero the
bindings it touched, no leak on Destroy.

**Chroma is not subsampled**, and Chroma Q is a separate quantiser rather than an
offset. Subsampling would have to come with an upsampler that breaks Q 0's
losslessness, and the round this was built in was about the verification
pass. It is the obvious v0.2 for the look.

**`--pipe` is the fleet's format, and the cue sheet is plumbicon's with two
deliberate differences.** Raw RGBA on stdin and stdout at `--size`, stderr for
everything else, a trailing partial frame dropped rather than rendered (half a
frame of garbage would be a reference every later P-frame predicts from).
`frame Name value` per line in HOST units: 0..1 for standard sliders, the real
count for the `FF_TYPE_INTEGER` ones, the element value for options, 0/1 for
booleans. Standard and integer tracks follow plumbicon exactly -- held before
the first cue and after the last, linear between. The differences:

- **Options and booleans STEP.** Drop I arms on *selection*, so a ramp from
  Off (0) to All (2) would pass through Next (1) and arm a latch nobody asked
  for. macroblock documents the same hazard and leaves it to the author; here
  it is not reachable.
- **An event is pressed on its cue's own frame (1 then 0), never held.**
  Under plumbicon's rule a single `150 Refresh 1` would be held from frame 0
  and make every frame an I-frame.

`frame Onset 1` is the one name that is not a parameter: it hands the plugin one
frame of `--onset`'s loud spectrum on a bed of silence, through the host's own
`SetParamElementValue` call, so Drop I = On Onset can be filmed. It cannot fire
on frame 0 (the detector is primed there) or within five frames of the last
onset (its refractory period). A name that is not a parameter is refused before
a frame is read, and so are the About buttons (they open a browser) and Audio
(a spectrum, not a number). The clock is `SetTime( n * 1000 / fps )` in
milliseconds from the frame number, as Resolume sends it, never accumulated --
and used by the plugin for one log line.

**The user guide claims only what the code does.** `docs/USER-GUIDE.md` was
written against `Controls.cpp`, `decideIntra`, `Onset.cpp` and the predict and
composite shaders, not against the README. Writing it found one README claim the
code does not support -- Vector Scale moving things "backwards"; the control is
0 to 4x and cannot reverse -- and that line was corrected. Three things are
stated with care rather than guessed: the macOS download's signing is not
described (the release job signs ad hoc; whether the fleet re-signs it is not
this repo's to say); the GPU memory column is computed from the buffers
`ensureBuffers` allocates, not measured; and "put it on the layer, not the clip"
rests on a clip effect being its own instance, which is how Resolume's clip
effect chains work but has not been watched happening with this plugin. The
About block keeps `guide=""` -- its URL is the backend's to hand out -- so the
guide's About section lists only the three buttons that exist.

---

## The traps

Ordered by how much time they will cost you.

**One fragment per block is not a motion search, it is a CPU emulator.** The
first search did the whole thing in one fragment per block: 23 ms for level 0 of
a 720p frame, 3600 threads each walking 6400 dependent fetches. Same total taps
with 8-pixel blocks (four times the threads, a quarter the serial work each) ran
in 6.6 ms; with 32-pixel blocks, 42 ms. Occupancy, not arithmetic. The search
is now one fragment per (block, candidate) writing one SAD, then one fragment
per block selecting, in 9×9 candidate chunks merged through the select pass's
`Previous` input: ten times faster, every vector still exact. `BLOCK` and `PAD`
are `#define`s — one program per (block, pad) pair — because as uniforms the
SAD loops were dynamic and another factor slower still.

**A GLSL uniform named `half`.** `half` is reserved (so are `patch sample input
output filter common active layout flat`); it compiled nowhere and said only
`syntax error` with a line number in a file that does not exist. The first thing
this repo tripped on, before a single frame was rendered.

**`git checkout <file>` is not "undo the mutation".** The mutation test changes
one character of the shipped GLSL and checks a check fails. Reverting it with
`git checkout` reverts to the last COMMIT, and the two-pass search rewrite was
not committed yet: an hour's work gone, rebuilt from the transcript. Commit
first, mutate, `git checkout`, and never the other way round.

**BSD `sed` has no `0,/re/` range.** The first mutation was applied with a
GNU-ism, silently did nothing, and `--mosh` passed — honestly, because the
shader was unchanged. Confirm a mutation is in the binary (`strings` finds the
mutated text) before believing what the checks say about it.

**`--mosh` can pass on nothing.** With I-frames scaled by Residual Gain, the
whole mosh run under gain 0 was black from frame 0, and black block-copies to
black: six "bitwise" passes that proved nothing. The negative control — shift
the vectors by one pixel and require a mismatch — found 0 samples differing,
which is impossible on a real picture. The check now asserts its reference IS
a picture before comparing anything. A check that cannot fail is not a check;
the negative controls are not decoration.

**Odd shifts and small coarse cells.** See the decision above: a 4×4 cell on a
half-resolution picture is sixteen samples, and sixteen samples of a shifted
texture whose 4×4 means are not a shift of the means will pick a neighbour.
The ±2 fine window cannot recover a coarse pick two cells out.

**The GOP counter's origin.** Frame 0 is intra; if the counter is 0 there and
increments on every P-frame, frame G reads G − 1 and is not due. 1 on the
I-frame.

**The vector sign.** Above. Not a bug in the plugin, a bug in the harness's idea
of what "translated by" means, and the codec convention is the one that gives
the block copy its meaning.

**`ScopedFBOBinding` does not restore the viewport**, and every pass here sets
one. The composite draws to the host's framebuffer and viewport, both captured
at the top of `ProcessOpenGL`.

**Every `ffglex::Scoped*` binding clears to 0 on exit — it does not restore.**
Every `Ensure()` runs before anything binds a texture. `Buffer::Ensure` restores
the framebuffer and texture bindings it touched, which is one fewer thing an
edit can undo by moving a line.

**An inactive integer sampler still has to point at an integer texture.** A
sampler the shader will not read on this pass is still validated, and `0` or a
float texture on an `isampler2D` makes the driver log about an incomplete
texture on every frame. Every unused sampler is pointed at a real integer
buffer.

**`texelFetch` out of range is undefined**, not clamped. Every fetch here clamps
its coordinate first; the coefficient buffers are padded to whole 8×8 blocks so
the column passes never reach past a real row.

**GLSL `%`, `/` and `>>` on negative ints.** `/` and `%` are undefined; `>>`'s
sign extension is not promised. `fd2( x )` computes `floor( x / 2 )` as
`( ( x + 65536 ) >> 1 ) - 32768`, on both sides of the mirror. The harness's
`blockCopy` divides only even numbers.

**The FFGL SDK's traps, all still live**: `FFGLScopedFBOBinding.h` is not in
the umbrella header; `SetParamInfo` clamps a `FF_TYPE_STANDARD` default into
0..1 before `SetParamRange` can widen it (integers are exempt, and Search Range,
GOP and Vector Hold are `FF_TYPE_INTEGER` with real ranges for that reason); a
TEXT parameter without a `SetTextParameter` override makes `FF_INSTANTIATE_GL`
fail for the whole plugin; `residual_core` is an OBJECT library because the
registration is a file-scope constructor nothing names.

**Timing on a shared machine.** Seven other plugin builds were running. A pass
that measured 23 ms measured 67 ms a minute later and 23 ms again after that.
Take several frames, take the stable ones, and do not read a single number.

---

## Every numeric check, audited

The brief asked of every check: **would this still hold on a different
rasteriser, and at a different raster?** Last round four of six plugins in this
fleet shipped checks calibrated to this Mac's GPU that failed on a GPU-less CI
runner, and in all four cases the test was wrong.

The discipline here was to keep the sampler out of every claim. Every codec
read is a `texelFetch` from an integer texture and every codec write is an
integer, so there is no filtering and no float-to-fixed conversion to be
rasteriser-dependent. The one float path — the DCT — has a bound derived from
GLSL 4.10's precision requirements and the transform's orthonormality, printed
by `--transform` next to the observed error.

| Check | Assertion | Tolerance, and where it comes from | Rasteriser-independent? | Raster-independent? |
|---|---|---|---|---|
| `--transform` | YCoCg-R round-trips every 8-bit triple; `C·Cᵀ = I`; float round trip exact after rounding | **None** on the round trip (integer arithmetic); 1e-6 on orthonormality (float entries); the DCT bound is **derived** — 24 correctly-rounded ops per pass on values bounded by the L2 norm, four passes: 0.2546 codes against the 0.5 that rounding absorbs, observed 0.0002 | Yes — **no GL at all**. The one check a runner with no context can still run | No raster |
| `--vectors` | every interior textured block returns exactly `(2dx, 2dy)`; every flat block `(0, 0)`; a shift of R+1 is found by no block | **None — exact equality** on an integer. Interior means the reference region is inside the previous frame and the search neighbourhood (block ± (R + 8 + B/2)) touches neither the flat patch nor an edge; the count and the exclusion are printed per case | Yes. SAD on integer luma through `texelFetch`; the winner is an argmin under a total order | **320×180 and 640×360**, three block sizes, ranges 4 to 32, seven shifts including odd ones and ones on the range limit; 63 cases, 29,961 blocks judged, 15,259 excluded and said so |
| `--lossless` | decoded == source, RGB, every frame | **Zero.** YCoCg-R is reversible, and the DCT's error is under the bound, so the rounding lands on the residual exactly | Yes, given the derived bound — which is a statement about IEEE float and GLSL 4.10's precision rules, not about this GPU | **320×180 and 1280×720**, three block sizes, half-pel on and off, seven P-frames each; 49,766,400 samples |
| `--mosh` | decoded after a cut == `blockCopy( previous decoded, vectors )`, RGB | **Zero.** A whole-pel block copy is one integer fetch on both sides; the CPU copy is eleven lines. Asserted non-vacuous: the reference must be a picture | Yes | **320×180 and 640×360**, blocks 8, 16 and 32, a hard cut and an ordinary P-frame; 2,592,000 samples |
| `--gop` cadence | I-frames at exactly `0, G, 2G, …`; Drop I Next drops `{4}` and lands `{0, 8, 12}`; All lands `{0}`; Refresh under All lands `{0, 9}`; a cut at 8 lands `{0, 8}` | **Exact** frame indices from the plugin's own report | Yes — CPU logic, except the cut, which is a threshold on an integer SAD stated at 5.7 codes/pixel | 320×180 |
| `--gop` source-alone | two different histories, same source at the I-frame: identical decoded frames; the P-frame before differs | **Zero**, and the P-frame must differ so the claim is not vacuous | Yes | **320×180 and 640×360** |
| `--gop` intra | at Q 0 the I-frame is its source (**zero**); at Q 0.3 it matches a CPU double-precision transform | **One code value.** The GPU sums in float in one order, the CPU in double in another; they differ by less than the bound, so only a value within the bound of a `.5` can round differently. Observed worst: 1 | Yes | 320×180 |
| `--drift` | error against a ramping source is **exactly n** codes at frame n, strictly increasing, for 31 frames | **Zero**, and the 31 is derived: a flat block of value n has DC `8n`; at step 512 that quantises to zero while `8n < 256`, i.e. n ≤ 31 | Yes — the DC of a flat block is exact and the comparison is `floor( 8n / 512 + 0.5 )` | **320×180 and 640×360** |
| `--resize` | after 320×180 → 400×200 under Drop I = All and gain 0, frame 6 is intra, 7–9 are predicted, and every pixel is the picture | **Zero** | Yes | The check IS a raster change |
| `--onset` | onsets at exactly `{14}`, dropped at `{16}`, I-frames at `{0, 4, 8, 12, 20}` | **Exact** frame indices | Yes — no GL in the detector | 320×180 (the picture is irrelevant) |
| `--negative` | seven perturbations, asserted to FAIL | n/a | Yes | 320×180 |
| `--bench` | — | **Not pass/fail.** No threshold is worth asserting on somebody else's GPU | — | — |

### The negative controls

`rstest --negative` perturbs the model or the expectation by an amount a real
defect would produce and asserts the relevant check rejects it:

1. **no tie-break** (`SetTieBreakForTest(false)`) — on a wholly flat frame, 240
   of 240 blocks return a non-zero vector; `--vectors` would reject them
2. **open-loop prediction** (`SetOpenLoopForTest(true)`, predict from the
   previous *source*) — the drift error is 1 code at every frame and never
   grows; `--drift` rejects it. This is the spec's headline negative control
   and it is exactly the distinction "closed-loop" names
3. **an unprimed detector** (`Onset::SetPrimedForTest(false)`) — fires on frame
   0 as well as at the hit, and drops the I-frame at 4 as well as 16; `--onset`
   rejects it
4. **vectors one pixel out**, judged by `--mosh`'s block copy — 160,228 of
   172,800 samples differ at 320×180 once the reference is a real picture
5. **Q 0.3 judged as lossless** — 119,937 samples differ, worst 14 codes
6. **two histories at a P-frame** — 158,906 samples differ; the source-alone
   claim is not vacuous
7. **a shift of (3, −2) judged as (4, −2)** — 180 of 180 interior blocks "wrong"

All seven reject. If one stops rejecting, the check it belongs to has gone soft.

### One thing checked by breaking the plugin

A check that runs a *copy* of the code proves nothing about what ships. One
character of the shipped predict shader — `ivec2 base = p + whole;` to
`p - whole` — was changed, confirmed present in the harness binary with
`strings`, and run: **`--mosh` fails in all six cases**, 2,243,063 of 2,592,000
samples differing. `--lossless`, `--vectors` and `--gop` pass under the same
mutation, and correctly: a residual at Q 0 corrects any prediction whatever,
the vectors never touch the predict pass, and an I-frame has no prediction
under it. That is the shape of a good mutation: one check names the term it
depends on and the others do not. Reverted; nothing left behind.

### What the audit changed

- **`--vectors` had the sign backwards** (see the decision) and, once fixed,
  found the coarse search unreliable on odd shifts. Both of those were in the
  plugin's favour and the harness's disfavour, and the second one was a real
  weakness the 2× coarse window fixed.
- **`--gop` found the I-frames a frame late.** The counter now starts at 1.
- **`--mosh` was vacuous** under the first version's gain-scaled I-frames; the
  negative control found it, I-frames now reconstruct at unity, and the check
  asserts its reference is a picture.
- **`--resize` was black** for the same reason and became the second witness.
- **The scene-cut threshold** sat exactly on the default (11.31 codes/pixel
  against a measured 11.3) in one mosh case; the checks that depend on the
  detector now state their threshold rather than inherit a default.
- **A check that ran under the wrong file** — the reverted-by-`git checkout`
  episode — produced a linker error, not a wrong pass, which is the good
  outcome of that mistake.

### What this audit does NOT cover

- **Nothing here has run on a GPU-less rasteriser.** The CI workflow is written
  and will do exactly that; there is no repo for it to run in yet. The argument
  above is that none of these checks *can* depend on the rasteriser; it is an
  argument, not a measurement, and the first CI run is what turns it into one.
  `--transform` is the exception and needs no GL at all.
- **Nothing here has run on Intel.** `lipo` says both slices are there; only
  arm64 has executed.
- **The DCT bound assumes correctly-rounded add and multiply.** GLSL 4.10 §4.5.1
  requires it; a driver that does not honour it would show up as a `--lossless`
  failure, which is the right place.

---

## What is genuinely verified, and what is assumed

**Verified, by measurement, on this machine (Apple M4 Max, macOS 26.4,
`4.1 Metal - 90.5`), `tools/verify.sh` on 2026-09-23:**

- **296 assertions**, all passing, across nine check suites; the headline numbers
  are in the table above and the script prints them on every run.
- **YCoCg-R is reversible for all 16,777,216 RGB triples**, and the DCT round
  trip's observed error (0.0002 codes) sits inside its derived bound (0.2546).
- **The motion search is exact where it can be**: 0 of 29,961 interior blocks
  wrong across 63 cases at two rasters, three block sizes and seven shifts;
  every flat block (0, 0); a shift one past the range never found.
- **Q 0 is lossless, bitwise**: 0 of 49,766,400 samples.
- **The mosh is a block copy, bitwise**: 0 of 2,592,000 samples across six
  cases, and the negative control shows the comparison sees one pixel.
- **I-frames land where the GOP says and are functions of the source alone**,
  bitwise, at two rasters; Drop I, Refresh and the scene-cut detector do what
  their names say.
- **Closed-loop drift is exactly n codes at frame n for 31 frames**, and open
  loop is not.
- **A resize restarts with an I-frame and never shows a cleared reference.**
- **The primed detector fires once, at the hit**, and the I-frame after it is
  the one dropped.
- **No dead controls.** All **14** sweepable controls measurably change the
  picture (`tools/sweep.py`); Refresh and Audio are proven by `--gop` and
  `--onset` instead, and the sweep says so.
- **24 shaders compile** through glslc, including the motion pair in all six
  block/pad variants.
- **The build is universal and exports `plugMain`** — `lipo` reports
  `x86_64 arm64`, `nm -gU` finds `_plugMain`; the plist names the binary; the
  release job's `codesign` succeeds on a copy.
- **It registers, instantiates and renders 120 frames in a real FFGL host** —
  `oxbow selftest`, which also confirms `SW Residual`, `RS01`, effect.
- **The render cost** (`rstest --bench --frames 60`, after a 20-frame warm-up,
  `glFinish` on both sides, moving content so the search has work):

  | | ms/frame, defaults | without the readback | % of a 60 fps frame |
  | --- | --- | --- | --- |
  | 1280×720 | 5.1 | 3.7 | 31% |
  | 1920×1080 | 6.1 | 5.0 | 36% |
  | 3840×2160 | 14.0 | 13.2 | 84% |

  Defaults are 16-pixel blocks, ±16 search, three pyramid levels, half-pel off;
  half-pel on adds about 0.3 ms at 1080p and 1 ms at 4K. The most expensive
  setting — 8-pixel blocks at ±32, which is sixteen 9×9 chunk passes at the
  coarsest level — runs at about 50 ms a frame at *every* raster, which says
  the cost there is per pass, not per pixel, and is an open question below.
  Motion search is the whole cost: the DCT and the composite together are under
  half a millisecond at 720p and about 4 ms at 4K.

**Assumed, or not yet done:**

- **Never loaded into Resolume.** Everything here was compiled, rendered and
  measured offline against the real plugin class in a headless CGL context,
  plus one load in the fleet's own `oxbow` host. Nothing has driven Arena. How
  the parameters *present* — four groups, two dropdowns, three integer sliders,
  a button and an audio picker — is untested, and so is what Resolume does with
  an FFT buffer parameter on a plugin that also declares `SetTimeSupported`.
- **Never run on Windows, on Intel, or on a GPU-less rasteriser.**
- **Resolume's 64 spectrum bins have never been measured.** The onset detector
  sums positive flux over all of them, so it does not depend on which bin is
  which — but its floor and margin were tuned on a synthetic spectrum and will
  need a session in Arena to judge.
- **The scene-cut threshold's default (11.3 codes/pixel) is a guess** informed
  by synthetic textures that leave 11–17 after motion compensation across an
  unrelated cut. Real footage has more contrast and will probably want it
  lower; the control is there.
- **The 2× coarse window is judged on synthetic textures.** On real footage a
  hierarchical search is a heuristic and can still pick a wrong local minimum;
  that is a property of every encoder, and a datamosh does not mind.
- **No 4:2:0, no presets, no OFX, no browser demo.** See the decisions above.

---

## Open design questions

- **Why does the 8-block ±32 search cost ~50 ms at every raster?** Sixteen chunk
  passes plus sixteen selects at the coarsest level, and the time does not
  scale with pixels. Either per-pass overhead in Apple's GL is around 1.5 ms
  for these viewport sizes or something in the chunk merge is serialising. A
  fourth pyramid level for 8-pixel blocks (a 2×2 cell with a 4-pad, or a fixed
  4×4 coarse cell on a different grid) would cut the chunk count to one; it
  was not tried.
- **Is the remaining ~2.5 ms per level at 720p real work?** 90,000 threads × 256
  taps × 2 integer fetches per level. That is ~18 G fetches/s, well under what
  the GPU can do; the clamp per fetch and the `usampler2D` path are suspects,
  and an R8 unorm pyramid read with `texelFetch` (still exact: `k/255` rounds
  back to `k`) is the experiment to run.
- **Chroma at 4:2:0.** The look wants it; losslessness at Q 0 would then have
  to be stated as "luma lossless, chroma to the upsampler" or Q 0 would have to
  bypass the subsampling.
- **Should the scene-cut detector see held frames?** It cannot without a fresh
  SAD, which is a fresh search. A cheap zero-vector SAD on held frames would
  answer it at a fraction of the cost.
- **Presets.** "Classic mosh", "Bloom", "Blocks" are three rows in a table, and
  they bring the whole host-echo mechanism with them (tinsel's AGENTS.md).

---

## Siblings

`Diag` is tinsel's, by way of photofinish. The harness rig, the sweep and
`verify.sh` are photofinish's shape. The onset detector is macroblock's design.
`PassBuffer` was deliberately not copied — see `Buffer.h`. The block-grid
identity across pyramid levels (`ceil( ceil( W / 2^l ) / ( B / 2^l ) ) ==
ceil( W / B )`) is what lets one vector texture shape serve every level, and
is the closest thing here to macroblock's "the lattice must partition the
axis". Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).
