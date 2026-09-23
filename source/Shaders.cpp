#include "Shaders.h"

namespace residual::shaders
{

const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;

	//Straight through. Every pass here addresses its inputs by texel with
	//gl_FragCoord; only the composite reads uv, to map the host's viewport
	//onto the picture.
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// Shared helpers. A fragment, not a shader.
//---------------------------------------------------------------------------
const char* const kCommon = R"(
//floor( x / 2 ) for any int. GLSL leaves `/` and `%` undefined on negative
//operands and does not promise an arithmetic `>>`, so the value is offset to
//be non-negative, shifted, and put back.
int fd2( int x ) //= mirrored
{
	return ( ( x + 65536 ) >> 1 ) - 32768;
}

//YCoCg-R: reversible in integers, so Q 0 can be lossless bit for bit. Y is
//0..255; Co and Cg are -255..255.
ivec3 toYCoCg( ivec3 rgb ) //= mirrored
{
	int co = rgb.r - rgb.b;
	int t  = rgb.b + fd2( co );
	int cg = rgb.g - t;
	int y  = t + fd2( cg );
	return ivec3( y, co, cg );
}

ivec3 toRGB( ivec3 ycc ) //= mirrored
{
	int t = ycc.x - fd2( ycc.z );
	int g = ycc.z + t;
	int b = t - fd2( ycc.y );
	int r = b + ycc.y;
	return ivec3( r, g, b );
}

ivec2 clampTo( ivec2 p, ivec2 size )
{
	return clamp( p, ivec2( 0 ), size - 1 );
}
)";

//---------------------------------------------------------------------------
// 1. copy -- the host's texture into an integer one of ours.
//---------------------------------------------------------------------------
const char* const kCopyBody = R"(
uniform sampler2D Source;

out uvec4 fragColor;

void main()
{
	//texelFetch, not texture(): the picture sits in the lower-left of a
	//texture that may be larger than it, and a fetch by integer coordinate
	//reads exactly that texel with no filter and no MaxUV to get wrong.
	ivec2 p = ivec2( gl_FragCoord.xy );
	vec4 c  = texelFetch( Source, p, 0 );

	//To the nearest code. An 8-bit host texture comes back as the same byte;
	//anything wider is quantised to eight bits here, which is what a codec
	//does to it anyway.
	fragColor = uvec4( clamp( ivec4( floor( c * 255.0 + 0.5 ) ), 0, 255 ) );
}
)";

//---------------------------------------------------------------------------
// 2. luma -- the codec's own Y, for the motion search.
//---------------------------------------------------------------------------
const char* const kLumaBody = R"(
uniform usampler2D Source;

out uint fragColor;

void main()
{
	ivec2 p   = ivec2( gl_FragCoord.xy );
	ivec3 rgb = ivec3( texelFetch( Source, p, 0 ).rgb );
	fragColor = uint( toYCoCg( rgb ).x );
}
)";

//---------------------------------------------------------------------------
// 3. downsample -- one pyramid level from the one below it.
//---------------------------------------------------------------------------
const char* const kDownsampleBody = R"(
uniform usampler2D Fine;
uniform ivec2 FineSize;

out uint fragColor;

int at( ivec2 p )
{
	return int( texelFetch( Fine, clampTo( p, FineSize ), 0 ).r );
}

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy ) * 2;
	int sum = at( p ) + at( p + ivec2( 1, 0 ) ) + at( p + ivec2( 0, 1 ) ) + at( p + ivec2( 1, 1 ) );
	fragColor = uint( ( sum + 2 ) >> 2 );
}
)";

