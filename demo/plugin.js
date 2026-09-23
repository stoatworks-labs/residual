/**
 * Residual — browser demo.
 *
 * Datamosh, built as a real codec: a block-matching motion estimator on a luma
 * pyramid, a prediction from the decoder's own last frame, a quantised 8×8 DCT
 * residual in YCoCg-R, and a GOP — then controls that break one stage at a
 * time. Take away the I-frame at a cut and the decoder keeps painting the old
 * picture with the new scene's motion; take away the residual and nothing ever
 * corrects it; hold one frame's vectors and the pixels keep flowing.
 *
 * Every pass the plugin runs is here, on the GPU, from its own GLSL: copy,
 * luma, the pyramid, the chunked motion search and select at every level
 * (including the half-pel refinement), the SAD sum, predict, the four DCT
 * passes and the composite. The fifteen shader pieces below — `kVertexShader`,
 * `kCommon` and the thirteen bodies — are `source/Shaders.cpp` copied across
 * unedited, and they are assembled exactly as `shaders::assemble()` and
 * `shaders::assembleMotion()` assemble them. `demo/tools/check_shaders.py`
 * compares every piece character for character against the C++ and is called
 * from `tools/verify.sh`.
 *
 * The CPU half is small and it is a **port**: `source/Controls.cpp`,
 * `codec::quantStep`, `codec::dctBasis`, `codec::levelsFor` and the lattice
 * helpers, the motion search's chunk and ping-pong schedule (`searchLevel`),
 * the frame-type decision (`decideIntra`), the Drop I latch and the Vector
 * Hold count, all out of `Residual.cpp`. **Nothing checks that port but a
 * reader.**
 *
 * ------------------------------------------------------- what differs
 *
 * **The audio side.** The `Audio` buffer and `source/Onset.cpp` are absent: the
 * spectrum reaches the plugin through a Resolume FFT parameter and a browser
 * has no equivalent. Drop I's `On Onset` is in the list, because the plugin
 * declares it, and here nothing ever arms it — it behaves as Off.
 *
 * **Refresh is a toggle, not an event** (FF_TYPE_EVENT has no kit equivalent),
 * released by the renderer once the press is taken.
 *
 * **Search Range, GOP and Vector Hold are sliders.** They are FF_TYPE_INTEGER
 * in the plugin.
 *
 * **Two unused sampler bindings are swapped for a dummy.** The plugin binds
 * something to every sampler on every pass, including ones the pass has
 * switched off (`HasCoarse` 0, `HasPrevious` 0) — and in two places that
 * something is the texture the pass is drawing into: the coarsest level's
 * `Coarse` at a 32-pixel block, and `Previous` on the first chunk of a
 * half-pel search. Desktop GL shrugs at a feedback loop the shader never
 * reads; WebGL2 refuses the draw outright. So where the plugin would bind the
 * render target, this page binds a 1×1 integer texture instead. The shader
 * does not read it either way, so no pixel can differ.
 *
 * **The cuts.** A datamosh needs a cut, and the generated clips never cut. So
 * by default this page cuts to colour bars and back every three seconds — its
 * own switch, not the plugin's, and the Cuts control in the transport turns it
 * off (then changing the Clip is the cut).
 *
 * **The About block** — four buttons that open a browser.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, bindTexture } from './vendor/gl.js';
import { SOURCES, SourceRenderer } from './vendor/sources.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here.
//
// kCommon is a FRAGMENT, not a shader; the bodies are not shaders either
// until assemble() / assembleMotion() below put the version line and kCommon
// in front of them, exactly as the C++ does.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

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
`;

const COMMON = `
//floor( x / 2 ) for any int. GLSL leaves \`/\` and \`%\` undefined on negative
//operands and does not promise an arithmetic \`>>\`, so the value is offset to
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
`;

const COPY_BODY = `
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
`;

const LUMA_BODY = `
uniform usampler2D Source;

out uint fragColor;

void main()
{
	ivec2 p   = ivec2( gl_FragCoord.xy );
	ivec3 rgb = ivec3( texelFetch( Source, p, 0 ).rgb );
	fragColor = uint( toYCoCg( rgb ).x );
}
`;

const DOWNSAMPLE_BODY = `
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
`;

const MOTION_SAD_BODY = `
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
`;

const MOTION_SELECT_BODY = `
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
`;

const SAD_ROWS_BODY = `
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
`;

const SAD_TOTAL_BODY = `
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
`;

const PREDICT_BODY = `
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
`;

const DCT_ROW_BODY = `
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
`;

const DCT_COL_BODY = `
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
`;

const IDCT_COL_BODY = `
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
`;

const IDCT_ROW_BODY = `
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
`;

const COMPOSITE_BODY = `
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
`;

/** shaders::assemble — `#version 410 core` + kCommon + body. */
const assemble = (body) => `#version 410 core\n${COMMON}${body}`;

/** shaders::assembleMotion — the version, BLOCK and PAD, kCommon, body. */
const assembleMotion = (body, block, pad) => `#version 410 core\n#define BLOCK ${block}\n#define PAD ${pad}\n${COMMON}${body}`;

