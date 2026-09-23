#!/usr/bin/env python3
"""Every control must actually change the picture.

A GLSL uniform whose name does not match the C++ is ignored without a word:
glGetUniformLocation returns -1 and glUniform on -1 is a documented no-op. So a
control can be wired to nothing while the plugin compiles, links, loads and
renders perfectly, and nothing in a build says anything. This is the only check
in the repo that stands between a typo and a shipped slider that does nothing.

    python3 tools/sweep.py [--size WxH] [--frames N] [--binary build/rstest]

Exit code 1 means something is dead.

------------------------------------------------------------------ the traps

**The card has to MOVE, and it has to CUT.** A still picture codes to itself,
so every encoder control reads as dead; a picture with no cut in it never
asks the scene-cut detector anything. The demo card drifts, carries a shape
going the other way, and swaps scene every 24 frames.

**Several controls only mean anything once the codec is broken.** Vector Hold
and Vector Scale change the prediction, and with the residual added back at
unity the prediction is corrected away every frame at moderate Q; they are
swept with Drop I on All and Residual Gain at 0, where the prediction is the
whole picture. Search Range, Half Pel and Block Size are swept the same way
for the same reason.

**A dropdown holds its element VALUE, not its display slot.** FFGL keeps the
two apart and only the value is ever stored, so `rstest --list` prints each
option's real range and this file sweeps every element. An integer control
is swept at both ends of its declared range and the middle.

**Two controls cannot be swept from a picture and are proven elsewhere.**
Refresh is a trigger: `--set` delivers it before frame 0, which is an I-frame
anyway, so no sweep position can differ; `rstest --gop` proves it mid-run.
Audio is the FFT buffer: its float value is meaningless, and `rstest --onset`
proves the spectrum reaches the detector through the same call the host makes.

**Never sweep the About block.** Those are buttons that open a web browser,
and sweeping them opens one tab per press. `rstest --list` marks them `about`.
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent

WIDTH, HEIGHT = 320, 180

# Past the cut at frame 24 and the I-frame a GOP of 30 puts at frame 30, so
# both the scene-cut detector and the GOP have been asked something.
FRAMES = 40

# The prediction is the whole picture when nothing corrects it.
MOSHED = {"Drop I": 2, "Residual Gain": 0.0}

# What else has to be true for a control to mean anything.
CONTEXT: dict[str, dict[str, float]] = {
    "Block Size": dict(MOSHED),
    "Search Range": dict(MOSHED),
    "Half Pel": dict(MOSHED),
    # Next and All only differ once two I-frames have been due.
    "Drop I": {"GOP": 12},
    "Vector Hold": dict(MOSHED),
    "Vector Scale": dict(MOSHED),
}

SKIP: dict[str, str] = {
    "Refresh": "a trigger; delivered before frame 0 it cannot differ. Proven by rstest --gop",
    "Audio": "the FFT buffer; its float value means nothing. Proven by rstest --onset",
}


def parameters(binary):
    """id, name, kind, low, high, straight from the harness's own declaration."""
    out = subprocess.run([binary, "--list"], capture_output=True, text=True)
    if out.returncode != 0:
        print("could not list parameters:", out.stdout, out.stderr)
        sys.exit(1)

    found = []
    for line in out.stdout.splitlines():
        m = re.match(
            r"\s*(\d+)\s+(.+?)\s{2,}(\S+)\s+([\d.eE+-]+)\s+\[\s*([\d.eE+-]+)"
            r"\s*\.\.\s*([\d.eE+-]+)\s*\]",
            line,
        )
        if m:
            found.append((int(m.group(1)), m.group(2).strip(), m.group(3),
                          float(m.group(5)), float(m.group(6))))
    return found


def positions(kind, low, high):
    """Where to sample this control.

    An option is swept at every element value it has. An integer at both ends
    and the middle of its real range. Anything else at 0, 0.5 and 1 -- three
    rather than two, because a control can be a no-op at both ends and not in
    between.
    """
    if kind == "option":
        return [float(v) for v in range(int(round(high)) + 1)]
    if kind == "boolean":
        return [low, high]
    if kind == "integer":
        return [low, float(round((low + high) / 2.0)), high]
    return [low, (low + high) / 2.0, high]


def render(binary, path, size, frames, overrides):
    args = [binary, "--out", str(path), "--size", size, "--frames", str(frames)]
    for name, value in overrides.items():
        args += ["--set", f"{name}={value}"]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        print("render failed:", " ".join(args), result.stdout, result.stderr)
        sys.exit(1)
    return pathlib.Path(path).read_bytes()


def pixels(png):
    """Raw RGBA out of the harness's own PNG (filter 0 rows), so nothing else
    is a dependency."""
    i = 8
    idat = b""
    width = height = 0
    while i < len(png):
        length = int.from_bytes(png[i:i + 4], "big")
        kind = png[i + 4:i + 8]
        data = png[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(data[0:4], "big")
            height = int.from_bytes(data[4:8], "big")
        elif kind == b"IDAT":
            idat += data
        i += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for row in range(height):
        out += raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)]
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", default=str(ROOT / "build" / "rstest"))
    ap.add_argument("--size", default=f"{WIDTH}x{HEIGHT}")
    ap.add_argument("--frames", type=int, default=FRAMES)
    args = ap.parse_args()

    binary = args.binary
    if not pathlib.Path(binary).exists():
        print(f"{binary} is not built")
        return 2

    dead = []
    swept = 0
    skipped = 0

    with tempfile.TemporaryDirectory(prefix="rssweep") as scratch:
        for pid, name, kind, low, high in parameters(binary):
            if kind == "about":
                continue
            if name in SKIP:
                print(f"  skip  {name}  ({SKIP[name]})")
                skipped += 1
                continue

            context = dict(CONTEXT.get(name, {}))
            digests = set()
            for i, value in enumerate(positions(kind, low, high)):
                overrides = dict(context)
                overrides[name] = value
                png = render(binary, f"{scratch}/{pid}_{i}.png", args.size,
                             args.frames, overrides)
                digests.add(bytes(pixels(png)))

            swept += 1
            alive = len(digests) > 1
            if not alive:
                dead.append(name)
            print(f"  {'ok' if alive else 'DEAD':4}  {name}")

    print()
    if dead:
        print(f"{len(dead)} control(s) changed nothing: {', '.join(dead)}")
        print("either the uniform name does not match the shader, or the sweep")
        print("needs a CONTEXT entry saying what else has to be true.")
        return 1

    print(f"all {swept} swept controls measurably change the picture"
          f" ({skipped} proven by the harness instead)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