//---------------------------------------------------------------------------
// 4. motion -- two passes per pyramid level.
//
// One fragment per BLOCK doing the whole search was measured at 23 ms for
// level 0 of a 720p frame: 3600 threads each walking 6400 dependent
// fetches, on a GPU that wants tens of thousands of short threads. So the
// search is split. `motionSad` runs one fragment per (block, candidate) and
// writes one SAD; `motionSelect` runs one fragment per block, reads its
// candidates' SADs and keeps the best under the tie-break. Candidates come
// in 9x9 chunks so the SAD buffer is 81 times the block grid whatever the
// range, and a wide coarse search is several chunk passes merged through
// the select pass's `Previous` input.
//---------------------------------------------------------------------------
const char* const kMotionSadBody = R"(
uniform usampler2D Cur;    //this level's luma, current frame
uniform usampler2D Prev;   //this level's luma, previous SOURCE frame
uniform isampler2D Coarse; //the level above's vectors, whole level pixels
uniform isampler2D Selected;//half-pel mode: this level's whole-pel winner
uniform int HasCoarse;     //0 at the coarsest level
uniform ivec2 ImageSize;   //this level's picture size
uniform int Range;         //this level's search range, whole pixels
uniform int Window;        //half-width searched around the centre
uniform int ChunkSide;     //candidates per block along each axis (9, or 3 for half-pel)
uniform ivec2 ChunkOffset; //this chunk's first offset, relative to the centre
uniform int HalfPelMode;   //1: the 3x3 half-pel neighbours of Selected

//BLOCK and PAD are #defined by assembleMotion(): one program per pair, so
//the SAD loops have constant bounds.

out int fragColor;

int curAt( ivec2 p )
{
	return int( texelFetch( Cur, clampTo( p, ImageSize ), 0 ).r );
}

int prevAt( ivec2 p )
{
	return int( texelFetch( Prev, clampTo( p, ImageSize ), 0 ).r );
}

//The previous frame at p + hv / 2, hv in half-pel units. A whole-pel offset
//is one fetch; a half-pel one is the codec's rounded mean of two or four.
int prevHalfAt( ivec2 p, ivec2 hv )
{
	ivec2 whole = ivec2( fd2( hv.x ), fd2( hv.y ) );
	ivec2 base  = p + whole;
	ivec2 f     = hv - 2 * whole;

	int a = prevAt( base );
	if( f.x == 0 && f.y == 0 )
		return a;
	int b = prevAt( base + ivec2( 1, 0 ) );
	int c = prevAt( base + ivec2( 0, 1 ) );
	int d = prevAt( base + ivec2( 1, 1 ) );
	if( f.y == 0 )
		return ( a + b + 1 ) >> 1; //= mirrored
	if( f.x == 0 )
		return ( a + c + 1 ) >> 1; //= mirrored
	return ( a + b + c + d + 2 ) >> 2; //= mirrored
}

//Sum of absolute differences over the block, whole-pel vector v. Pixels
//that lie outside the picture are not counted, so a partial block at the
//edge is judged on what it actually has.
//
//At the coarse levels the block is grown by PAD on every side, so a 4x4
//coarse cell is judged on 8x8 samples rather than 16. A cell that small on
//a half-resolution picture is too few samples to find the right minimum
//in: the fine level's window is only +-2, and a coarse pick two cells out
//is a vector the fine search never sees. The block being CODED is still the
//block; only the support the coarse search judges it on is wider.
int sadAt( ivec2 origin, ivec2 v )
{
	int sad = 0;
	for( int j = -PAD; j < BLOCK + PAD; ++j )
	{
		for( int i = -PAD; i < BLOCK + PAD; ++i )
		{
			ivec2 p = origin + ivec2( i, j );
			if( p.x < 0 || p.y < 0 || p.x >= ImageSize.x || p.y >= ImageSize.y )
				continue;
			sad += abs( curAt( p ) - prevAt( p + v ) );
		}
	}
	return sad;
}

int sadHalfAt( ivec2 origin, ivec2 hv )
{
	int sad = 0;
	for( int j = 0; j < BLOCK; ++j )
	{
		for( int i = 0; i < BLOCK; ++i )
		{
			ivec2 p = origin + ivec2( i, j );
			if( p.x >= ImageSize.x || p.y >= ImageSize.y )
				continue;
			sad += abs( curAt( p ) - prevHalfAt( p, hv ) );
		}
	}
	return sad;
}