//===========================================================================
// Codec.h / Codec.cpp / Controls.cpp — ported.
//===========================================================================

const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);
const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
const lround = (v) => (v < 0 ? -Math.floor(-v + 0.5) : Math.floor(v + 0.5));

const TRANSFORM = 8; // codec::kTransform
const COARSEST_BLOCK = 4; // codec::kCoarsestBlock
const MAX_LEVELS = 4; // codec::kMaxLevels
const REFINE_WINDOW = 2; // codec::kRefineWindow
const BLOCK_SIZES = [8, 16, 32]; // codec::kBlockSizes
const CHUNK_SIDE = 9; // Residual.cpp kChunkSide

const blocksAcross = (pixels, block) => Math.floor((pixels + block - 1) / block);
const levelSize = (pixels, level) => (pixels + (1 << level) - 1) >> level;
const rangeAtLevel = (range, level) => (range + (1 << level) - 1) >> level;
function levelsFor(block) {
  let levels = 1;
  while ((block >> (levels - 1)) > COARSEST_BLOCK) levels += 1;
  return levels;
}

/** codec::quantStep — zero (bypass) at 0, then 2 .. 512 geometrically. */
const quantStep = (q) => (q <= 0 ? 0 : Math.pow(2, 1 + 8 * Math.min(1, q)));

/** codec::dctBasis — computed in double, handed over as float. */
function dctBasis() {
  const out = new Float32Array(TRANSFORM * TRANSFORM);
  for (let u = 0; u < TRANSFORM; u += 1) {
    const a = u === 0 ? Math.sqrt(1 / TRANSFORM) : Math.sqrt(2 / TRANSFORM);
    for (let x = 0; x < TRANSFORM; x += 1) {
      out[u * TRANSFORM + x] = a * Math.cos(((2 * x + 1) * u * Math.PI) / (2 * TRANSFORM));
    }
  }
  return out;
}

const roundedInt = (v, lo, hi) => clamp(lround(v), lo, hi);
const DROP_OFF = 0;
const DROP_NEXT = 1;
const DROP_ALL = 2;
const DROP_ON_ONSET = 3;
const DROP_NAMES = ['Off', 'Next', 'All', 'On Onset'];

// The FF_TYPE_INTEGER parameters, as 0..1 sliders over the plugin's own range.
const INTEGERS = { searchRange: [1, 32], gop: [1, 250], vectorHold: [1, 60] };
const integerValue = (id, v) => {
  const [lo, hi] = INTEGERS[id];
  return lo + Math.round(clamp01(v) * (hi - lo));
};
const integerParam = (id, value) => {
  const [lo, hi] = INTEGERS[id];
  return (value - lo) / (hi - lo);
};

const controls = {
  blockSize: (option) => BLOCK_SIZES[roundedInt(option, 0, BLOCK_SIZES.length - 1)],
  // SearchRange / GopFrames / VectorHoldFrames clamp an integer that is
  // already in range, so the slider's own mapping is the whole conversion.
  searchRange: (v) => integerValue('searchRange', v),
  gopFrames: (v) => integerValue('gop', v),
  vectorHoldFrames: (v) => integerValue('vectorHold', v),
  sceneThreshold: (v) => 2 * Math.pow(64 / 2, clamp01(v)),
  quantStep: (v) => quantStep(v),
  residualGain: (v) => 2 * clamp01(v),
  vectorScale: (v) => 4 * clamp01(v),
};

//===========================================================================
// Buffer.cpp — an integer (or float) render target, NEAREST, cleared on
// allocation. WebGL2 zero-fills new texture storage, which is the clear.
//===========================================================================

class Buffer {
  constructor(gl) {
    this.gl = gl;
    this.texture = null;
    this.fbo = null;
    this.width = 0;
    this.height = 0;
    this.format = 0;
  }

  /** Returns true when it (re)allocated — the contents were lost. */
  ensure(width, height, format) {
    const gl = this.gl;
    if (this.texture && this.width === width && this.height === height && this.format === format) return false;
    this.destroy();
    this.width = width;
    this.height = height;
    this.format = format;
    this.texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.texture);
    gl.texStorage2D(gl.TEXTURE_2D, 1, format, width, height);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    this.fbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, this.texture, 0);
    const status = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.bindTexture(gl.TEXTURE_2D, null);
    if (status !== gl.FRAMEBUFFER_COMPLETE) {
      throw new Error(`codec buffer incomplete (0x${status.toString(16)}) at ${width}x${height}`);
    }
    return true;
  }

  bindAsTarget() {
    const gl = this.gl;
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.fbo);
    gl.viewport(0, 0, this.width, this.height);
  }

  destroy() {
    const gl = this.gl;
    if (this.fbo) gl.deleteFramebuffer(this.fbo);
    if (this.texture) gl.deleteTexture(this.texture);
    this.fbo = null;
    this.texture = null;
    this.width = 0;
    this.height = 0;
  }
}

