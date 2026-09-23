#pragma once

#include <string>

/**
	The passes, as GLSL 4.10 source.

	Every buffer between passes is an INTEGER texture, read with texelFetch,
	and every pass that writes codec state writes `out uvec4` or `out ivec4`.
	There is no sampler filtering anywhere in the codec and no float-to-fixed
	conversion between a shader and a byte, which is what lets the harness make
	bitwise claims. The only float buffers are the two DCT intermediates, and
	their arithmetic has a derived error bound (codec::dctRoundTripBound) that
	the reconstruction's rounding absorbs.

	The passes, in the order they run each frame:

	 1. copy        host texture -> RGBA8UI source. texelFetch, rounded to a
	                code, so an 8-bit host texture arrives exactly.
	 2. luma        source -> R8UI Y, the codec's own luma (YCoCg-R).
	 3. downsample  Y level l-1 -> level l, 2x2 means, up to three times.
	 4. motion      one fragment per BLOCK, per level, coarsest first. A full
	                search at the coarsest level, a +-2 window around the
	                doubled coarse vector below it, and at level 0 a half-pel
	                refinement of the winner. SAD on luma. Equal SADs prefer the
	                smaller vector, zero first.
	 5. sadRows,    the per-block SADs of the chosen vectors summed into one
	    sadTotal    number, read back by the CPU for the scene-cut decision.
	 6. predict     the reference (last DECODED frame) block-copied by the
	                vectors. A whole-pel vector is one fetch; a half-pel one is
	                the codec's rounded average of two or four.
	 7. dctRow,     the residual (source - prediction, in Y/Co/Cg) through a
	    dctCol      separable 8x8 DCT, quantised on the way out of dctCol.
	 8. idctCol,    back, scaled by Residual Gain, rounded, added to the
	    idctRow     prediction, converted to RGB, clamped: the DECODED frame.
	 9. composite   decoded (or a mix with the source) to the host, with the
	                vector overlay if asked for.

	Two things are mirrored from Codec.h and marked `//= mirrored` on both
	sides: the YCoCg-R transform, and the half-pel rounding
	`( a + b + 1 ) >> 1`, `( a + b + c + d + 2 ) >> 2`. Nothing else exists
	twice; the DCT basis is a uniform computed on the CPU.

	`kCommon` is a FRAGMENT -- no #version, no main -- prepended to every
	fragment shader by `assemble()`. tools/verify.sh's extractor knows the
	same recipe.
*/
namespace residual::shaders
{

extern const char* const kVertexShader;

/// The helpers every fragment shader shares. Not a complete shader.
extern const char* const kCommon;

extern const char* const kCopyBody;
extern const char* const kLumaBody;
extern const char* const kDownsampleBody;
extern const char* const kMotionBody;
extern const char* const kSadRowsBody;
extern const char* const kSadTotalBody;
extern const char* const kPredictBody;
extern const char* const kDctRowBody;
extern const char* const kDctColBody;
extern const char* const kIdctColBody;
extern const char* const kIdctRowBody;
extern const char* const kCompositeBody;

/// `#version 410 core` + kCommon + body.
std::string assemble( const char* body );

} // namespace residual::shaders
