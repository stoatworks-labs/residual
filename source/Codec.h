#pragma once

#include <cstdint>
#include <vector>

/**
	The codec's arithmetic, in one place, with no GL in it.

	Everything here is a fact about the lattice or the transform rather than
	about a picture: where the blocks are, how many pyramid levels a block size
	needs, what a quantiser step is, how RGB becomes Y/Co/Cg and back, and what
	the DCT basis is. The plugin uses it to size buffers and set uniforms; the
	harness uses it to work out what the plugin MUST have produced.

	The GLSL in Shaders.cpp mirrors two things from here, and both are marked
	`//= mirrored` on both sides: the YCoCg-R transform and the half-pel
	rounding. Nothing else is written twice. The DCT basis is computed here and
	uploaded as a uniform, so the shader never evaluates a cosine.

	--------------------------------------------------------------- the colour space

	YCoCg-R, the reversible integer form (Malvar and Sullivan, 2003; H.264 High
	4:4:4 uses it for lossless RGB). Forward:

	    Co = R - B
	    t  = B + floor( Co / 2 )
	    Cg = G - t
	    Y  = t + floor( Cg / 2 )

	and the inverse undoes it in the opposite order. Every step is an integer
	add or a floor-halving, so an 8-bit RGB triple goes to Y in 0..255 and Co,
	Cg in -255..255 and comes back EXACTLY. That is what lets "Q 0 is lossless"
	be a bitwise claim rather than a tolerance: a Rec.709 matrix in float, or
	even in integers, does not round-trip 8 bits.

	`floor( x / 2 )` is written as an arithmetic shift of a value offset to be
	non-negative, because GLSL leaves `/` and `%` undefined on negative
	operands and does not promise what `>>` does to a sign bit either.

	--------------------------------------------------------------- the pyramid

	Motion is searched coarse to fine. The coarsest level is the one at which
	the block is 4x4 luma samples, so an 8-pixel block has two levels, a
	16-pixel block three, a 32-pixel block four. The BLOCK GRID is the same at
	every level -- ceil( ceil( W / 2^l ) / ( B / 2^l ) ) == ceil( W / B ) -- so
	one vector texture per level, all the same size, and a vector found at
	level l+1 names the block it belongs to at level l without any arithmetic.
*/
namespace residual::codec
{

/// The transform block. Fixed: the residual is coded as 8x8 DCTs whatever the
/// motion block size is, as in every codec from MPEG-1 to H.264.
constexpr int kTransform = 8;

/// The coarsest pyramid level has blocks this many luma samples across.
constexpr int kCoarsestBlock = 4;

/// The most pyramid levels any block size here needs (32 -> 16 -> 8 -> 4).
constexpr int kMaxLevels = 4;

/// The search window, in level pixels, around the vector handed down from the
/// coarser level. Two either side: the coarse vector is at worst one level
/// pixel out from doubling a rounded value, plus one from the coarse level
/// having matched a neighbouring minimum on a half-resolution picture.
constexpr int kRefineWindow = 2;

/// The block sizes the Block Size option offers, by option value.
constexpr int kBlockSizes[ 3 ] = { 8, 16, 32 };

/// Vectors are stored in HALF-PEL units at level 0: an integer vector (vx, vy)
/// is stored as (2vx, 2vy), and a half-pel refinement adds 0 or 1 to either.
/// Every coarser level stores whole level pixels.
constexpr int kHalfPelUnits = 2;

/// ceil( pixels / B ).
inline int blocksAcross( int pixels, int blockSize )
{
	return ( pixels + blockSize - 1 ) / blockSize;
}

/// How many pyramid levels a block size needs so the coarsest is 4x4.
inline int levelsFor( int blockSize )
{
	int levels = 1;
	while( ( blockSize >> ( levels - 1 ) ) > kCoarsestBlock )
		++levels;
	return levels;
}

/// The size of a picture axis at pyramid level l: ceil( pixels / 2^l ).
inline int levelSize( int pixels, int level )
{
	return ( pixels + ( 1 << level ) - 1 ) >> level;
}

/// The search range at level l: ceil( range / 2^l ).
inline int rangeAtLevel( int range, int level )
{
	return ( range + ( 1 << level ) - 1 ) >> level;
}

/// floor( x / 2 ) for any int, by the same offset-and-shift the GLSL uses.
inline int floorHalf( int x ) //= mirrored
{
	return ( ( x + 65536 ) >> 1 ) - 32768;
}

struct YCoCg
{
	int y, co, cg;
};

inline YCoCg toYCoCg( int r, int g, int b ) //= mirrored
{
	const int co = r - b;
	const int t  = b + floorHalf( co );
	const int cg = g - t;
	const int y  = t + floorHalf( cg );
	return YCoCg { y, co, cg };
}

inline void toRGB( const YCoCg& c, int& r, int& g, int& b ) //= mirrored
{
	const int t = c.y - floorHalf( c.cg );
	g           = c.cg + t;
	b           = t - floorHalf( c.co );
	r           = b + c.co;
}

/// The luma the motion estimator matches on: the codec's own Y.
inline int luma( int r, int g, int b )
{
	return toYCoCg( r, g, b ).y;
}

/// The quantiser step for a Q control in 0..1. ZERO at Q = 0, which means
/// bypass -- the coefficients are not even rounded -- and 2 .. 512
/// geometrically above it. Not "1 at Q = 0": rounding every coefficient to an
/// integer is a real quantiser and it is not lossless.
double quantStep( float q );

/// The orthonormal 8x8 DCT basis, `out[ u * 8 + x ] = a( u ) cos( ( 2x + 1 ) u pi / 16 )`.
/// Uploaded to the shaders as a uniform, so the GPU never evaluates a cosine.
void dctBasis( float out[ kTransform * kTransform ] );

/// The largest error the float DCT round trip can leave on one sample of an
/// 8-bit residual, in code values, derived from the arithmetic rather than
/// measured. --lossless prints it next to the 0.5 it has to stay under.
double dctRoundTripBound();

/// The intra reconstruction of an RGBA8 frame on the CPU: Y/Co/Cg, 8x8 DCT,
/// quantise at `stepLuma` / `stepChroma`, dequantise, inverse, scale the
/// residual by `gain`, round, back to RGB, clamp. Row 0 is whichever row 0 is
/// in `rgba`; the transform does not care.
///
/// This IS a second copy of the transform, and it is used for exactly one
/// thing: showing that an I-frame at Q > 0 lands where the arithmetic says,
/// to one code value. Everything bitwise in the harness is measured without
/// it.
std::vector< uint8_t > intraReference( const std::vector< uint8_t >& rgba, int width, int height,
                                       double stepLuma, double stepChroma, double gain );

/// A pure block copy: `out( p ) = reference( clamp( p + v( block of p ) ) )`,
/// with `vectors` in half-pel units, four ints per block (x, y, sad, spare),
/// `blocksX` across. Every vector must be even -- an integer copy is the claim
/// this exists to check -- and a half-pel vector is a hard error.
std::vector< uint8_t > blockCopy( const std::vector< uint8_t >& reference, int width, int height,
                                  const std::vector< int32_t >& vectors, int blocksX, int blockSize );

} // namespace residual::codec