//===========================================================================
// Residual.cpp — the frame, ported.
//===========================================================================

const MOTION_VARIANTS = [
  [8, 0], [16, 0], [32, 0], [4, 2], [8, 4], [16, 8],
];
const motionVariantFor = (block, pad) => MOTION_VARIANTS.findIndex(([b, p]) => b === block && p === pad);

const CUT_SECONDS = 3;

function createRenderer(gl, quad) {
  const program = (body, label) => new Program(gl, VERTEX, assemble(body), label);
  const copyShader = program(COPY_BODY, 'copy');
  const lumaShader = program(LUMA_BODY, 'luma');
  const downsampleShader = program(DOWNSAMPLE_BODY, 'downsample');
  const sadRowsShader = program(SAD_ROWS_BODY, 'sadRows');
  const sadTotalShader = program(SAD_TOTAL_BODY, 'sadTotal');
  const predictShader = program(PREDICT_BODY, 'predict');
  const dctRowShader = program(DCT_ROW_BODY, 'dctRow');
  const dctColShader = program(DCT_COL_BODY, 'dctCol');
  const idctColShader = program(IDCT_COL_BODY, 'idctCol');
  const idctRowShader = program(IDCT_ROW_BODY, 'idctRow');
  const compositeShader = program(COMPOSITE_BODY, 'composite');
  const motionSad = MOTION_VARIANTS.map(([b, p]) => new Program(gl, VERTEX, assembleMotion(MOTION_SAD_BODY, b, p), `motionSad ${b}/${p}`));
  const motionSelect = MOTION_VARIANTS.map(([b, p]) => new Program(gl, VERTEX, assembleMotion(MOTION_SELECT_BODY, b, p), `motionSelect ${b}/${p}`));

  const basis = dctBasis();

  const source = [new Buffer(gl), new Buffer(gl)];
  const decoded = [new Buffer(gl), new Buffer(gl)];
  const pyramid = [0, 1].map(() => [0, 1, 2, 3].map(() => new Buffer(gl)));
  const vectors = [0, 1, 2, 3].map(() => new Buffer(gl));
  const vectorsScratch = new Buffer(gl);
  const sads = new Buffer(gl);
  const predicted = new Buffer(gl);
  const coef = [new Buffer(gl), new Buffer(gl)];
  const sadRows = new Buffer(gl);
  const sadTotal = new Buffer(gl);

  // See the header: what this page binds where the plugin would bind the pass's
  // own render target to a sampler the pass has switched off.
  const dummyInt = new Buffer(gl);
  dummyInt.ensure(1, 1, gl.RGBA32I);
  const safe = (texture, target) => (target && texture === target.texture ? dummyInt.texture : texture);

  // The cut this page makes, which is not the plugin's.
  const cutter = new SourceRenderer(gl, quad);
  const barsClip = SOURCES.find((s) => s.id === 'bars');

  // Codec state.
  let current = 0;
  let referenceValid = false;
  let pictureWidth = 0;
  let pictureHeight = 0;
  let blocksX = 0;
  let blocksY = 0;
  let framesSinceIntra = 0;
  let holdRemaining = 0;
  let refreshPending = false;
  let dropArmed = false;
  let lastDrop = DROP_OFF;
  let lastMeanSad = 0;
  const readback = new Uint32Array(4);

  // What the readout under the picture reports.
  const stats = { frames: 0, intra: 0, dropped: 0, cuts: 0 };
  const line = document.createElement('p');
  line.className = 'stage__status';
  document.querySelector('.stage')?.append(line);

  function ensureBuffers(width, height, blockSize) {
    let referenceLost = !referenceValid;
    let lost = false;
    for (let i = 0; i < 2; i += 1) {
      lost = source[i].ensure(width, height, gl.RGBA8UI) || lost;
      lost = decoded[i].ensure(width, height, gl.RGBA8UI) || lost;
      for (let l = 0; l < MAX_LEVELS; l += 1) pyramid[i][l].ensure(levelSize(width, l), levelSize(height, l), gl.R8UI);
    }
    referenceLost = referenceLost || lost;
    pictureWidth = width;
    pictureHeight = height;

    const bx = blocksAcross(width, blockSize);
    const by = blocksAcross(height, blockSize);
    for (let l = 0; l < MAX_LEVELS; l += 1) {
      // New vectors are zero, which is nothing to hold on to.
      if (vectors[l].ensure(bx, by, gl.RGBA32I)) holdRemaining = 0;
    }
    vectorsScratch.ensure(bx, by, gl.RGBA32I);
    sads.ensure(bx * CHUNK_SIDE, by * CHUNK_SIDE, gl.R32I);
    blocksX = bx;
    blocksY = by;

    predicted.ensure(width, height, gl.RGBA8UI);
    const paddedW = blocksAcross(width, TRANSFORM) * TRANSFORM;
    const paddedH = blocksAcross(height, TRANSFORM) * TRANSFORM;
    for (const c of coef) c.ensure(paddedW, paddedH, gl.RGBA32F);
    sadRows.ensure(1, by, gl.R32UI);
    sadTotal.ensure(1, 1, gl.R32UI);
    return referenceLost;
  }

  const setIVec2 = (shader, name, x, y) => {
    const loc = shader.location(name);
    if (loc !== null) gl.uniform2i(loc, x, y);
  };

  function searchLevel(level, levels, blockSize, range, halfPel, cur, prv) {
    const levelBlock = blockSize >> level;
    const levelPad = level === 0 ? 0 : levelBlock / 2;
    const variant = motionVariantFor(levelBlock, levelPad);
    const sadShader = motionSad[variant];
    const selectShader = motionSelect[variant];

    const coarsest = level === levels - 1;
    const levelRange = rangeAtLevel(range, level);
    const window = coarsest ? levelRange : REFINE_WINDOW;
    const side = 2 * window + 1;
    const chunks = Math.floor((side + CHUNK_SIDE - 1) / CHUNK_SIDE);
    const passes = chunks * chunks;

    const imageW = pyramid[cur][level].width;
    const imageH = pyramid[cur][level].height;

    const refine = level === 0 && halfPel;
    const landing = refine ? vectorsScratch : vectors[level];
    const other = refine ? vectors[level] : vectorsScratch;
    let previous = null;
    const coarseTexture = vectors[Math.min(level + 1, MAX_LEVELS - 1)].texture;

    for (let pass = 0; pass < passes; pass += 1) {
      const cx = pass % chunks;
      const cy = Math.floor(pass / chunks);
      const offsetX = -window + cx * CHUNK_SIDE;
      const offsetY = -window + cy * CHUNK_SIDE;
      const last = pass === passes - 1;
      const target = last ? landing : ((passes - 1 - pass) % 2 === 0 ? landing : other);

      // The SAD of every candidate in this chunk, one fragment each.
      sads.bindAsTarget();
      gl.viewport(0, 0, blocksX * CHUNK_SIDE, blocksY * CHUNK_SIDE);
      sadShader.use();
      bindTexture(gl, 0, pyramid[cur][level].texture);
      bindTexture(gl, 1, pyramid[prv][level].texture);
      bindTexture(gl, 2, coarseTexture);
      bindTexture(gl, 3, vectorsScratch.texture);
      sadShader.setSampler('Cur', 0);
      sadShader.setSampler('Prev', 1);
      sadShader.setSampler('Coarse', 2);
      sadShader.setSampler('Selected', 3);
      sadShader.setInt('HasCoarse', coarsest ? 0 : 1);
      setIVec2(sadShader, 'ImageSize', imageW, imageH);
      sadShader.setInt('Range', levelRange);
      sadShader.setInt('Window', window);
      sadShader.setInt('ChunkSide', CHUNK_SIDE);
      setIVec2(sadShader, 'ChunkOffset', offsetX, offsetY);
      sadShader.setInt('HalfPelMode', 0);
      quad.draw();

      // The best so far, one fragment per block.
      target.bindAsTarget();
      selectShader.use();
      bindTexture(gl, 0, pyramid[cur][level].texture);
      bindTexture(gl, 1, pyramid[prv][level].texture);
      bindTexture(gl, 2, sads.texture);
      bindTexture(gl, 3, safe(coarseTexture, target));
      bindTexture(gl, 4, safe(previous ? previous.texture : vectorsScratch.texture, target));
      selectShader.setSampler('Cur', 0);
      selectShader.setSampler('Prev', 1);
      selectShader.setSampler('Sads', 2);
      selectShader.setSampler('Coarse', 3);
      selectShader.setSampler('Previous', 4);
      selectShader.setInt('HasCoarse', coarsest ? 0 : 1);
      selectShader.setInt('HasPrevious', previous ? 1 : 0);
      setIVec2(selectShader, 'ImageSize', imageW, imageH);
      selectShader.setInt('Range', levelRange);
      selectShader.setInt('ChunkSide', CHUNK_SIDE);
      setIVec2(selectShader, 'ChunkOffset', offsetX, offsetY);
      selectShader.setInt('TieBreak', 1);
      selectShader.setInt('HalfPelMode', 0);
      selectShader.setInt('OutputHalfPel', level === 0 && !refine ? 1 : 0);
      quad.draw();

      previous = target;
    }

    if (!refine) return;

    // The eight half-pel neighbours of the whole-pel winner, then the merge.
    sads.bindAsTarget();
    gl.viewport(0, 0, blocksX * 3, blocksY * 3);
    sadShader.use();
    bindTexture(gl, 0, pyramid[cur][level].texture);
    bindTexture(gl, 1, pyramid[prv][level].texture);
    bindTexture(gl, 2, vectors[1].texture);
    bindTexture(gl, 3, vectorsScratch.texture);
    sadShader.setSampler('Cur', 0);
    sadShader.setSampler('Prev', 1);
    sadShader.setSampler('Coarse', 2);
    sadShader.setSampler('Selected', 3);
    sadShader.setInt('HasCoarse', 0);
    setIVec2(sadShader, 'ImageSize', imageW, imageH);
    sadShader.setInt('Range', levelRange);
    sadShader.setInt('Window', 1);
    sadShader.setInt('ChunkSide', 3);
    setIVec2(sadShader, 'ChunkOffset', -1, -1);
    sadShader.setInt('HalfPelMode', 1);
    quad.draw();

    vectors[0].bindAsTarget();
    selectShader.use();
    bindTexture(gl, 0, pyramid[cur][level].texture);
    bindTexture(gl, 1, pyramid[prv][level].texture);
    bindTexture(gl, 2, sads.texture);
    bindTexture(gl, 3, vectors[1].texture);
    bindTexture(gl, 4, vectorsScratch.texture);
    selectShader.setSampler('Cur', 0);
    selectShader.setSampler('Prev', 1);
    selectShader.setSampler('Sads', 2);
    selectShader.setSampler('Coarse', 3);
    selectShader.setSampler('Previous', 4);
    selectShader.setInt('HasCoarse', 0);
    selectShader.setInt('HasPrevious', 1);
    setIVec2(selectShader, 'ImageSize', imageW, imageH);
    selectShader.setInt('Range', levelRange);
    selectShader.setInt('ChunkSide', 3);
    setIVec2(selectShader, 'ChunkOffset', -1, -1);
    selectShader.setInt('TieBreak', 1);
    selectShader.setInt('HalfPelMode', 1);
    selectShader.setInt('OutputHalfPel', 1);
    quad.draw();
  }

  /** Residual::decideIntra, ported. */
  function decideIntra(p, referenceLost, estimated, meanSad) {
    const drop = roundedInt(p('dropI'), 0, DROP_NAMES.length - 1);
    const gop = controls.gopFrames(p('gop'));
    const cuts = p('sceneCut') >= 0.5;

    const sceneCut = estimated && cuts && !referenceLost && meanSad > controls.sceneThreshold(p('sceneThreshold'));
    const due = framesSinceIntra >= gop;
    const wanted = referenceLost || refreshPending || due || sceneCut;

    let intra = wanted;
    let dropped = false;
    if (wanted && !referenceLost && !refreshPending) {
      if (drop === DROP_NEXT || drop === DROP_ON_ONSET) {
        if (dropArmed) {
          intra = false;
          dropArmed = false;
          dropped = true;
        }
      } else if (drop === DROP_ALL) {
        intra = false;
        dropped = true;
      }
    }
    refreshPending = false;

    if (intra || dropped) framesSinceIntra = 1;
    else framesSinceIntra += 1;

    return { intra, dropped, sceneCut };
  }

  return {
    render({ input: clip, params, width, height, time, variant }) {
      const p = (id) => params.get(id);

      // This page's cut: every CUT_SECONDS, to colour bars and back.
      let input = clip;
      if (variant === 'auto' && Math.floor(time / CUT_SECONDS) % 2 === 1) {
        input = cutter.render(barsClip, width, height, time);
      }

      //---------------------------------------------------------------
      // The two events. Refresh is FF_TYPE_EVENT in the plugin; a toggle
      // here, released once the press is taken. Drop I = Next arms on
      // selection, and leaving the mode disarms (SetFloatParameter).
      //---------------------------------------------------------------
      if (p('refresh') > 0.5) {
        refreshPending = true;
        params.set('refresh', 0);
      }
      const dropNow = roundedInt(p('dropI'), 0, DROP_NAMES.length - 1);
      if (dropNow !== lastDrop) {
        dropArmed = dropNow === DROP_NEXT;
        lastDrop = dropNow;
      }

      //---------------------------------------------------------------
      // What the controls say.
      //---------------------------------------------------------------
      const blockSize = controls.blockSize(p('blockSize'));
      const range = controls.searchRange(p('searchRange'));
      const halfPel = p('halfPel') >= 0.5;
      const hold = controls.vectorHoldFrames(p('vectorHold'));
      const stepLuma = controls.quantStep(p('q'));
      const stepChroma = controls.quantStep(p('chromaQ'));
      const gain = controls.residualGain(p('residualGain'));
      const scale = controls.vectorScale(p('vectorScale'));
      const levels = levelsFor(blockSize);

      const referenceLost = ensureBuffers(width, height, blockSize);
      if (referenceLost) holdRemaining = 0;

      current = 1 - current;
      const cur = current;
      const prv = 1 - current;

      gl.disable(gl.BLEND);

      // 1. The picture, exactly, into an integer texture of ours.
      source[cur].bindAsTarget();
      copyShader.use();
      bindTexture(gl, 0, input.texture);
      copyShader.setSampler('Source', 0);
      quad.draw();

      // 2. The luma pyramid, every level, every frame.
      pyramid[cur][0].bindAsTarget();
      lumaShader.use();
      bindTexture(gl, 0, source[cur].texture);
      lumaShader.setSampler('Source', 0);
      quad.draw();
      for (let l = 1; l < MAX_LEVELS; l += 1) {
        pyramid[cur][l].bindAsTarget();
        downsampleShader.use();
        bindTexture(gl, 0, pyramid[cur][l - 1].texture);
        downsampleShader.setSampler('Fine', 0);
        setIVec2(downsampleShader, 'FineSize', pyramid[cur][l - 1].width, pyramid[cur][l - 1].height);
        quad.draw();
      }

      // 3. Audio: absent. Nothing ever arms Drop I = On Onset here.

      // 4. Motion, coarsest level first. Skipped while vectors are held.
      const estimate = !referenceLost && holdRemaining === 0;
      let meanSad = lastMeanSad;
      if (estimate) {
        for (let l = levels - 1; l >= 0; l -= 1) searchLevel(l, levels, blockSize, range, halfPel, cur, prv);

        sadRows.bindAsTarget();
        sadRowsShader.use();
        bindTexture(gl, 0, vectors[0].texture);
        sadRowsShader.setSampler('Vectors', 0);
        sadRowsShader.setInt('BlocksX', blocksX);
        quad.draw();

        sadTotal.bindAsTarget();
        sadTotalShader.use();
        bindTexture(gl, 0, sadRows.texture);
        sadTotalShader.setSampler('Rows', 0);
        sadTotalShader.setInt('BlocksY', blocksY);
        quad.draw();

        // One unsigned int back from the GPU, as the plugin reads it. WebGL2
        // only promises RGBA_INTEGER for an unsigned-integer read, so four come
        // back and the first is the one.
        gl.readPixels(0, 0, 1, 1, gl.RGBA_INTEGER, gl.UNSIGNED_INT, readback);
        meanSad = readback[0] / (width * height);
        holdRemaining = hold - 1;
      } else if (holdRemaining > 0) {
        holdRemaining -= 1;
      }
      lastMeanSad = meanSad;

      // 5. I or P.
      const { intra, dropped, sceneCut } = decideIntra(p, referenceLost, estimate, meanSad);

      // 6. The prediction: the last DECODED frame, block-copied.
      if (!intra) {
        predicted.bindAsTarget();
        predictShader.use();
        bindTexture(gl, 0, decoded[prv].texture);
        bindTexture(gl, 1, vectors[0].texture);
        predictShader.setSampler('Reference', 0);
        predictShader.setSampler('Vectors', 1);
        setIVec2(predictShader, 'ImageSize', width, height);
        predictShader.setInt('Block', blockSize);
        predictShader.set('Scale', scale);
        quad.draw();
      }

      // 7. The residual, forward, quantised.
      coef[0].bindAsTarget();
      dctRowShader.use();
      bindTexture(gl, 0, source[cur].texture);
      bindTexture(gl, 1, predicted.texture);
      dctRowShader.setSampler('Source', 0);
      dctRowShader.setSampler('Predicted', 1);
      setIVec2(dctRowShader, 'ImageSize', width, height);
      dctRowShader.setInt('Intra', intra ? 1 : 0);
      dctRowShader.setArray('Basis', basis, 1);
      quad.draw();

      coef[1].bindAsTarget();
      dctColShader.use();
      bindTexture(gl, 0, coef[0].texture);
      dctColShader.setSampler('Rows', 0);
      dctColShader.set('StepLuma', stepLuma);
      dctColShader.set('StepChroma', stepChroma);
      dctColShader.setArray('Basis', basis, 1);
      quad.draw();

      // 8. Back, and reconstructed: the decoded frame.
      coef[0].bindAsTarget();
      idctColShader.use();
      bindTexture(gl, 0, coef[1].texture);
      idctColShader.setSampler('Coef', 0);
      idctColShader.setArray('Basis', basis, 1);
      quad.draw();

      decoded[cur].bindAsTarget();
      idctRowShader.use();
      bindTexture(gl, 0, coef[0].texture);
      bindTexture(gl, 1, source[cur].texture);
      bindTexture(gl, 2, predicted.texture);
      idctRowShader.setSampler('Cols', 0);
      idctRowShader.setSampler('Source', 1);
      idctRowShader.setSampler('Predicted', 2);
      setIVec2(idctRowShader, 'ImageSize', width, height);
      idctRowShader.setInt('Intra', intra ? 1 : 0);
      // I-frames reconstruct at unity, so Refresh at gain 0 still paints.
      idctRowShader.set('Gain', intra ? 1.0 : gain);
      idctRowShader.setArray('Basis', basis, 1);
      quad.draw();

      // 9. To the canvas.
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);
      compositeShader.use();
      bindTexture(gl, 0, source[cur].texture);
      bindTexture(gl, 1, decoded[cur].texture);
      bindTexture(gl, 2, vectors[0].texture);
      compositeShader.setSampler('Source', 0);
      compositeShader.setSampler('Decoded', 1);
      compositeShader.setSampler('Vectors', 2);
      setIVec2(compositeShader, 'ImageSize', width, height);
      compositeShader.setInt('Block', blockSize);
      compositeShader.set('Scale', scale);
      compositeShader.set('MixAmount', clamp01(p('mix')));
      compositeShader.setInt('ShowVectors', p('showVectors') >= 0.5 ? 1 : 0);
      quad.draw();

      for (let unit = 4; unit >= 0; unit -= 1) bindTexture(gl, unit, null);
      referenceValid = true;

      stats.frames += 1;
      if (intra) stats.intra += 1;
      if (dropped) stats.dropped += 1;
      if (sceneCut) stats.cuts += 1;
      const kind = intra ? 'I' : dropped ? 'P (an I-frame dropped)' : 'P';
      line.textContent = `This frame: ${kind} · mean SAD ${meanSad.toFixed(2)} codes/px${estimate ? '' : ' (held vectors)'} · so far ${stats.intra} I-frames, ${stats.dropped} dropped, ${stats.cuts} scene cuts detected, in ${stats.frames} frames`;
    },
  };
}