void main()
{
	ivec2 cell   = ivec2( gl_FragCoord.xy );
	ivec2 block  = cell / ChunkSide;
	ivec2 c      = cell - block * ChunkSide;
	ivec2 origin = block * BLOCK;

	if( HalfPelMode == 1 )
	{
		//The eight half-pel neighbours of the whole-pel winner; the centre
		//cell is the winner itself and is not re-evaluated.
		ivec2 h = c - 1;
		if( h.x == 0 && h.y == 0 )
		{
			fragColor = -1;
			return;
		}
		ivec2 hv = texelFetch( Selected, block, 0 ).xy * 2 + h;
		if( abs( hv.x ) > 2 * Range || abs( hv.y ) > 2 * Range )
		{
			fragColor = -1;
			return;
		}
		fragColor = sadHalfAt( origin, hv );
		return;
	}

	//Where to search. The coarsest level searches the whole range around
	//zero; every finer level searches a small window around the doubled
	//coarse vector.
	ivec2 centre = ivec2( 0 );
	if( HasCoarse == 1 )
		centre = clamp( texelFetch( Coarse, block, 0 ).xy * 2, ivec2( -Range ), ivec2( Range ) );

	ivec2 offset = ChunkOffset + c;
	ivec2 v      = centre + offset;
	if( abs( offset.x ) > Window || abs( offset.y ) > Window || abs( v.x ) > Range || abs( v.y ) > Range )
	{
		fragColor = -1;//not a candidate
		return;
	}

	fragColor = sadAt( origin, v );
}
)";

const char* const kMotionSelectBody = R"(
uniform usampler2D Cur;
uniform usampler2D Prev;
uniform isampler2D Sads;     //one SAD per (block, candidate), -1 where not a candidate
uniform isampler2D Coarse;
uniform isampler2D Previous; //the best so far from earlier chunks, or the whole-pel winner in half-pel mode
uniform int HasCoarse;
uniform int HasPrevious;
uniform ivec2 ImageSize;
uniform int Range;
uniform int ChunkSide;
uniform ivec2 ChunkOffset;
uniform int TieBreak;      //1: equal SADs prefer the smaller vector, zero first
uniform int HalfPelMode;   //1: merge the half-pel neighbours into the whole-pel winner
uniform int OutputHalfPel; //1: write half-pel units (level 0)

out ivec4 fragColor;

int curAt( ivec2 p )
{
	return int( texelFetch( Cur, clampTo( p, ImageSize ), 0 ).r );
}

int prevAt( ivec2 p )
{
	return int( texelFetch( Prev, clampTo( p, ImageSize ), 0 ).r );
}

//The zero vector's SAD, evaluated here so that zero is always a candidate
//whether or not the window around the coarse vector happens to contain it.
int sadZero( ivec2 origin )
{
	int sad = 0;
	for( int j = -PAD; j < BLOCK + PAD; ++j )
	{
		for( int i = -PAD; i < BLOCK + PAD; ++i )
		{
			ivec2 p = origin + ivec2( i, j );
			if( p.x < 0 || p.y < 0 || p.x >= ImageSize.x || p.y >= ImageSize.y )
				continue;
			sad += abs( curAt( p ) - prevAt( p ) );
		}
	}
	return sad;
}

//The tie-break. A lower SAD always wins. At equal SAD the shorter vector
//wins, and at equal length the one lower in (y, x) order -- a total order,
//so the result never depends on the order candidates were tried in. That
//is what makes a flat block return (0, 0) rather than whichever zero-SAD
//candidate happened to be last, and what makes the vectors deterministic
//enough to assert on. With TieBreak off, the last equal candidate wins.
bool better( int sad, ivec2 v, int bestSad, ivec2 bestV )
{
	if( sad != bestSad )
		return sad < bestSad;
	if( TieBreak == 0 )
		return true;
	int n  = abs( v.x ) + abs( v.y );
	int bn = abs( bestV.x ) + abs( bestV.y );
	if( n != bn )
		return n < bn;
	if( v.y != bestV.y )
		return v.y < bestV.y;
	return v.x < bestV.x;
}

