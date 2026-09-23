#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go.
#
#   tools/verify.sh
#
# It builds FRESH and UNIVERSAL, because the two failures this repo cannot see
# any other way are both invisible in a build log:
#
#   - CMake latches the architecture list when the first target is created, so
#     a late -DCMAKE_OSX_ARCHITECTURES is silently ignored and an arm64-only
#     binary is reported as a success. `lipo` is the only witness.
#   - a stale build directory configured for one architecture will happily
#     produce a correct-looking bundle for the other.
#
# The checks, and what each one answers that none of the others can:
#
#   shaders     does every shader compile, through a real GLSL compiler, before
#               a host has to find out. A shader that will not compile presents
#               to an operator as "the effect does nothing", with the real
#               message buried in the diagnostics log. The motion shaders are
#               assembled with #defines, and the extractor knows the recipe.
#   transform   YCoCg-R reversible for every triple, the DCT basis orthonormal,
#               the float round trip inside its derived bound. No GL at all.
#   vectors     a texture translated by (dx, dy) returns exactly (dx, dy) from
#               every interior block; flat blocks return (0, 0); a shift past
#               the range is not found. Two rasters, three block sizes.
#   lossless    Q 0 and gain 1 give the source back, bitwise, at two rasters.
#   mosh        the headline: no I-frame and no residual over a hard cut, and
#               the decoded frame IS the previous decoded frame block-copied by
#               the vectors, bitwise.
#   gop         I-frames land exactly every GOP-th frame, an I-frame is a
#               function of its source alone, Drop I and Refresh do what they
#               say, the scene-cut detector fires at a cut and not on motion.
#   drift       closed-loop error grows by exactly one code a frame under the
#               quantiser's dead zone, for 31 frames, at two rasters.
#   resize      a resize mid-run restarts with an I-frame and never shows a
#               cleared reference.
#   onset       the first onset after a clip trigger drops an I-frame; a primed
#               detector fires neither on frame 0 nor at the clock jump.
#   negative    seven perturbations of the model, asserted to FAIL. A check
#               that cannot fail is not a check.
#   sweep       no control is silently dead. A GLSL uniform whose name does not
#               match the C++ is ignored without a word.
#   pipe        the fleet's --pipe format end to end: whole frames in give
#               whole frames out, the right way up; a partial frame at the end
#               is dropped, not rendered; a cue naming no parameter is refused
#               before a frame is read. The project video is made through this.
#   plugMain    does the bundle contain a plugin at all -- a file-scope
#               CFFGLPluginInfo nothing names, which a linker may drop while
#               still producing a bundle that loads and exports plugMain.
#   lipo        is the macOS build really universal.
#   plist       does CFBundleExecutable name the binary that is actually on
#               disk. If it does not, codesign reports "code object is not
#               signed at all" about a NESTED object and mentions neither the
#               plist nor the cause -- and that is a release-time failure with
#               no local symptom.
#   codesign    the exact command the release job runs, against a copy.
#   oxbow       instantiation and 120 frames in a real FFGL host, which nothing
#               else here reaches, plus the name, the id and the type as a host
#               sees them.
#   bench       the render cost. Not pass/fail -- there is no threshold worth
#               asserting on somebody else's GPU -- but a verify run leaves a
#               timing on the record, which is what turns "it feels slower"
#               into a comparison.
#
# The last four are release-job work done locally on purpose. A check that only
# runs in CI, after a tag, is a check that will catch you after the tag.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build-verify}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/Shaders.cpp",
]

# Every fragment shader is assembled at run time: #version, kCommon, body.
# Mirrors assemble() and assembleMotion() in Shaders.cpp. The two motion
# bodies take BLOCK and PAD as #defines, one program per (block, pad) pair;
# the pairs here are kMotionVariants in Residual.cpp, and the extractor
# compiles every one of them.
BODIES = [
	"kCopyBody", "kLumaBody", "kDownsampleBody", "kSadRowsBody", "kSadTotalBody",
	"kPredictBody", "kDctRowBody", "kDctColBody", "kIdctColBody", "kIdctRowBody",
	"kCompositeBody",
]
MOTION = [ "kMotionSadBody", "kMotionSelectBody" ]
VARIANTS = [ ( 8, 0 ), ( 16, 0 ), ( 32, 0 ), ( 4, 2 ), ( 8, 4 ), ( 16, 8 ) ]