//===========================================================================
// The page.
//===========================================================================

const std = (id, name, def, group, display, hint) => ({ id, name, type: 'standard', default: def, group, display, hint });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });
const integer = (id, name, value, group, unit, hint) => ({
  id,
  name,
  type: 'standard',
  default: integerParam(id, value),
  group,
  display: (v) => `${integerValue(id, v)}${unit}`,
  hint,
});
const stepLabel = (v) => {
  const s = quantStep(v);
  return s === 0 ? 'bypass' : `step ${s < 10 ? s.toFixed(1) : Math.round(s)}`;
};

mountDemo({
  name: 'Residual',
  pluginId: 'RS01',
  tagline:
    'Datamosh, built as a real codec. A block-matching motion estimator, a prediction from the decoder\'s own last frame, a quantised DCT residual and a GOP — and then controls that break one stage at a time. Take away the I-frame at a cut and the decoder keeps painting the old picture with the new scene\'s motion; take away the residual and nothing ever corrects it; hold one frame\'s vectors and the pixels keep flowing. Start with Drop I on All and Residual Gain at zero. Refresh is the clean-up button.',
  repo: 'https://github.com/stoatworks-labs/residual',
  page: 'https://stoatworks-labs.com/software/residual/',
  video: 'https://www.youtube.com/watch?v=2KrF01zB9mA',

  blurb:
    "It is Residual's own GLSL — the whole codec, motion search to reconstruction — ported from the repository to WebGL2 and running on generated clips in this page, with a cut to colour bars every three seconds made by this page, because a datamosh needs a cut and the clips never have one. Same parameters, same maths, no install.",

  // The two DCT intermediates are RGBA32F render targets.
  needFloat: true,
  // The decoded frame carries the source's alpha through.
  showBackdrop: true,

  variants: {
    label: 'Cuts',
    default: 'auto',
    options: [
      { id: 'auto', name: 'To bars every 3 s', hint: "This page cuts the input to colour bars and back every three seconds. Not the plugin's — the generated clips never cut, and a datamosh is what a decoder does at a cut." },
      { id: 'none', name: 'Only when you change the clip', hint: 'No cuts from this page. Pick a different Clip to cut.' },
    ],
  },

  params: [
    opt('blockSize', 'Block Size', ['8', '16', '32'], 1, 'Encoder',
      'The motion block, in pixels. The residual is always 8×8.'),
    integer('searchRange', 'Search Range', 16, 'Encoder', ' px',
      '±1 to ±32 pixels, searched hierarchically on a luma pyramid. An integer the plugin takes typed; a slider here.'),
    bool('halfPel', 'Half Pel', 0, 'Encoder',
      'Refine each vector to half a pixel, with the codec\'s rounded averages.'),
    integer('gop', 'GOP', 30, 'Encoder', ' frames',
      'An I-frame every N frames, 1 to 250. 1 is a codec with no prediction in it.'),
    bool('sceneCut', 'Scene Cut', 1, 'Encoder',
      'Insert an I-frame where the motion-compensated difference says the picture changed, as encoders do.'),
    std('sceneThreshold', 'Scene Threshold', 0.5, 'Encoder',
      (v) => `${controls.sceneThreshold(v).toFixed(1)} codes/px`,
      'How large that difference has to be: 2 to 64 codes per pixel, geometric. A cut leaves tens; ordinary motion a few.'),

    std('q', 'Q', 0.3, 'Residual', stepLabel,
      'The luma quantiser. 0 is a real bypass and the codec is lossless; above it the step runs from 2 to 512.'),
    std('residualGain', 'Residual Gain', 0.5, 'Residual',
      (v) => `×${controls.residualGain(v).toFixed(2)}`,
      'What the dequantised residual is multiplied by before it is added back. 1.0 is honest; 0 is a decoder that never corrects. I-frames always reconstruct at unity.'),
    std('chromaQ', 'Chroma Q', 0.4, 'Residual', stepLabel, 'The quantiser for Co and Cg.'),

    opt('dropI', 'Drop I', DROP_NAMES, 0, 'Mosh',
      'Off; Next (the next I-frame, once — choose it again to re-arm); All (no I-frame ever arrives); On Onset (an audio onset arms it — there is no audio on this page, so here it never arms).'),
    integer('vectorHold', 'Vector Hold', 1, 'Mosh', ' frames',
      'Reuse one frame\'s vectors for 1 to 60 frames, and the pixels keep flowing the way they were.'),
    std('vectorScale', 'Vector Scale', 0.25, 'Mosh',
      (v) => `×${controls.vectorScale(v).toFixed(2)}`,
      '0 to 4×, in half-pel units. 1 is the vectors as found.'),
    bool('refresh', 'Refresh', 0, 'Mosh',
      'Force one I-frame: the clean-up button. FF_TYPE_EVENT in the plugin, drawn by a host as a button; a toggle here that the renderer releases itself.'),

    bool('showVectors', 'Show Vectors', 0, 'View', 'Draw the vector field over the picture.'),
    std('mix', 'Mix', 1, 'View', (v) => `${Math.round(clamp01(v) * 100)}%`, 'Crossfade with the untouched clip.'),
  ],

  sources: ['scene', 'grid', 'spot', 'detail', 'ramp', 'bars'],

  // Combinations chosen for this page. The plugin ships no factory presets, so
  // every value is a real control at a real position, but the choice is ours.
  presets: {
    'The classic mosh (Drop I All, gain 0)': { dropI: DROP_ALL, residualGain: 0 },
    'Drop I All, residual honest': { dropI: DROP_ALL },
    'Drop the next I-frame only': { dropI: DROP_NEXT, residualGain: 0.2 },
    'Bloom: held vectors, no residual': { dropI: DROP_ALL, residualGain: 0, vectorHold: integerParam('vectorHold', 30) },
    'Vectors doubled': { dropI: DROP_ALL, residualGain: 0, vectorScale: 0.5 },
    'Show the vectors': { showVectors: 1 },
    'Crushed (Q and Chroma Q high)': { q: 0.8, chromaQ: 0.9 },
    'Big blocks, no correction': { blockSize: 2, dropI: DROP_ALL, residualGain: 0 },
    'Lossless (Q 0)': { q: 0, chromaQ: 0 },
  },

  differences: [
    'The audio side is not here. The plugin\'s 64-bin Audio buffer and its onset detector are absent rather than present and dead: the spectrum reaches the plugin through a Resolume FFT parameter and a browser has no equivalent. Drop I still lists On Onset, because the plugin declares it, but nothing on this page can arm it, so it behaves as Off.',
    'The cut to colour bars every three seconds is this page\'s, not the plugin\'s. The generated clips never cut, and without a cut there is nothing for a dropped I-frame to fail at. Set Cuts to “Only when you change the clip” in the transport to switch it off; then picking a different Clip is the cut.',
    'Refresh is a toggle here, not an event: FFGL has FF_TYPE_EVENT and the kit\'s parameter model does not, so the renderer releases it once it has taken the press. Search Range, GOP and Vector Hold are FF_TYPE_INTEGER in the plugin, typed as numbers; here each is a slider that lands on every integer in the plugin\'s own range.',
    'The CPU half is a hand port, and nothing checks it but a reader: the conversions in Controls.cpp, quantStep and the DCT basis from Codec.cpp, the motion search\'s chunk-and-merge schedule, the frame-type decision, the Drop I latch and the Vector Hold count, all out of Residual.cpp. demo/tools/check_shaders.py compares all fifteen shader pieces, and it fails the repository\'s verify script if a character of them drifts from source/Shaders.cpp.',
    'Two sampler bindings differ, and no pixel can. The plugin binds a texture to every sampler on every pass, including ones the pass has switched off — and twice that texture is the pass\'s own render target (the coarsest level\'s Coarse at a 32-pixel block, and Previous on the first chunk of a half-pel search). Desktop GL tolerates a feedback loop the shader never reads; WebGL2 refuses the draw. Here a 1×1 integer texture is bound instead. The shader does not read it either way.',
    'The codec runs one frame per frame this page draws, at your display\'s rate, and it keeps its reference across frames — so changing a control while paused draws, and codes, one more frame. The GOP is counted in those frames.',
    'The sum of SADs is read back as four unsigned ints rather than one (WebGL2 only promises RGBA_INTEGER for an integer read); the first is the plugin\'s number.',
    'The presets are this page\'s own combinations. The plugin ships no factory presets; every value is a real control at a real position, but the choice is ours. The line under the picture — frame type, mean SAD, counts — is this page\'s readout of the codec\'s own decisions; the plugin logs the same things but draws nothing of the kind.',
    'The About block is absent: it is four buttons that open a browser.',
    'The plugin\'s numerical proof — Q 0 lossless bit for bit, a whole-pel block copy a pure fetch, an I-frame a function of its source alone, the GOP landing on the right frame, a scene cut detected, and dropped I-frames leaving the old picture moving — is an offline harness in the repository. Nothing on this page measures anything.',
  ],

  createRenderer,
});