void main()
{
	ivec2 block  = ivec2( gl_FragCoord.xy );
	ivec2 origin = block * BLOCK;

	if( HalfPelMode == 1 )
	{
		//Strictly better only: a tie keeps the whole-pel vector, which is
		//the cheaper fetch and the one the tie-break already chose.
		ivec4 whole = texelFetch( Previous, block, 0 );
		ivec2 outV  = whole.xy * 2;
		int outSad  = whole.z;
		for( int cy = 0; cy < 3; ++cy )
		{
			for( int cx = 0; cx < 3; ++cx )
			{
				int sad = texelFetch( Sads, block * 3 + ivec2( cx, cy ), 0 ).r;
				if( sad < 0 )
					continue;
				ivec2 hv = whole.xy * 2 + ivec2( cx - 1, cy - 1 );
				if( sad < outSad )
				{
					outSad = sad;
					outV   = hv;
				}
			}
		}
		fragColor = ivec4( outV, outSad, 0 );
		return;
	}

	ivec2 centre = ivec2( 0 );
	if( HasCoarse == 1 )
		centre = clamp( texelFetch( Coarse, block, 0 ).xy * 2, ivec2( -Range ), ivec2( Range ) );

	ivec2 bestV;
	int bestSad;
	if( HasPrevious == 1 )
	{
		ivec4 previous = texelFetch( Previous, block, 0 );
		bestV          = previous.xy;
		bestSad        = previous.z;
	}
	else
	{
		bestV   = ivec2( 0 );
		bestSad = sadZero( origin );
	}

	for( int cy = 0; cy < ChunkSide; ++cy )
	{
		for( int cx = 0; cx < ChunkSide; ++cx )
		{
			int sad = texelFetch( Sads, block * ChunkSide + ivec2( cx, cy ), 0 ).r;
			if( sad < 0 )
				continue;
			ivec2 v = centre + ChunkOffset + ivec2( cx, cy );
			if( v.x == 0 && v.y == 0 )
				continue;//zero is already the opening candidate
			if( better( sad, v, bestSad, bestV ) )
			{
				bestSad = sad;
				bestV   = v;
			}
		}
	}

	if( OutputHalfPel == 1 )
		bestV *= 2;

	fragColor = ivec4( bestV, bestSad, 0 );
}
)";

//---------------------------------------------------------------------------
// 5. The per-block SADs summed, in two passes, for the scene-cut decision.
//---------------------------------------------------------------------------
const char* const kSadRowsBody = R"(
uniform isampler2D Vectors;
uniform int BlocksX;

out uint fragColor;

void main()
{
	int y    = int( gl_FragCoord.y );
	uint sum = 0u;
	for( int x = 0; x < BlocksX; ++x )
		sum += uint( texelFetch( Vectors, ivec2( x, y ), 0 ).z );
	fragColor = sum;
}
)";

const char* const kSadTotalBody = R"(
uniform usampler2D Rows;
uniform int BlocksY;

out uint fragColor;

void main()
{
	uint sum = 0u;
	for( int y = 0; y < BlocksY; ++y )
		sum += texelFetch( Rows, ivec2( 0, y ), 0 ).r;
	fragColor = sum;
}
)";

//---------------------------------------------------------------------------
// 6. predict -- the reference, block-copied by the vectors.
//---------------------------------------------------------------------------
const char* const kPredictBody = R"(
uniform usampler2D Reference; //the last DECODED frame
uniform isampler2D Vectors;   //level 0, half-pel units
uniform ivec2 ImageSize;
uniform int Block;
uniform float Scale;

out uvec4 fragColor;

ivec4 refAt( ivec2 p )
{
	return ivec4( texelFetch( Reference, clampTo( p, ImageSize ), 0 ) );
}

