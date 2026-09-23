"""The demo's GLSL must be the plugin's GLSL, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two have drifted.

------------------------------------------------------------------- the point

`demo/plugin.js` carries a second copy of every shader in `source/Shaders.cpp`,
because a browser cannot include a C++ file. Two copies of a shader is exactly
the arrangement that drifts, and the drift is invisible from both sides: the
plugin keeps working, the page keeps working, and they quietly stop being the
same effect. The page's whole claim is that what it runs is the plugin's own
code, so the moment that stops being checkable the page is a lie.

This compares the text, not the behaviour. Reformatting counts as drift, and
that is deliberate -- "it is only whitespace" is how a real change gets waved
through.

The one transformation is a decode, not a normalisation: a backtick cannot
appear raw inside a JavaScript template literal, so plugin.js escapes one as
\\`. This undoes that escape and REJECTS any other backslash on the JS side --
there is none in the C++, so a second escape could only be somebody hiding a
difference.

--------------------------------------------------------------- what it cannot

Nothing here checks the *ported* arithmetic, nor the assembly recipe itself
beyond the pieces. `assemble()` and `assembleMotion()` in plugin.js mirror
`shaders::assemble` / `shaders::assembleMotion`; the conversions out of
`Controls.cpp`, `quantStep` and the DCT basis out of `Codec.cpp`, and the
search schedule, `decideIntra`, the Drop I latch and the Vector Hold count out
of `Residual.cpp` are a hand translation, and only a reader can tell whether
they still agree. When you change one in C++, change it there too.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# The C++ constant, and the JS constant it must equal. Fifteen PIECES, not
# shaders: kCommon and the bodies are assembled at run time by
# shaders::assemble() / assembleMotion(), and plugin.js carries the pieces and
# assembles them the same way, so a change to kCommon is caught once here
# rather than hidden inside thirteen assembled strings.
PAIRS = [
    ("source/Shaders.cpp", "kVertexShader", "VERTEX"),
    ("source/Shaders.cpp", "kCommon", "COMMON"),
    ("source/Shaders.cpp", "kCopyBody", "COPY_BODY"),
    ("source/Shaders.cpp", "kLumaBody", "LUMA_BODY"),
    ("source/Shaders.cpp", "kDownsampleBody", "DOWNSAMPLE_BODY"),
    ("source/Shaders.cpp", "kMotionSadBody", "MOTION_SAD_BODY"),
    ("source/Shaders.cpp", "kMotionSelectBody", "MOTION_SELECT_BODY"),
    ("source/Shaders.cpp", "kSadRowsBody", "SAD_ROWS_BODY"),
    ("source/Shaders.cpp", "kSadTotalBody", "SAD_TOTAL_BODY"),
    ("source/Shaders.cpp", "kPredictBody", "PREDICT_BODY"),
    ("source/Shaders.cpp", "kDctRowBody", "DCT_ROW_BODY"),
    ("source/Shaders.cpp", "kDctColBody", "DCT_COL_BODY"),
    ("source/Shaders.cpp", "kIdctColBody", "IDCT_COL_BODY"),
    ("source/Shaders.cpp", "kIdctRowBody", "IDCT_ROW_BODY"),
    ("source/Shaders.cpp", "kCompositeBody", "COMPOSITE_BODY"),
]


def cpp_literal(path, name):
    """The body of `const char* const name = R"( ... )";`.

    A shader may be several ADJACENT raw strings -- MSVC caps one literal at
    about 16 KB -- so everything up to the terminating semicolon is joined, the
    same recipe tools/verify.sh's extraction uses.
    """
    source = (ROOT / path).read_text()
    match = re.search(
        r'const char\* const\s+' + re.escape(name)
        + r'\s*=\s*((?:\s*(?://[^\n]*\n)*\s*R"\(.*?\)")+)\s*;',
        source,
        re.S,
    )
    if match is None:
        return None
    return "".join(re.findall(r'R"\((.*?)\)"', match.group(1), re.S))


def js_literal(source, name):
    """The body of ``const NAME = `...`;``, with the backtick escape undone.

    Returns (text, complaint)."""
    match = re.search(r'^const\s+' + re.escape(name) + r'\s*=\s*`(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None
    body = match.group(1)
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        line = body[: stray.start()].count("\n") + 1
        return None, f"backslash that is not an escaped backtick, at line {line}"
    return body.replace("\\`", "`"), None


def main():
    js = (ROOT / "demo/plugin.js").read_text()
    failures = 0

    for path, cpp_name, js_name in PAIRS:
        expected = cpp_literal(path, cpp_name)
        actual, complaint = js_literal(js, js_name)

        if expected is None:
            print(f"MISSING  {cpp_name} not found in {path}")
            failures += 1
            continue
        if complaint is not None:
            print(f"UNUSABLE {js_name} in demo/plugin.js has a {complaint}")
            failures += 1
            continue
        if actual is None:
            print(f"MISSING  {js_name} not found in demo/plugin.js")
            failures += 1
            continue
        if "${" in expected:
            print(f"UNUSABLE {cpp_name} contains ${{ -- it cannot be a JS template literal")
            failures += 1
            continue

        if expected == actual:
            print(f"ok       {js_name} matches {cpp_name} ({len(expected)} chars)")
            continue

        failures += 1
        print(f"DRIFTED  {js_name} does not match {cpp_name}")
        expected_lines = expected.splitlines()
        actual_lines = actual.splitlines()
        for i in range(max(len(expected_lines), len(actual_lines))):
            a = expected_lines[i] if i < len(expected_lines) else "<end>"
            b = actual_lines[i] if i < len(actual_lines) else "<end>"
            if a != b:
                print(f"         first difference at line {i + 1}")
                print(f"           {path}: {a!r}")
                print(f"           demo/plugin.js: {b!r}")
                break

    print()
    if failures:
        print(f"{failures} shader(s) have drifted. Copy the C++ across; do not edit the JS.")
        return 1

    print(f"all {len(PAIRS)} shaders are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