# A shader may be several adjacent raw strings (MSVC caps one literal at about
# 16 KB), so everything up to the terminating semicolon is joined.
named = {}
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(\w+)\s*=\s*((?:\s*(?://[^\n]*\n)*\s*R"\(.*?\)")+)\s*;', text, re.S ):
		named[ m.group( 1 ) ] = "".join( re.findall( r'R"\((.*?)\)"', m.group( 2 ), re.S ) )

def emit( name, body ):
	# The vertex shader is the one that writes gl_Position; everything else is
	# a fragment shader. glslc takes the stage from the extension.
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

# A name that has moved is a KeyError here, not a silent skip.
emit( "kVertexShader", named[ "kVertexShader" ] )
for name in BODIES:
	emit( name, "#version 410 core\n" + named[ "kCommon" ] + named[ name ] )
for name in MOTION:
	for block, pad in VARIANTS:
		emit( f"{name}_{block}_{pad}",
		      f"#version 410 core\n#define BLOCK {block}\n#define PAD {pad}\n" + named[ "kCommon" ] + named[ name ] )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass. It means the extraction
		# above has lost track of where this repo keeps its GLSL, and a check
		# that silently looks at nothing is worse than no check.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	# Twenty-four: the vertex shader, eleven fragment bodies, and the two
	# motion bodies in six variants each.
	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

step "shaders"
if shaders_compile; then
	pass "every shader compiles"
else
	fail "a shader does not compile"
fi

step "build (fresh, universal, Release)"
rm -rf "$BUILD"
if ! cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1; then
	fail "configure failed -- run: cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release"
	exit 1
fi
if cmake --build "$BUILD" --parallel >/dev/null 2>&1; then
	pass "builds"
else
	fail "build failed -- run: cmake --build $BUILD"
	exit 1
fi

RSTEST="$BUILD/rstest"

step "checks"
# One process, so the GL context is stood up once. Each check prints its own
# numbers; the summary here is the verdict.
if "$RSTEST" --transform --vectors --lossless --mosh --gop --drift --resize --onset --negative \
             > /tmp/residual-checks.txt 2>&1; then
	grep -c '^   ok' /tmp/residual-checks.txt \
		| xargs -I{} printf '   {} assertions passed (full output: /tmp/residual-checks.txt)\n'
	pass "every check"
else
	printf '\n'
	grep -E '^   (FAIL|ok)' /tmp/residual-checks.txt | grep FAIL | sed 's/^/   /'
	fail "a check failed -- see /tmp/residual-checks.txt"
fi

# The headline number from every check, on the record, whether or not anything
# failed. A run that only says "all checks passed" says nothing about how much
# margin there was, and margin is the only thing that says whether a tolerance
# is honest.
sed -n '/^== summary/,/^$/p' /tmp/residual-checks.txt | sed 's/^/   /'

step "sweep"
if python3 tools/sweep.py --binary "$RSTEST" > /tmp/residual-sweep.txt 2>&1; then
	tail -1 /tmp/residual-sweep.txt | sed 's/^/   /'
	pass "no control is silently dead"
else
	tail -4 /tmp/residual-sweep.txt | sed 's/^/   /'
	fail "tools/sweep.py reports a dead control"
fi

step "pipe"
# Q 0 through the pipe: the codec is lossless (--lossless proves that
# bitwise on the decoded texture), so what comes out must be what went in --
# which is what finds a pipe that flips one way and not the other, or drops
# a frame. The composite writes the host's RGBA8 through the GL's
# float-to-fixed conversion, where the specification only PREFERS round-to-
# nearest, so the tolerance is one code value and not zero.
if python3 - "$RSTEST" <<'PIPE_PY' > /tmp/residual-pipe.txt 2>&1
import os, subprocess, sys, tempfile
rstest = sys.argv[ 1 ]
W, H, N = 64, 36, 5
def frame( f ):
	# Asymmetric in x and in y, and moving, so a flip or a lost frame shows.
	return bytes( v for y in range( H ) for x in range( W )
	              for v in ( ( x * 4 + f * 3 ) % 256, ( y * 7 ) % 256, ( x * y + f ) % 256, 255 ) )
frames = [ frame( f ) for f in range( N ) ]
failures = 0
def report( ok, what ):
	global failures
	print( ( "ok    " if ok else "FAIL  " ) + what )
	failures += 0 if ok else 1
with tempfile.TemporaryDirectory() as d:
	cues = os.path.join( d, "cues.txt" )
	with open( cues, "w" ) as f:
		f.write( "# a comment\n0 Q 0\n0 Chroma Q 0\n0 Residual Gain 0.5\n2 Refresh 1\n" )
	run = subprocess.run( [ rstest, "--pipe", "--size", f"{W}x{H}", "--fps", "30", "--script", cues ],
	                      input=b"".join( frames ) + b"\x80" * 100, capture_output=True )
	out = run.stdout
	report( run.returncode == 0, f"exits 0 (got {run.returncode})" )
	report( len( out ) == N * W * H * 4, f"{N} whole frames and 100 stray bytes in, {len( out ) // ( W * H * 4 )} frames out" )
	worst = 0
	for f in range( min( N, len( out ) // ( W * H * 4 ) ) ):
		got = out[ f * W * H * 4 : ( f + 1 ) * W * H * 4 ]
		worst = max( [ worst ] + [ abs( a - b ) for i, ( a, b ) in enumerate( zip( got, frames[ f ] ) ) if i % 4 != 3 ] )
	report( worst <= 1, f"Q 0 and Chroma Q 0 through the pipe give the source back, the right way up: worst {worst} code(s)" )
	log = run.stderr.decode().strip().splitlines()[ -1 ]
	report( "refresh 2;" in log, "a Refresh cue presses the button on its frame: " + log.split( "; ", 1 )[ -1 ] )
	with open( cues, "w" ) as f:
		f.write( "0 Drop Eye 2\n" )
	bad = subprocess.run( [ rstest, "--pipe", "--size", f"{W}x{H}", "--script", cues ],
	                      input=frames[ 0 ], capture_output=True )
	report( bad.returncode == 2 and not bad.stdout, "a cue naming no parameter is refused before a frame is written" )
sys.exit( 1 if failures else 0 )
PIPE_PY
then
	sed 's/^/   /' /tmp/residual-pipe.txt
	pass "the --pipe format round-trips"
else
	sed 's/^/   /' /tmp/residual-pipe.txt
	fail "--pipe -- see /tmp/residual-pipe.txt"
fi

BUNDLE="$BUILD/Residual.bundle"
BIN="$BUNDLE/Contents/MacOS/Residual"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure.
	# Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	printf '   architectures: %s\n' "$archs"
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	version=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	[ "$ident" = "com.stoatworks.ffgl.residual" ] \
		&& pass "CFBundleIdentifier is $ident" \
		|| fail "CFBundleIdentifier is '$ident', expected com.stoatworks.ffgl.residual"
	[ "$version" = "0.1.0" ] \
		&& pass "CFBundleVersion is $version" \
		|| fail "CFBundleVersion is '$version', expected 0.1.0"

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Residual.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
		codesign --force --sign - --timestamp=none "$tmp/Residual.bundle" 2>&1 | sed 's/^/       /'
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		out=$("$OXBOW" selftest "$BUNDLE" 2>&1)

		# The identity a host actually sees. The FFGL name field is not
		# null-terminated, so a name over 16 characters is truncated silently
		# and the only place that shows is here.
		grep -q '^name: *SW Residual$' <<<"$out" \
			&& pass "name is 'SW Residual' (11 of the 16 characters a host reads)" \
			|| fail "name is not 'SW Residual': $(grep '^name:' <<<"$out")"
		grep -q '^id: *RS01$' <<<"$out" \
			&& pass "id is RS01" \
			|| fail "id is not RS01: $(grep '^id:' <<<"$out")"
		grep -q '^type: *effect$' <<<"$out" \
			&& pass "type is effect" \
			|| fail "type is not effect: $(grep '^type:' <<<"$out")"

		case "$out" in
			*"FF_INSTANTIATE_GL failed"*) fail "instantiation failed -- see: $OXBOW selftest $BUNDLE" ;;
			*"selftest:    PASS"*) pass "registers, instantiates and renders 120 frames" ;;
			*) fail "oxbow did not report PASS -- see: $OXBOW selftest $BUNDLE" ;;
		esac

		grep -E '^(gl|frames|lit pixels|gl error):' <<<"$out" | sed 's/^/   /'
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

step "bench (for the record, not pass/fail)"
"$RSTEST" --bench --frames 60 2>&1 | sed -n '2,7p' | sed 's/^/   /'

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