void main()
{
	ivec2 p  = ivec2( gl_FragCoord.xy );
	ivec2 hv = texelFetch( Vectors, p / Block, 0 ).xy;

	//Vector Scale, in half-pel units. At 1.0 this is exactly the identity:
	//float( hv ) * 1.0 + 0.5, floored, is hv.
	hv = ivec2( floor( vec2( hv ) * Scale + 0.5 ) );

	ivec2 whole = ivec2( fd2( hv.x ), fd2( hv.y ) );
	ivec2 base = p + whole;
	ivec2 f    = hv - 2 * whole;

	//A whole-pel vector is ONE FETCH. That is the claim --mosh makes bitwise:
	//nothing here is filtered, weighted or interpolated unless the vector
	//lands between texels.
	ivec4 a = refAt( base );
	ivec4 r = a;
	if( f.x != 0 || f.y != 0 )
	{
		ivec4 b = refAt( base + ivec2( 1, 0 ) );
		ivec4 c = refAt( base + ivec2( 0, 1 ) );
		ivec4 d = refAt( base + ivec2( 1, 1 ) );
		if( f.y == 0 )
			r = ( a + b + 1 ) >> 1; //= mirrored
		else if( f.x == 0 )
			r = ( a + c + 1 ) >> 1; //= mirrored
		else
			r = ( a + b + c + d + 2 ) >> 2; //= mirrored
	}

	fragColor = uvec4( r );
}
)";

//---------------------------------------------------------------------------
// 7. The residual, forward.
//---------------------------------------------------------------------------
const char* const kDctRowBody = R"(
uniform usampler2D Source;
uniform usampler2D Predicted;
uniform ivec2 ImageSize;
uniform int Intra;      //1: the prediction is zero, and this is an I-frame
uniform float Basis[ 64 ];

out vec4 fragColor;

//The residual in Y/Co/Cg at p, replicate-padded past the picture's edge so a
//partial 8x8 block at the edge is a whole block of something.
ivec3 residualAt( ivec2 p )
{
	p = clampTo( p, ImageSize );
	ivec3 s = toYCoCg( ivec3( texelFetch( Source, p, 0 ).rgb ) );
	if( Intra == 1 )
		return s;
	ivec3 q = toYCoCg( ivec3( texelFetch( Predicted, p, 0 ).rgb ) );
	return s - q;
}

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy );
	int u   = p.x & 7;
	int bx  = p.x >> 3;

	vec3 sum = vec3( 0.0 );
	for( int i = 0; i < 8; ++i )
		sum += Basis[ u * 8 + i ] * vec3( residualAt( ivec2( bx * 8 + i, p.y ) ) );

	fragColor = vec4( sum, 0.0 );
}
)";

const char* const kDctColBody = R"(
uniform sampler2D Rows;
uniform float Basis[ 64 ];
uniform float StepLuma;   //0 means bypass: not even rounded
uniform float StepChroma;

out vec4 fragColor;

float quantise( float c, float step )
{
	if( step <= 0.0 )
		return c;
	return floor( c / step + 0.5 ) * step;
}

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy );
	int v   = p.y & 7;
	int by  = p.y >> 3;

	vec3 sum = vec3( 0.0 );
	for( int j = 0; j < 8; ++j )
		sum += Basis[ v * 8 + j ] * texelFetch( Rows, ivec2( p.x, by * 8 + j ), 0 ).xyz;

	fragColor = vec4( quantise( sum.x, StepLuma ),
	                  quantise( sum.y, StepChroma ),
	                  quantise( sum.z, StepChroma ), 0.0 );
}
)";

//---------------------------------------------------------------------------
// 8. The residual, back, and the reconstruction.
//---------------------------------------------------------------------------
const char* const kIdctColBody = R"(
uniform sampler2D Coef;
uniform float Basis[ 64 ];

out vec4 fragColor;

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy );
	int j   = p.y & 7;
	int by  = p.y >> 3;

	vec3 sum = vec3( 0.0 );
	for( int v = 0; v < 8; ++v )
		sum += Basis[ v * 8 + j ] * texelFetch( Coef, ivec2( p.x, by * 8 + v ), 0 ).xyz;

	fragColor = vec4( sum, 0.0 );
}
)";

const char* const kIdctRowBody = R"(
uniform sampler2D Cols;
uniform usampler2D Source;
uniform usampler2D Predicted;
uniform ivec2 ImageSize;
uniform int Intra;
uniform float Gain;
uniform float Basis[ 64 ];

out uvec4 fragColor;

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy );
	int i   = p.x & 7;
	int bx  = p.x >> 3;

	vec3 r = vec3( 0.0 );
	for( int u = 0; u < 8; ++u )
		r += Basis[ u * 8 + i ] * texelFetch( Cols, ivec2( bx * 8 + u, p.y ), 0 ).xyz;

	//Residual Gain, then to the nearest code. At Q 0 and gain 1 the value here
	//is the exact residual plus the float transform's error, which
	//codec::dctRoundTripBound puts well under half a code -- so the rounding
	//lands on the residual exactly and the frame is lossless.
	ivec3 ri = ivec3( floor( r * Gain + 0.5 ) );

	ivec3 pred = ivec3( 0 );
	if( Intra == 0 )
		pred = toYCoCg( ivec3( texelFetch( Predicted, p, 0 ).rgb ) );

	ivec3 rgb = clamp( toRGB( pred + ri ), 0, 255 );

	//Alpha is not coded. The decoded frame carries the source's alpha through.
	uint a = texelFetch( Source, p, 0 ).a;

	fragColor = uvec4( rgb, a );
}
)";

//---------------------------------------------------------------------------
// 9. composite -- to the host.
//---------------------------------------------------------------------------
const char* const kCompositeBody = R"(
uniform usampler2D Source;
uniform usampler2D Decoded;
uniform isampler2D Vectors;
uniform ivec2 ImageSize;
uniform int Block;
uniform float Scale;
uniform float MixAmount;
uniform int ShowVectors;

in vec2 uv;
out vec4 fragColor;

void main()
{
	//The host's viewport onto the picture. uv at a pixel centre is half a
	//texel from either boundary, so the floor is never in doubt.
	ivec2 p = clampTo( ivec2( floor( uv * vec2( ImageSize ) ) ), ImageSize );

	vec4 src = vec4( texelFetch( Source, p, 0 ) ) / 255.0;
	vec4 dec = vec4( texelFetch( Decoded, p, 0 ) ) / 255.0;
	vec4 c   = mix( src, dec, MixAmount );

	if( ShowVectors == 1 )
	{
		ivec2 block = p / Block;
		vec2 v      = vec2( texelFetch( Vectors, block, 0 ).xy ) * 0.5 * Scale;
		vec2 centre = vec2( block * Block ) + 0.5 * float( Block );
		vec2 q      = vec2( p ) + 0.5;

		//Distance from this pixel to the segment centre -> centre + v.
		float len = length( v );
		float d   = length( q - centre );
		if( len >= 0.5 )
		{
			vec2 dir = v / len;
			float t  = clamp( dot( q - centre, dir ), 0.0, len );
			d        = length( q - ( centre + dir * t ) );
		}
		if( d < 0.7 )
			c = vec4( 0.2, 1.0, 0.4, 1.0 );
		if( length( q - centre ) < 1.0 )
			c = vec4( 1.0, 1.0, 1.0, 1.0 );
	}

	fragColor = c;
}
)";

std::string assemble( const char* body )
{
	return std::string( "#version 410 core\n" ) + kCommon + body;
}

std::string assembleMotion( const char* body, int block, int pad )
{
	return std::string( "#version 410 core\n#define BLOCK " ) + std::to_string( block ) + "\n#define PAD "
	       + std::to_string( pad ) + "\n" + kCommon + body;
}

} // namespace residual::shaders
