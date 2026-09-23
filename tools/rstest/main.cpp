/**
	rstest -- render Residual offline, and check the codec does what a codec does.

	Everything a decoder does is integer arithmetic with a closed form, so the
	checks here are mostly BITWISE: the decoded frame after a cut with no
	residual IS the previous decoded frame block-copied by the vectors; at Q 0
	the decoded frame IS the source; an I-frame IS a function of its source
	alone. The vectors and the decoded frame are read straight out of the
	plugin's integer textures with GL_RGBA_INTEGER, so no float-to-fixed
	conversion sits between the shader and the byte being asserted on.

	    rstest --out /tmp/f.png    render the demo card through the plugin
	    rstest --card /tmp/c.png   the demo card on its own
	    rstest --list              every parameter, its kind and its range
	    rstest --transform         YCoCg-R reversible, DCT orthonormal. No GL
	    rstest --vectors           a translated texture gives exactly (dx, dy)
	    rstest --lossless          Q 0, gain 1: decoded == source, bitwise
	    rstest --mosh              no I, no residual: decoded == block copy, bitwise
	    rstest --gop               I-frames land where the GOP says, and are
	                               functions of the source alone
	    rstest --drift             closed-loop drift grows, one code a frame
	    rstest --resize            a resize restarts with an I-frame, never black
	    rstest --onset             the first onset after a trigger drops an I-frame
	    rstest --negative          the checks above can actually fail
	    rstest --bench             the render cost, 720p through 4K

	Every check that touches a picture runs at 320x180 -- what CI uses -- and
	at least one other raster. Every tolerance is a lattice: one code value,
	one block, or nothing at all.
*/

#include "Codec.h"
#include "Controls.h"
#include "Residual.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace residual;

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type,
               const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height,
               const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(),
	               static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Source frames. Row 0 at the BOTTOM throughout, which is GL's convention and
// the plugin's; only the PNG writer flips, once.
//---------------------------------------------------------------------------
using Frame = std::vector< uint8_t >;

uint32_t hashInt( uint32_t x )
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

double hash01( uint32_t x )
{
	return static_cast< double >( hashInt( x ) ) / 4294967296.0;
}

int floorDiv( int a, int b )
{
	return a >= 0 ? a / b : -( ( -a + b - 1 ) / b );
}

/// Value noise on a lattice of cell size `s`, bilinear between corners, at an
/// INTEGER position. The whole point: sampled at ( x - dx, y - dy ) for
/// integer dx, dy this is the same set of numbers, moved -- so a translated
/// frame is an exact translation, and the vector it must return is (dx, dy)
/// with no sub-pixel anything to argue about.
double lattice( int x, int y, int s, uint32_t seed )
{
	const int cx    = floorDiv( x, s );
	const int cy    = floorDiv( y, s );
	const double fx = static_cast< double >( x - cx * s ) / s;
	const double fy = static_cast< double >( y - cy * s ) / s;

	auto corner = [ & ]( int ix, int iy ) {
		return hash01( hashInt( static_cast< uint32_t >( ix ) * 73856093u
		                        ^ static_cast< uint32_t >( iy ) * 19349663u ^ seed ) );
	};

	const double a = corner( cx, cy );
	const double b = corner( cx + 1, cy );
	const double c = corner( cx, cy + 1 );
	const double d = corner( cx + 1, cy + 1 );
	return ( a * ( 1 - fx ) + b * fx ) * ( 1 - fy ) + ( c * ( 1 - fx ) + d * fx ) * fy;
}

/// Content at three scales -- 16, 4 and 1 pixels -- so the coarsest pyramid
/// level still sees structure to match on and the finest sees detail that
/// pins the vector to one pixel. Pure white noise would defeat the pyramid:
/// the 4x4 mean of a shifted noise field is not a shift of the 4x4 means.
double texture( int x, int y, uint32_t seed )
{
	return 0.5 * lattice( x, y, 16, seed ) + 0.3 * lattice( x, y, 4, seed + 101u )
	       + 0.2 * lattice( x, y, 1, seed + 202u );
}

struct Rect
{
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;//half-open, in TEXTURE coordinates
	bool contains( int x, int y ) const
	{
		return x >= x0 && x < x1 && y >= y0 && y < y1;
	}
};

/// A textured frame: the texture function sampled at ( x + vx, y + vy ), three
/// channels from three seeds, with an optional FLAT rectangle (in texture
/// coordinates, so it translates with everything else).
///
/// (vx, vy) is the MOTION VECTOR the codec must return, in the codec's own
/// convention: a vector points from a block to where it came from, so the
/// prediction is `reference( p + v )`. A frame sampled at ( x + vx, y + vy )
/// holds, at p, what the unshifted frame holds at p + v -- which is exactly
/// the block copy the vector describes. (On screen the picture moves by
/// -v; MPEG has always had it this way round.)
Frame texturedFrame( int width, int height, uint32_t seed, int vx, int vy, const Rect* flat = nullptr )
{
	Frame f( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const int tx = x + vx;
			const int ty = y + vy;
			uint8_t* o   = f.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			if( flat && flat->contains( tx, ty ) )
			{
				o[ 0 ] = 140;
				o[ 1 ] = 90;
				o[ 2 ] = 60;
			}
			else
			{
				o[ 0 ] = static_cast< uint8_t >( std::lround( 255.0 * texture( tx, ty, seed ) ) );
				o[ 1 ] = static_cast< uint8_t >( std::lround( 255.0 * texture( tx, ty, seed + 7u ) ) );
				o[ 2 ] = static_cast< uint8_t >( std::lround( 255.0 * texture( tx, ty, seed + 13u ) ) );
			}
			o[ 3 ] = 255;
		}
	}
	return f;
}

Frame flatFrame( int width, int height, int r, int g, int b )
{
	Frame f( static_cast< size_t >( width ) * height * 4 );
	for( size_t i = 0; i < f.size(); i += 4 )
	{
		f[ i + 0 ] = static_cast< uint8_t >( r );
		f[ i + 1 ] = static_cast< uint8_t >( g );
		f[ i + 2 ] = static_cast< uint8_t >( b );
		f[ i + 3 ] = 255;
	}
	return f;
}

//---------------------------------------------------------------------------
// The demo card, for --out and for the sweep.
//
// Two SCENES that alternate every 24 frames -- a hard cut is the whole
// subject, so the card has one -- each a textured background drifting at a
// known speed with a coloured shape moving across it the other way, a flat
// patch (so there are blocks with nothing to match) and a colour-bar strip
// at the bottom. It moves, because a still picture codes to itself and every
// control reads as dead.
//---------------------------------------------------------------------------
Frame demoCard( int width, int height, int frame )
{
	const int scene   = ( frame / 24 ) % 2;
	const int t       = frame;
	const uint32_t sd = scene == 0 ? 1u : 2u;

	const int driftX = scene == 0 ? 1 * t : -2 * t;
	const int driftY = scene == 0 ? 0 : 1 * ( t / 2 );

	Rect flat;
	flat.x0 = static_cast< int >( width * 0.62 );
	flat.x1 = static_cast< int >( width * 0.92 );
	flat.y0 = static_cast< int >( height * 0.45 );
	flat.y1 = static_cast< int >( height * 0.85 );

	Frame f = texturedFrame( width, height, sd, driftX, driftY, &flat );

	const double w = width, h = height;
	//The shape: a disc in scene 0, a square in scene 1, each moving the other
	//way from the background at 3 px/frame, wrapped.
	const double shapeX = std::fmod( 0.15 * w + ( scene == 0 ? 3.0 : -3.0 ) * t + 10.0 * w, w );
	const double shapeY = 0.60 * h;
	const double radius = std::min( 0.12 * h, 0.08 * w );

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			uint8_t* o     = f.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			const double v = ( y + 0.5 ) / h;
			const double u = ( x + 0.5 ) / w;

			//Six saturated bars across the bottom tenth.
			if( v < 0.10 )
			{
				static const int bars[ 6 ][ 3 ] = {
					{ 255, 25, 25 }, { 25, 255, 25 }, { 25, 25, 255 },
					{ 25, 255, 255 }, { 255, 25, 255 }, { 255, 255, 25 }
				};
				const int bar = std::min( 5, static_cast< int >( u * 6.0 ) );
				o[ 0 ]        = static_cast< uint8_t >( bars[ bar ][ 0 ] );
				o[ 1 ]        = static_cast< uint8_t >( bars[ bar ][ 1 ] );
				o[ 2 ]        = static_cast< uint8_t >( bars[ bar ][ 2 ] );
			}

			const double dx = x + 0.5 - shapeX;
			const double dy = y + 0.5 - shapeY;
			const bool inside = scene == 0 ? ( dx * dx + dy * dy < radius * radius )
			                               : ( std::fabs( dx ) < radius && std::fabs( dy ) < radius );
			if( inside )
			{
				o[ 0 ] = static_cast< uint8_t >( scene == 0 ? 255 : 40 );
				o[ 1 ] = static_cast< uint8_t >( scene == 0 ? 210 : 220 );
				o[ 2 ] = static_cast< uint8_t >( scene == 0 ? 60 : 255 );
			}
		}
	}
	return f;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	//Accelerated first; fall back so the harness still runs somewhere without
	//a GPU. Nothing here asserts on a filtered fetch, so it should pass there
	//too.
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, const unsigned char* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

Frame flipRows( const Frame& image, int width, int height )
{
	Frame flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::memcpy( flipped.data() + static_cast< size_t >( y ) * stride,
		             image.data() + static_cast< size_t >( height - 1 - y ) * stride, stride );
	return flipped;
}

//---------------------------------------------------------------------------
// The rig: the real plugin class, a texture to feed it, a texture to render
// into. Nothing here re-implements any part of the codec.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index = 0;
	float value        = 0.0f;
};

std::vector< NamedParameter > listParameters( Residual& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Residual::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ) } );
	}
	return list;
}

struct Rig
{
	Residual plugin;
	int width  = 0;
	int height = 0;
	double fps = 60.0;

	/// Seconds added to every SetTime, and the frame the clock jumps back to
	/// zero at (a clip retrigger), for --onset.
	double clockOrigin = 0.0;
	int clockJumpAt    = -1;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;

	int srcWidth  = 0;
	int srcHeight = 0;

	FFGLTextureStruct inputStruct {};
	FFGLTextureStruct* inputs[ 1 ] {};
	ProcessOpenGLStruct process {};
	bool started = false;

	/// The spectrum handed over before each frame, as the host would hand
	/// it: one value per element of the Audio buffer. All zero unless a check
	/// sets it.
	std::vector< float > spectrum = std::vector< float >( audio::kBins, 0.0f );

	bool begin( int w, int h )
	{
		width  = w;
		height = h;

		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}

		srcWidth  = w;
		srcHeight = h;

		const Frame empty = flatFrame( w, h, 0, 0, 0 );
		sourceTexture     = makeTexture( w, h, empty.data() );
		outputTexture     = makeTexture( w, h, nullptr );
		outputFBO         = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( w );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( h );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;

		started = true;
		return true;
	}

	/// Hand the plugin a DIFFERENTLY SIZED input from here on, and a
	/// differently sized output too: the composition's resolution changed.
	void resize( int w, int h )
	{
		glDeleteTextures( 1, &sourceTexture );
		glDeleteFramebuffers( 1, &outputFBO );
		glDeleteTextures( 1, &outputTexture );

		const Frame empty = flatFrame( w, h, 0, 0, 0 );
		sourceTexture     = makeTexture( w, h, empty.data() );
		outputTexture     = makeTexture( w, h, nullptr );
		outputFBO         = makeFramebuffer( outputTexture );
		width = srcWidth = w;
		height = srcHeight = h;

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( w );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( h );
		inputStruct.Handle                              = sourceTexture;
		process.HostFBO                                 = outputFBO;
	}

	bool set( const std::string& name, float value )
	{
		for( const NamedParameter& p : listParameters( plugin ) )
		{
			if( p.name != name )
				continue;
			plugin.SetFloatParameter( p.index, value );
			return true;
		}
		std::fprintf( stderr, "no parameter called '%s'\n", name.c_str() );
		return false;
	}

	/// One frame with this source.
	bool frame( int index, const Frame& source )
	{
		double seconds = clockOrigin + static_cast< double >( index ) / fps;
		if( clockJumpAt >= 0 && index >= clockJumpAt )
			seconds = static_cast< double >( index - clockJumpAt ) / fps;
		plugin.SetTime( seconds );

		//The spectrum, written the way the host writes one.
		for( int bin = 0; bin < audio::kBins; ++bin )
			plugin.SetParamElementValue( Residual::PT_AUDIO, static_cast< unsigned int >( bin ), spectrum[ bin ] );

		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, srcWidth, srcHeight, GL_RGBA, GL_UNSIGNED_BYTE,
		                 source.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		return plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
	}

	/// The host-side output, RGBA8, row 0 at the bottom.
	Frame readOutput() const
	{
		Frame pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return pixels;
	}

	/// The DECODED frame, straight from the plugin's integer texture.
	Frame readDecoded() const
	{
		Frame out;
		int w = 0, h = 0;
		plugin.ReadDecodedForTest( out, w, h );
		return out;
	}

	std::vector< int32_t > readVectors( int& bx, int& by ) const
	{
		std::vector< int32_t > out;
		plugin.ReadVectorsForTest( out, bx, by );
		return out;
	}

	~Rig()
	{
		if( !started )
			return;
		plugin.DeInitGL();
		glDeleteFramebuffers( 1, &outputFBO );
		glDeleteTextures( 1, &outputTexture );
		glDeleteTextures( 1, &sourceTexture );
	}
};

//---------------------------------------------------------------------------
// Reporting. Every check files ONE headline number, printed together at the
// end: a run that only says "all checks passed" says nothing about margin.
//---------------------------------------------------------------------------
int g_failures = 0;

struct Headline
{
	std::string check;
	std::string what;
	std::string value;
};
std::vector< Headline > g_headlines;

void headline( const char* check, const char* what, const std::string& value )
{
	g_headlines.push_back( Headline { check, what, value } );
}

void printSummary()
{
	if( g_headlines.empty() )
		return;

	std::printf( "\n== summary: the headline number from every check\n" );
	for( const Headline& h : g_headlines )
		std::printf( "   %-10s %-46s %s\n", h.check.c_str(), h.what.c_str(), h.value.c_str() );
}

void ok( const std::string& what )
{
	std::printf( "   ok    %s\n", what.c_str() );
}

void bad( const std::string& what )
{
	std::printf( "   FAIL  %s\n", what.c_str() );
	++g_failures;
}

void check( bool condition, const std::string& what )
{
	if( condition )
		ok( what );
	else
		bad( what );
}

/// RGB only: alpha is not coded and carries the source's through.
long long rgbMismatches( const Frame& a, const Frame& b, int& worst )
{
	long long count = 0;
	worst           = 0;
	const size_t n  = std::min( a.size(), b.size() );
	for( size_t i = 0; i < n; ++i )
	{
		if( ( i & 3 ) == 3 )
			continue;
		const int d = std::abs( static_cast< int >( a[ i ] ) - static_cast< int >( b[ i ] ) );
		if( d > 0 )
			++count;
		worst = std::max( worst, d );
	}
	return count;
}

double meanAbsError( const Frame& a, const Frame& b )
{
	double sum = 0.0;
	long long n = 0;
	for( size_t i = 0; i < a.size(); ++i )
	{
		if( ( i & 3 ) == 3 )
			continue;
		sum += std::abs( static_cast< int >( a[ i ] ) - static_cast< int >( b[ i ] ) );
		++n;
	}
	return n > 0 ? sum / static_cast< double >( n ) : 0.0;
}

//---------------------------------------------------------------------------
// --transform. No GL. The colour space is reversible for every triple, the
// DCT basis is orthonormal, and the float round trip stays under half a code.
//---------------------------------------------------------------------------
int runTransform()
{
	std::printf( "== transform: the arithmetic that has to be exact\n" );

	//YCoCg-R over ALL 16,777,216 triples. Not a sample: the claim is "every".
	{
		long long wrong = 0;
		int yMin = 1 << 20, yMax = -( 1 << 20 ), cMin = 1 << 20, cMax = -( 1 << 20 );
		for( int r = 0; r < 256; ++r )
			for( int g = 0; g < 256; ++g )
				for( int b = 0; b < 256; ++b )
				{
					const codec::YCoCg c = codec::toYCoCg( r, g, b );
					int rr, gg, bb;
					codec::toRGB( c, rr, gg, bb );
					if( rr != r || gg != g || bb != b )
						++wrong;
					yMin = std::min( yMin, c.y );
					yMax = std::max( yMax, c.y );
					cMin = std::min( { cMin, c.co, c.cg } );
					cMax = std::max( { cMax, c.co, c.cg } );
				}
		std::printf( "   YCoCg-R: %lld of 16777216 triples fail to round-trip; Y in %d..%d, Co/Cg in %d..%d\n",
		             wrong, yMin, yMax, cMin, cMax );
		check( wrong == 0, "YCoCg-R is reversible for every 8-bit RGB triple" );
		check( yMin == 0 && yMax == 255 && cMin == -255 && cMax == 255,
		       "Y spans 0..255 and Co/Cg span -255..255, the ranges the residual bound assumes" );
		headline( "transform", "RGB triples that fail the YCoCg-R round trip",
		          std::to_string( wrong ) + " of 16777216" );
	}

	//The basis is orthonormal: C * C^T == I to a few ulps of float.
	{
		float basis[ 64 ];
		codec::dctBasis( basis );
		double worst = 0.0;
		for( int u = 0; u < 8; ++u )
			for( int v = 0; v < 8; ++v )
			{
				double dot = 0.0;
				for( int x = 0; x < 8; ++x )
					dot += static_cast< double >( basis[ u * 8 + x ] ) * basis[ v * 8 + x ];
				worst = std::max( worst, std::fabs( dot - ( u == v ? 1.0 : 0.0 ) ) );
			}
		std::printf( "   DCT basis: C * C^T departs from the identity by at most %.3g\n", worst );
		check( worst < 1e-6, "the DCT basis is orthonormal (float entries, so 1e-6)" );
	}

	//The round trip, in the same float arithmetic the shader does, on random
	//8-bit residual blocks: exact after rounding, and the worst pre-rounding
	//error sits inside the derived bound.
	{
		float basis[ 64 ];
		codec::dctBasis( basis );
		const double bound = codec::dctRoundTripBound();

		double worstError = 0.0;
		long long wrong   = 0;
		uint32_t seed     = 1u;
		for( int block = 0; block < 2000; ++block )
		{
			int in[ 8 ][ 8 ];
			for( int j = 0; j < 8; ++j )
				for( int i = 0; i < 8; ++i )
				{
					seed          = hashInt( seed + 1u );
					in[ j ][ i ]  = static_cast< int >( seed % 1021u ) - 510;
				}

			float rows[ 8 ][ 8 ], cols[ 8 ][ 8 ], back[ 8 ][ 8 ];
			for( int y = 0; y < 8; ++y )
				for( int u = 0; u < 8; ++u )
				{
					float s = 0.0f;
					for( int i = 0; i < 8; ++i )
						s += basis[ u * 8 + i ] * static_cast< float >( in[ y ][ i ] );
					rows[ y ][ u ] = s;
				}
			for( int u = 0; u < 8; ++u )
				for( int v = 0; v < 8; ++v )
				{
					float s = 0.0f;
					for( int j = 0; j < 8; ++j )
						s += basis[ v * 8 + j ] * rows[ j ][ u ];
					cols[ v ][ u ] = s;
				}
			for( int u = 0; u < 8; ++u )
				for( int j = 0; j < 8; ++j )
				{
					float s = 0.0f;
					for( int v = 0; v < 8; ++v )
						s += basis[ v * 8 + j ] * cols[ v ][ u ];
					back[ j ][ u ] = s;
				}
			for( int j = 0; j < 8; ++j )
				for( int i = 0; i < 8; ++i )
				{
					float s = 0.0f;
					for( int u = 0; u < 8; ++u )
						s += basis[ u * 8 + i ] * back[ j ][ u ];
					worstError = std::max( worstError, std::fabs( static_cast< double >( s ) - in[ j ][ i ] ) );
					if( static_cast< int >( std::floor( s + 0.5f ) ) != in[ j ][ i ] )
						++wrong;
				}
		}
		std::printf( "   DCT round trip on 2000 random residual blocks: worst error %.4f codes,"
		             " derived bound %.4f, %lld samples wrong after rounding\n",
		             worstError, bound, wrong );
		check( bound < 0.5, "the derived round-trip bound is under half a code" );
		check( worstError <= bound, "the observed round-trip error sits inside the derived bound" );
		check( wrong == 0, "every sample rounds back to itself" );
		headline( "transform", "DCT round-trip error, observed / bound",
		          std::to_string( worstError ).substr( 0, 6 ) + " / " + std::to_string( bound ).substr( 0, 6 )
		              + " codes" );
	}

	return 0;
}

//---------------------------------------------------------------------------
// --vectors. A textured frame translated by (dx, dy): every interior block
// returns exactly (dx, dy). Flat blocks return (0, 0).
//---------------------------------------------------------------------------
struct VectorCase
{
	int width, height;
	int blockOption;
	int range;
	bool halfPel;
};

long long g_vectorWrong    = 0;
long long g_vectorChecked  = 0;
long long g_vectorExcluded = 0;
int g_vectorCases          = 0;

/// Run two frames -- the texture, then the texture moved by (dx, dy) -- and
/// return the level-0 vectors of the second.
bool vectorsFor( const VectorCase& c, int dx, int dy, const Rect* flat, std::vector< int32_t >& vectors,
                 int& bx, int& by, bool tieBreak = true )
{
	Rig rig;
	if( !rig.begin( c.width, c.height ) )
		return false;

	rig.set( "Block Size", static_cast< float >( c.blockOption ) );
	rig.set( "Search Range", static_cast< float >( c.range ) );
	rig.set( "Half Pel", c.halfPel ? 1.0f : 0.0f );
	rig.set( "Scene Cut", 0.0f );
	rig.plugin.SetTieBreakForTest( tieBreak );

	if( !rig.frame( 0, texturedFrame( c.width, c.height, 5u, 0, 0, flat ) ) )
		return false;
	if( !rig.frame( 1, texturedFrame( c.width, c.height, 5u, dx, dy, flat ) ) )
		return false;

	vectors = rig.readVectors( bx, by );
	return true;
}

bool vectorCase( const VectorCase& c, int dx, int dy, bool expectFound )
{
	const int B = codec::kBlockSizes[ c.blockOption ];
	const int R = c.range;

	//The flat patch, in texture coordinates: the right half, less a border.
	//Large, because a flat block has to clear the margin below on every
	//side in both frames, and at 320x180 with 32-pixel blocks that is tight.
	Rect flat;
	flat.x0 = static_cast< int >( c.width * 0.50 );
	flat.x1 = static_cast< int >( c.width * 0.98 );
	flat.y0 = static_cast< int >( c.height * 0.08 );
	flat.y1 = static_cast< int >( c.height * 0.92 );

	std::vector< int32_t > vectors;
	int bx = 0, by = 0;
	if( !vectorsFor( c, dx, dy, &flat, vectors, bx, by ) )
		return false;

	//Which blocks the claim applies to. A TEXTURED interior block has its
	//whole reference region -- the block moved by (dx, dy), where it came
	//from -- inside the previous frame, and none of its search neighbourhood
	//touches the flat patch in either frame. A FLAT block has its whole
	//search neighbourhood inside the patch in both frames. Everything else is
	//excluded and counted.
	//
	//The neighbourhood is the block grown by R + 8 + B/2: the coarsest
	//level's range rounds up to a multiple of its cell (at most 8 pixels for
	//a 32 block); a coarse cell straddling the patch's edge averages flat
	//with texture; and the coarse levels match on a window half a block
	//wider than the block on every side.
	const int margin = R + 8 + B / 2;

	long long textured = 0, texturedWrong = 0, flatCount = 0, flatWrong = 0, excluded = 0;
	int firstWrongX = -1, firstWrongY = -1, firstWrongVx = 0, firstWrongVy = 0;
	long long found = 0;

	for( int j = 0; j < by; ++j )
	{
		for( int i = 0; i < bx; ++i )
		{
			const int32_t* v = vectors.data() + ( static_cast< size_t >( j ) * bx + i ) * 4;
			const int x0 = i * B, y0 = j * B, x1 = x0 + B, y1 = y0 + B;

			if( v[ 0 ] == 2 * dx && v[ 1 ] == 2 * dy )
				++found;

			//Whole block inside the frame?
			const bool insideFrame = x1 <= c.width && y1 <= c.height;
			//Reference region inside the previous frame?
			const bool refInside = x0 + dx >= 0 && y0 + dy >= 0 && x1 + dx <= c.width && y1 + dy <= c.height;

			//Neighbourhood, in the current frame and in the previous frame's
			//coordinates (which are the texture's).
			const Rect hoodCur { x0 - margin, y0 - margin, x1 + margin, y1 + margin };
			const Rect hoodTex { x0 - margin + dx, y0 - margin + dy, x1 + margin + dx, y1 + margin + dy };
			auto inside = []( const Rect& a, const Rect& b ) {
				return a.x0 >= b.x0 && a.y0 >= b.y0 && a.x1 <= b.x1 && a.y1 <= b.y1;
			};
			auto disjoint = []( const Rect& a, const Rect& b ) {
				return a.x1 <= b.x0 || b.x1 <= a.x0 || a.y1 <= b.y0 || b.y1 <= a.y0;
			};
			//The patch as it appears in the CURRENT frame: texture coordinate
			//t is on screen at t - v.
			const Rect flatCur { flat.x0 - dx, flat.y0 - dy, flat.x1 - dx, flat.y1 - dy };

			const bool isFlat = inside( hoodCur, flatCur ) && inside( hoodTex, flat );
			const bool isTextured = insideFrame && refInside && disjoint( hoodCur, flatCur ) && disjoint( hoodTex, flat );

			if( isFlat )
			{
				++flatCount;
				if( v[ 0 ] != 0 || v[ 1 ] != 0 )
					++flatWrong;
			}
			else if( isTextured && expectFound )
			{
				++textured;
				if( v[ 0 ] != 2 * dx || v[ 1 ] != 2 * dy )
				{
					if( texturedWrong == 0 )
					{
						firstWrongX  = i;
						firstWrongY  = j;
						firstWrongVx = v[ 0 ];
						firstWrongVy = v[ 1 ];
					}
					++texturedWrong;
				}
			}
			else
				++excluded;
		}
	}

	char label[ 160 ];
	std::snprintf( label, sizeof( label ), "%dx%d B=%-2d R=%-2d%s (%+3d,%+3d)", c.width, c.height, B, R,
	               c.halfPel ? " hp" : "   ", dx, dy );

	if( expectFound )
	{
		std::printf( "   %s textured %3lld blocks, %lld wrong; flat %2lld blocks, %lld wrong; %lld excluded (edge or near the patch)",
		             label, textured, texturedWrong, flatCount, flatWrong, excluded );
		if( texturedWrong > 0 )
			std::printf( "  first wrong at block (%d,%d): (%d,%d) half-pel", firstWrongX, firstWrongY, firstWrongVx,
			             firstWrongVy );
		std::printf( "\n" );
		check( textured >= 4, std::string( label ) + " has enough interior textured blocks to mean anything" );
		check( texturedWrong == 0, std::string( label ) + " every interior textured block is exactly (dx, dy)" );
		check( flatCount >= 1, std::string( label ) + " has at least one flat block" );
		check( flatWrong == 0, std::string( label ) + " every flat block is (0, 0)" );
		g_vectorChecked += textured + flatCount;
		g_vectorWrong += texturedWrong + flatWrong;
		g_vectorExcluded += excluded;
	}
	else
	{
		std::printf( "   %s past the range: %lld of %lld blocks returned it\n", label, found,
		             static_cast< long long >( bx ) * by );
		check( found == 0, std::string( label ) + " a shift past the search range is not found" );
	}
	++g_vectorCases;
	return true;
}

int runVectors()
{
	std::printf( "== vectors: a translated texture returns exactly (dx, dy)\n" );

	const VectorCase cases[] = {
		{ 320, 180, kBlock8, 8, false },
		{ 320, 180, kBlock16, 16, false },
		//B=32 at R=8 here: at this raster a 32 block plus the R+8+16 margin
		//on every side only fits the patch at a short range. R=16 and R=32
		//with 32 blocks are the 640x360 cases below.
		{ 320, 180, kBlock32, 8, false },
		{ 320, 180, kBlock16, 4, false },
		{ 320, 180, kBlock16, 16, true },
		{ 640, 360, kBlock8, 8, false },
		{ 640, 360, kBlock16, 16, false },
		{ 640, 360, kBlock32, 32, false },
	};

	for( const VectorCase& c : cases )
	{
		const int R = c.range;
		//Odd and even, small and at the edge of the range, in every
		//direction. (R, 0) and (-R, R) sit exactly on the range limit.
		const int shifts[][ 2 ] = { { 0, 0 }, { 1, 1 }, { 3, -2 }, { -5, 7 }, { R, 0 }, { -R, R }, { 0, -R } };
		for( const auto& s : shifts )
		{
			if( std::abs( s[ 0 ] ) > R || std::abs( s[ 1 ] ) > R )
				continue;
			if( !vectorCase( c, s[ 0 ], s[ 1 ], true ) )
				return 1;
		}
		//One past the range, which must NOT be found.
		if( !vectorCase( c, R + 1, 0, false ) )
			return 1;
	}

	//A wholly flat frame: every block is flat, every vector must be zero.
	{
		const VectorCase c = { 320, 180, kBlock16, 16, false };
		Rig rig;
		if( !rig.begin( c.width, c.height ) )
			return 1;
		rig.set( "Block Size", static_cast< float >( c.blockOption ) );
		rig.set( "Search Range", static_cast< float >( c.range ) );
		rig.set( "Scene Cut", 0.0f );
		const Frame flat = flatFrame( c.width, c.height, 120, 120, 120 );
		if( !rig.frame( 0, flat ) || !rig.frame( 1, flat ) )
			return 1;
		int bx = 0, by = 0;
		const std::vector< int32_t > v = rig.readVectors( bx, by );
		long long nonzero = 0;
		for( int k = 0; k < bx * by; ++k )
			if( v[ k * 4 ] != 0 || v[ k * 4 + 1 ] != 0 )
				++nonzero;
		std::printf( "   320x180 wholly flat: %lld of %d blocks non-zero\n", nonzero, bx * by );
		check( nonzero == 0, "on a wholly flat frame every block returns (0, 0)" );
		g_vectorChecked += bx * by;
		g_vectorWrong += nonzero;
	}

	headline( "vectors", "blocks with the wrong vector",
	          std::to_string( g_vectorWrong ) + " of " + std::to_string( g_vectorChecked ) + " judged, "
	              + std::to_string( g_vectorExcluded ) + " excluded (edge or patch), " + std::to_string( g_vectorCases )
	              + " cases at 2 rasters" );
	return 0;
}

//---------------------------------------------------------------------------
// --lossless. Q 0, gain 1: decoded == source, every frame, bitwise.
//---------------------------------------------------------------------------
long long g_losslessWrong = 0;
long long g_losslessCompared = 0;

bool losslessCase( int width, int height, int blockOption, bool halfPel, const char* label )
{
	Rig rig;
	if( !rig.begin( width, height ) )
		return false;

	rig.set( "Block Size", static_cast< float >( blockOption ) );
	rig.set( "Half Pel", halfPel ? 1.0f : 0.0f );
	rig.set( "Q", 0.0f );
	rig.set( "Chroma Q", 0.0f );
	rig.set( "Residual Gain", 0.5f );//1.0
	rig.set( "GOP", 250.0f );
	rig.set( "Scene Cut", 0.0f );

	long long wrong = 0, compared = 0;
	int worst       = 0;
	int intraFrames = 0;
	for( int k = 0; k < 8; ++k )
	{
		const Frame source = demoCard( width, height, k );
		if( !rig.frame( k, source ) )
			return false;
		intraFrames += rig.plugin.LastFrameIntraForTest() ? 1 : 0;
		int w = 0;
		wrong += rgbMismatches( rig.readDecoded(), source, w );
		worst = std::max( worst, w );
		compared += static_cast< long long >( width ) * height * 3;
	}

	std::printf( "   %s %lld samples over 8 frames (%d intra, %d predicted), %lld differ, worst %d\n", label,
	             compared, intraFrames, 8 - intraFrames, wrong, worst );
	check( intraFrames == 1, std::string( label ) + " codes seven P-frames after the first I-frame" );
	check( wrong == 0, std::string( label ) + " is lossless, bitwise" );
	g_losslessWrong += wrong;
	g_losslessCompared += compared;
	return true;
}

int runLossless()
{
	std::printf( "== lossless: Q 0 and gain 1 give the source back, bitwise\n" );

	const struct
	{
		int w, h, block;
		bool hp;
		const char* label;
	} cases[] = {
		{ 320, 180, kBlock8, false, "320x180  B=8       " },
		{ 320, 180, kBlock16, false, "320x180  B=16      " },
		{ 320, 180, kBlock32, false, "320x180  B=32      " },
		{ 320, 180, kBlock16, true, "320x180  B=16 hp   " },
		{ 1280, 720, kBlock16, false, "1280x720 B=16      " },
		{ 1280, 720, kBlock8, true, "1280x720 B=8  hp   " },
	};
	for( const auto& c : cases )
		if( !losslessCase( c.w, c.h, c.block, c.hp, c.label ) )
			return 1;

	headline( "lossless", "samples differing from the source at Q 0",
	          std::to_string( g_losslessWrong ) + " of " + std::to_string( g_losslessCompared ) + ", at 2 rasters" );
	return 0;
}

//---------------------------------------------------------------------------
// --mosh. Drop I, gain 0, over a hard cut: the decoded frame is the previous
// decoded frame block-copied by the vectors, bitwise. The headline claim.
//---------------------------------------------------------------------------
long long g_moshWrong = 0;
long long g_moshCompared = 0;
int g_moshCases = 0;

bool moshCase( int width, int height, int blockOption, const char* label, bool perturbExpectation = false,
               long long* mismatchesOut = nullptr )
{
	Rig rig;
	if( !rig.begin( width, height ) )
		return false;

	const int B = codec::kBlockSizes[ blockOption ];
	rig.set( "Block Size", static_cast< float >( blockOption ) );
	rig.set( "Half Pel", 0.0f );
	rig.set( "Drop I", static_cast< float >( kDropAll ) );
	rig.set( "Residual Gain", 0.0f );
	rig.set( "Vector Scale", 0.25f );//1.0
	rig.set( "GOP", 250.0f );
	//A cut between two low-contrast synthetic textures leaves 11..17 codes
	//per pixel after motion compensation here, against 2..64 on the control;
	//said outright rather than left to the default.
	rig.set( "Scene Threshold", 0.3f );//5.7 codes per pixel

	//Scene A for six frames, drifting; then scene B, unrelated.
	const int cut = 6;
	Frame before;
	for( int k = 0; k < cut; ++k )
	{
		if( !rig.frame( k, texturedFrame( width, height, 11u, k, 0 ) ) )
			return false;
		if( k == cut - 1 )
			before = rig.readDecoded();
	}
	if( !rig.frame( cut, texturedFrame( width, height, 12u, 0, 0 ) ) )
		return false;

	const bool sceneCut = rig.plugin.LastSceneCutForTest();
	const bool dropped  = rig.plugin.LastFrameDroppedForTest();
	const bool intra    = rig.plugin.LastFrameIntraForTest();

	int bx = 0, by = 0;
	std::vector< int32_t > vectors = rig.readVectors( bx, by );
	const Frame after              = rig.readDecoded();

	if( perturbExpectation )
	{
		//An off-by-one in the vectors, in half-pel units of two: what a
		//wrong block copy would look like from outside.
		for( size_t k = 0; k < vectors.size(); k += 4 )
			vectors[ k ] += 2;
	}

	const Frame expected = codec::blockCopy( before, width, height, vectors, bx, B );

	int worst            = 0;
	const long long wrong = rgbMismatches( after, expected, worst );
	const long long compared = static_cast< long long >( width ) * height * 3;

	//The claim must not be vacuous: a black reference block-copies to black
	//and agrees with anything. The first version of the plugin scaled
	//I-frames by Residual Gain, so at gain 0 the whole run was black and this
	//check passed on nothing; the negative control below is what caught it.
	long long lit = 0;
	for( size_t i = 0; i < before.size(); i += 4 )
		lit += before[ i ] > 32 ? 1 : 0;
	const bool textured = lit > static_cast< long long >( width ) * height / 4;

	//And a plain P-frame two frames earlier, where the vectors are the real
	//drift, is the same arithmetic and must hold too.
	std::printf( "   %s cut detected=%s dropped=%s intra=%s; meanSAD %.1f; %lld of %lld samples differ from the block copy, worst %d\n",
	             label, sceneCut ? "yes" : "no", dropped ? "yes" : "no", intra ? "yes" : "no",
	             rig.plugin.LastMeanSadForTest(), wrong, compared, worst );

	if( mismatchesOut )
	{
		*mismatchesOut = wrong;
		return true;
	}

	check( textured, std::string( label ) + " the reference is a real picture, so the copy is not of nothing" );
	check( sceneCut, std::string( label ) + " the scene-cut detector saw the cut" );
	check( dropped && !intra, std::string( label ) + " the I-frame it wanted was dropped" );
	check( wrong == 0, std::string( label ) + " decoded == previous decoded block-copied by the vectors, bitwise" );
	g_moshWrong += wrong;
	g_moshCompared += compared;
	++g_moshCases;
	return true;
}

/// The same claim on an ordinary P-frame with real motion, no cut.
bool moshPlainCase( int width, int height, int blockOption, const char* label )
{
	Rig rig;
	if( !rig.begin( width, height ) )
		return false;

	const int B = codec::kBlockSizes[ blockOption ];
	rig.set( "Block Size", static_cast< float >( blockOption ) );
	rig.set( "Half Pel", 0.0f );
	rig.set( "Drop I", static_cast< float >( kDropAll ) );
	rig.set( "Residual Gain", 0.0f );
	rig.set( "GOP", 250.0f );

	Frame before;
	for( int k = 0; k < 4; ++k )
	{
		if( !rig.frame( k, demoCard( width, height, k ) ) )
			return false;
		if( k == 2 )
			before = rig.readDecoded();
	}
	int bx = 0, by = 0;
	const std::vector< int32_t > vectors = rig.readVectors( bx, by );
	const Frame expected                 = codec::blockCopy( before, width, height, vectors, bx, B );
	int worst                            = 0;
	const long long wrong                = rgbMismatches( rig.readDecoded(), expected, worst );
	const long long compared             = static_cast< long long >( width ) * height * 3;

	std::printf( "   %s ordinary P-frame: %lld of %lld samples differ from the block copy, worst %d\n", label,
	             wrong, compared, worst );
	check( wrong == 0, std::string( label ) + " an ordinary P-frame at gain 0 is the block copy, bitwise" );
	g_moshWrong += wrong;
	g_moshCompared += compared;
	++g_moshCases;
	return true;
}

int runMosh()
{
	std::printf( "== mosh: no I-frame, no residual, over a hard cut\n" );

	if( !moshCase( 320, 180, kBlock16, "320x180 B=16" ) )
		return 1;
	if( !moshCase( 320, 180, kBlock8, "320x180 B=8 " ) )
		return 1;
	if( !moshCase( 640, 360, kBlock16, "640x360 B=16" ) )
		return 1;
	if( !moshCase( 640, 360, kBlock32, "640x360 B=32" ) )
		return 1;
	if( !moshPlainCase( 320, 180, kBlock16, "320x180 B=16" ) )
		return 1;
	if( !moshPlainCase( 640, 360, kBlock8, "640x360 B=8 " ) )
		return 1;

	headline( "mosh", "samples differing from the block copy",
	          std::to_string( g_moshWrong ) + " of " + std::to_string( g_moshCompared ) + ", "
	              + std::to_string( g_moshCases ) + " cases at 2 rasters" );
	return 0;
}

//---------------------------------------------------------------------------
// --gop. I-frames land exactly every GOP-th frame; an I-frame is a function
// of its source alone; at Q 0 it IS its source; the scene-cut detector
// inserts one at a cut and not on motion; Refresh forces one; Drop I drops
// them.
//---------------------------------------------------------------------------
int g_gopWrong = 0;

/// Render `frames` frames of a drifting texture and return the frame indices
/// that were intra.
bool intraPattern( int width, int height, int gop, int drop, bool sceneCut, int frames,
                   std::vector< int >& intraAt, std::vector< int >& droppedAt, int refreshAt = -1,
                   int cutAt = -1 )
{
	Rig rig;
	if( !rig.begin( width, height ) )
		return false;

	rig.set( "GOP", static_cast< float >( gop ) );
	rig.set( "Drop I", static_cast< float >( drop ) );
	rig.set( "Scene Cut", sceneCut ? 1.0f : 0.0f );
	rig.set( "Scene Threshold", 0.3f );//5.7 codes per pixel, see moshCase

	for( int k = 0; k < frames; ++k )
	{
		if( k == refreshAt )
			rig.set( "Refresh", 1.0f );
		const Frame source = k >= cutAt && cutAt >= 0 ? texturedFrame( width, height, 21u, -k, k / 3 )
		                                              : texturedFrame( width, height, 20u, 2 * k, k );
		if( !rig.frame( k, source ) )
			return false;
		if( rig.plugin.LastFrameIntraForTest() )
			intraAt.push_back( k );
		if( rig.plugin.LastFrameDroppedForTest() )
			droppedAt.push_back( k );
	}
	return true;
}

std::string joined( const std::vector< int >& v )
{
	std::string s;
	for( size_t i = 0; i < v.size(); ++i )
		s += ( i ? "," : "" ) + std::to_string( v[ i ] );
	return s.empty() ? "-" : s;
}

int runGop()
{
	std::printf( "== gop: where the I-frames land, and what they are\n" );

	//The cadence.
	for( int gop : { 1, 6, 12 } )
	{
		std::vector< int > intraAt, droppedAt;
		if( !intraPattern( 320, 180, gop, kDropOff, false, 25, intraAt, droppedAt ) )
			return 1;
		std::vector< int > want;
		for( int k = 0; k < 25; k += gop )
			want.push_back( k );
		std::printf( "   GOP %-2d I-frames at %s\n", gop, joined( intraAt ).c_str() );
		check( intraAt == want, "GOP " + std::to_string( gop ) + ": I-frames land exactly every GOP-th frame" );
		g_gopWrong += intraAt == want ? 0 : 1;
	}

	//Drop I: Next drops one, All drops all, and a dropped I-frame still ends
	//the GOP.
	{
		std::vector< int > intraAt, droppedAt;
		if( !intraPattern( 320, 180, 4, kDropNext, false, 13, intraAt, droppedAt ) )
			return 1;
		std::printf( "   Drop I = Next, GOP 4: I-frames at %s, dropped at %s\n", joined( intraAt ).c_str(),
		             joined( droppedAt ).c_str() );
		check( intraAt == std::vector< int > { 0, 8, 12 } && droppedAt == std::vector< int > { 4 },
		       "Drop I = Next drops exactly the next I-frame and the GOP carries on from it" );

		intraAt.clear();
		droppedAt.clear();
		if( !intraPattern( 320, 180, 4, kDropAll, false, 13, intraAt, droppedAt ) )
			return 1;
		std::printf( "   Drop I = All,  GOP 4: I-frames at %s, dropped at %s\n", joined( intraAt ).c_str(),
		             joined( droppedAt ).c_str() );
		check( intraAt == std::vector< int > { 0 } && droppedAt == std::vector< int > { 4, 8, 12 },
		       "Drop I = All drops every I-frame but the first, which has nothing to predict from" );

		//Refresh under Drop I = All: the operator's button beats the latch.
		intraAt.clear();
		droppedAt.clear();
		if( !intraPattern( 320, 180, 250, kDropAll, false, 16, intraAt, droppedAt, 9 ) )
			return 1;
		std::printf( "   Drop I = All, Refresh at 9: I-frames at %s\n", joined( intraAt ).c_str() );
		check( intraAt == std::vector< int > { 0, 9 }, "Refresh forces an I-frame even under Drop I = All" );
	}

	//The scene-cut detector: one I-frame at the cut, none on plain motion.
	{
		std::vector< int > intraAt, droppedAt;
		if( !intraPattern( 320, 180, 250, kDropOff, true, 16, intraAt, droppedAt, -1, 8 ) )
			return 1;
		std::printf( "   Scene Cut on, GOP 250, cut at 8: I-frames at %s\n", joined( intraAt ).c_str() );
		check( intraAt == std::vector< int > { 0, 8 },
		       "the scene-cut detector inserts one I-frame at the cut and none on motion" );
		g_gopWrong += intraAt == std::vector< int > { 0, 8 } ? 0 : 1;
	}

	//An I-frame is a function of its source alone: two different histories,
	//the same source at the I-frame, the same decoded frame -- bitwise -- and
	//different decoded frames one frame earlier, where it is a P-frame.
	for( int raster = 0; raster < 2; ++raster )
	{
		const int width  = raster == 0 ? 320 : 640;
		const int height = raster == 0 ? 180 : 360;
		Frame atI[ 2 ], beforeI[ 2 ];
		for( int history = 0; history < 2; ++history )
		{
			Rig rig;
			if( !rig.begin( width, height ) )
				return 1;
			rig.set( "GOP", 6.0f );
			rig.set( "Scene Cut", 0.0f );
			rig.set( "Q", 0.3f );
			for( int k = 0; k < 6; ++k )
				if( !rig.frame( k, texturedFrame( width, height, history == 0 ? 31u : 32u, k, -k ) ) )
					return 1;
			beforeI[ history ] = rig.readDecoded();
			if( !rig.frame( 6, demoCard( width, height, 3 ) ) )
				return 1;
			if( !rig.plugin.LastFrameIntraForTest() )
				bad( "frame 6 was not an I-frame" );
			atI[ history ] = rig.readDecoded();
		}
		int worst = 0;
		const long long same = rgbMismatches( atI[ 0 ], atI[ 1 ], worst );
		int worstP = 0;
		const long long differ = rgbMismatches( beforeI[ 0 ], beforeI[ 1 ], worstP );
		std::printf( "   %dx%d two histories: the I-frame differs in %lld samples, the P-frame before it in %lld\n",
		             width, height, same, differ );
		check( same == 0, std::to_string( width ) + "x" + std::to_string( height )
		                      + " an I-frame is bitwise a function of its source alone" );
		check( differ > 0, std::to_string( width ) + "x" + std::to_string( height )
		                       + " the P-frame before it depends on the history (so the claim is not vacuous)" );
		g_gopWrong += same == 0 ? 0 : 1;
	}

	//At Q 0 an I-frame IS its source; at Q > 0 it lands where the transform
	//says, to one code.
	{
		Rig rig;
		if( !rig.begin( 320, 180 ) )
			return 1;
		rig.set( "GOP", 1.0f );
		rig.set( "Q", 0.0f );
		rig.set( "Chroma Q", 0.0f );
		const Frame source = demoCard( 320, 180, 5 );
		if( !rig.frame( 0, source ) )
			return 1;
		int worst = 0;
		const long long wrong = rgbMismatches( rig.readDecoded(), source, worst );
		std::printf( "   I-frame at Q 0: %lld samples differ from the source\n", wrong );
		check( wrong == 0, "an I-frame at Q 0 is its source, bitwise" );

		rig.set( "Q", 0.3f );
		rig.set( "Chroma Q", 0.4f );
		if( !rig.frame( 1, source ) )
			return 1;
		const Frame reference = codec::intraReference( source, 320, 180, controls::QuantStep( 0.3f ),
		                                               controls::QuantStep( 0.4f ), 1.0 );
		int worstQ            = 0;
		const long long wrongQ = rgbMismatches( rig.readDecoded(), reference, worstQ );
		int worstFromSource   = 0;
		const long long changed = rgbMismatches( rig.readDecoded(), source, worstFromSource );
		std::printf( "   I-frame at Q 0.3: %lld samples differ from the CPU reference (worst %d code);"
		             " %lld differ from the source (worst %d), so the quantiser did something\n",
		             wrongQ, worstQ, changed, worstFromSource );
		check( worstQ <= 1, "an I-frame at Q > 0 matches the CPU transform to one code (float order)" );
		check( changed > 0, "at Q > 0 the quantiser measurably changes the picture" );
		g_gopWrong += worstQ <= 1 ? 0 : 1;
	}

	headline( "gop", "cadence / independence assertions failed", std::to_string( g_gopWrong ) + " (I-frames bitwise a function of source at 2 rasters)" );
	return 0;
}

//---------------------------------------------------------------------------
// --drift. Q at its top, no I-frames: the error against a slowly ramping
// source grows by exactly one code a frame, for as long as the ramp stays
// under the quantiser's dead zone.
//
// Derived: a flat 8x8 block of value n has DC coefficient 8n (orthonormal DCT,
// a(0)^2 * 64 = 8). At Q 1 the step is 512, so a residual of n codes per
// pixel quantises to zero while 8n < 256, i.e. n <= 31. For 31 frames the
// decoder adds nothing back and its error against the source is exactly n.
//---------------------------------------------------------------------------
bool driftErrors( int width, int height, bool openLoop, std::vector< double >& errors )
{
	Rig rig;
	if( !rig.begin( width, height ) )
		return false;

	rig.set( "Q", 1.0f );
	rig.set( "Chroma Q", 1.0f );
	rig.set( "Residual Gain", 0.5f );
	rig.set( "Drop I", static_cast< float >( kDropAll ) );
	rig.set( "GOP", 250.0f );
	rig.set( "Scene Cut", 0.0f );
	rig.plugin.SetOpenLoopForTest( openLoop );

	for( int n = 0; n <= 31; ++n )
	{
		const Frame source = flatFrame( width, height, 64 + n, 64 + n, 64 + n );
		if( !rig.frame( n, source ) )
			return false;
		errors.push_back( meanAbsError( rig.readDecoded(), source ) );
	}
	return true;
}

int runDrift()
{
	std::printf( "== drift: closed-loop error grows one code a frame under the dead zone\n" );

	for( int raster = 0; raster < 2; ++raster )
	{
		const int width  = raster == 0 ? 320 : 640;
		const int height = raster == 0 ? 180 : 360;
		std::vector< double > errors;
		if( !driftErrors( width, height, false, errors ) )
			return 1;

		bool monotone = true, exact = true;
		for( int n = 1; n <= 31; ++n )
		{
			monotone = monotone && errors[ n ] > errors[ n - 1 ];
			exact    = exact && std::fabs( errors[ n ] - n ) < 1e-9;
		}
		std::printf( "   %dx%d errors at frames 1, 8, 16, 31: %.3f %.3f %.3f %.3f\n", width, height, errors[ 1 ],
		             errors[ 8 ], errors[ 16 ], errors[ 31 ] );
		check( monotone, std::to_string( width ) + "x" + std::to_string( height )
		                     + " the error grows strictly for 31 frames" );
		check( exact, std::to_string( width ) + "x" + std::to_string( height )
		                  + " and is exactly n codes at frame n, as the dead zone predicts" );
	}

	headline( "drift", "frames of strictly growing error, both rasters", "31 of 31, exactly n codes at frame n" );
	return 0;
}

//---------------------------------------------------------------------------
// --resize. A resize mid-run restarts with an I-frame and never shows a
// cleared or garbage reference.
//---------------------------------------------------------------------------
int runResize()
{
	std::printf( "== resize: a composition that changes resolution mid-run\n" );

	Rig rig;
	if( !rig.begin( 320, 180 ) )
		return 1;

	//The harshest settings: no I-frames allowed, no residual added, so the
	//only way a pixel gets its value is from the reference.
	rig.set( "Drop I", static_cast< float >( kDropAll ) );
	rig.set( "Residual Gain", 0.0f );
	rig.set( "Q", 0.0f );
	rig.set( "Chroma Q", 0.0f );

	for( int k = 0; k < 6; ++k )
		if( !rig.frame( k, flatFrame( 320, 180, 200, 120, 60 ) ) )
			return 1;

	rig.resize( 400, 200 );

	const Frame want = flatFrame( 400, 200, 200, 120, 60 );
	int darkestAfter = 255;
	bool firstIntra  = false;
	bool laterP      = true;
	long long wrong  = 0;
	for( int k = 6; k < 10; ++k )
	{
		if( !rig.frame( k, want ) )
			return 1;
		if( k == 6 )
			firstIntra = rig.plugin.LastFrameIntraForTest();
		else
			laterP = laterP && !rig.plugin.LastFrameIntraForTest();
		const Frame got = rig.readDecoded();
		int worst       = 0;
		wrong += rgbMismatches( got, want, worst );
		for( size_t i = 0; i < got.size(); i += 4 )
			darkestAfter = std::min( darkestAfter, static_cast< int >( got[ i + 1 ] ) );
	}

	std::printf( "   320x180 -> 400x200 under Drop I = All, gain 0: frame 6 intra=%s, frames 7..9 predicted=%s,"
	             " %lld samples wrong, darkest green %d of 120\n",
	             firstIntra ? "yes" : "no", laterP ? "yes" : "no", wrong, darkestAfter );
	check( firstIntra, "the frame after a resize is an I-frame, whatever Drop I says" );
	check( laterP, "and the frames after it are predicted from it again" );
	check( wrong == 0, "no pixel after the resize is anything but the picture (never a cleared reference)" );

	headline( "resize", "samples wrong after a mid-run resize", std::to_string( wrong ) + " (white in, white out)" );
	return 0;
}

//---------------------------------------------------------------------------
// --onset. The first onset after a clip trigger drops an I-frame: the
// detector is primed, so neither the first frame nor a clock jump fires it.
//---------------------------------------------------------------------------
bool onsetRun( bool primed, std::vector< int >& onsetsAt, std::vector< int >& intraAt, std::vector< int >& droppedAt )
{
	Rig rig;
	if( !rig.begin( 320, 180 ) )
		return false;

	rig.set( "Drop I", static_cast< float >( kDropOnOnset ) );
	rig.set( "GOP", 4.0f );
	rig.set( "Scene Cut", 0.0f );
	rig.plugin.OnsetForTest().SetPrimedForTest( primed );

	//A clip retrigger at frame 10: the host's clock jumps back to zero.
	rig.clockJumpAt = 10;

	for( int k = 0; k < 24; ++k )
	{
		//Loud and constant from frame 0 -- a primed detector must not read
		//that as an onset -- then a real hit at frame 14, where every bin
		//doubles.
		const float level = k >= 14 ? 0.8f : 0.2f;
		for( int bin = 0; bin < audio::kBins; ++bin )
			rig.spectrum[ bin ] = level * ( 1.0f - 0.5f * bin / audio::kBins );

		if( !rig.frame( k, demoCard( 320, 180, k ) ) )
			return false;
		if( rig.plugin.OnsetForTest().Fired() )
			onsetsAt.push_back( k );
		if( rig.plugin.LastFrameIntraForTest() )
			intraAt.push_back( k );
		if( rig.plugin.LastFrameDroppedForTest() )
			droppedAt.push_back( k );
	}
	return true;
}

int runOnset()
{
	std::printf( "== onset: the first onset after a clip trigger drops an I-frame\n" );

	std::vector< int > onsetsAt, intraAt, droppedAt;
	if( !onsetRun( true, onsetsAt, intraAt, droppedAt ) )
		return 1;

	std::printf( "   loud from frame 0, clock jump at 10, hit at 14: onsets at %s; I-frames at %s; dropped at %s\n",
	             joined( onsetsAt ).c_str(), joined( intraAt ).c_str(), joined( droppedAt ).c_str() );
	check( onsetsAt == std::vector< int > { 14 },
	       "exactly one onset, at the hit: not on the first frame, not at the clock jump" );
	check( droppedAt == std::vector< int > { 16 }, "the I-frame after the onset is the one dropped" );
	check( intraAt == std::vector< int > { 0, 4, 8, 12, 20 }, "every other I-frame arrives" );

	headline( "onset", "onsets from a primed detector over 24 frames",
	          std::to_string( onsetsAt.size() ) + " (the hit), 0 spurious; I-frame 16 dropped" );
	return 0;
}

//---------------------------------------------------------------------------
// --negative. The checks above can actually fail.
//---------------------------------------------------------------------------
int runNegative()
{
	std::printf( "== negative: the checks can fail\n" );

	const int before = g_failures;
	int rejected     = 0;
	int controls     = 0;

	//1. No tie-break: flat blocks must stop returning zero.
	{
		++controls;
		const VectorCase c = { 320, 180, kBlock16, 16, false };
		Rig rig;
		if( !rig.begin( c.width, c.height ) )
			return 1;
		rig.set( "Block Size", static_cast< float >( c.blockOption ) );
		rig.set( "Search Range", static_cast< float >( c.range ) );
		rig.set( "Scene Cut", 0.0f );
		rig.plugin.SetTieBreakForTest( false );
		const Frame flat = flatFrame( c.width, c.height, 120, 120, 120 );
		if( !rig.frame( 0, flat ) || !rig.frame( 1, flat ) )
			return 1;
		int bx = 0, by = 0;
		const std::vector< int32_t > v = rig.readVectors( bx, by );
		long long nonzero = 0;
		for( int k = 0; k < bx * by; ++k )
			if( v[ k * 4 ] != 0 || v[ k * 4 + 1 ] != 0 )
				++nonzero;
		std::printf( "   without the tie-break, %lld of %d flat blocks return a non-zero vector\n", nonzero, bx * by );
		const bool ok_ = nonzero > 0;
		check( ok_, "the vectors check rejects a search with no tie-break" );
		rejected += ok_;
	}

	//2. Open loop: the drift must stop growing.
	{
		++controls;
		std::vector< double > errors;
		if( !driftErrors( 320, 180, true, errors ) )
			return 1;
		bool monotone = true;
		for( int n = 1; n <= 31; ++n )
			monotone = monotone && errors[ n ] > errors[ n - 1 ];
		std::printf( "   predicting from the SOURCE, errors at frames 1, 8, 16, 31: %.3f %.3f %.3f %.3f\n",
		             errors[ 1 ], errors[ 8 ], errors[ 16 ], errors[ 31 ] );
		check( !monotone, "the drift check rejects open-loop prediction" );
		rejected += !monotone;
	}

	//3. An unprimed detector fires on the first frame.
	{
		++controls;
		std::vector< int > onsetsAt, intraAt, droppedAt;
		if( !onsetRun( false, onsetsAt, intraAt, droppedAt ) )
			return 1;
		std::printf( "   unprimed, the same run: onsets at %s; dropped at %s\n", joined( onsetsAt ).c_str(),
		             joined( droppedAt ).c_str() );
		const bool ok_ = onsetsAt != std::vector< int > { 14 };
		check( ok_, "the onset check rejects a detector that is not primed" );
		rejected += ok_;
	}

	//4. The block copy judged against vectors one pixel out.
	{
		++controls;
		long long wrong = 0;
		if( !moshCase( 320, 180, kBlock16, "320x180 B=16", true, &wrong ) )
			return 1;
		std::printf( "   vectors one pixel out: %lld samples differ\n", wrong );
		check( wrong > 0, "the mosh check rejects a block copy one pixel out" );
		rejected += wrong > 0;
	}

	//5. Lossless at Q > 0.
	{
		++controls;
		Rig rig;
		if( !rig.begin( 320, 180 ) )
			return 1;
		rig.set( "Q", 0.3f );
		rig.set( "Chroma Q", 0.0f );
		rig.set( "Residual Gain", 0.5f );
		const Frame source = demoCard( 320, 180, 0 );
		if( !rig.frame( 0, source ) )
			return 1;
		int worst             = 0;
		const long long wrong = rgbMismatches( rig.readDecoded(), source, worst );
		std::printf( "   at Q 0.3 the 'lossless' comparison finds %lld samples differing, worst %d\n", wrong, worst );
		check( wrong > 0, "the lossless check rejects any quantisation at all" );
		rejected += wrong > 0;
	}

	//6. Two histories judged at a P-frame.
	{
		++controls;
		Frame at[ 2 ];
		for( int history = 0; history < 2; ++history )
		{
			Rig rig;
			if( !rig.begin( 320, 180 ) )
				return 1;
			rig.set( "GOP", 250.0f );
			rig.set( "Scene Cut", 0.0f );
			for( int k = 0; k < 6; ++k )
				if( !rig.frame( k, texturedFrame( 320, 180, history == 0 ? 31u : 32u, k, -k ) ) )
					return 1;
			if( !rig.frame( 6, demoCard( 320, 180, 3 ) ) )
				return 1;
			at[ history ] = rig.readDecoded();
		}
		int worst             = 0;
		const long long wrong = rgbMismatches( at[ 0 ], at[ 1 ], worst );
		std::printf( "   two histories at a P-frame differ in %lld samples\n", wrong );
		check( wrong > 0, "the source-alone check rejects a P-frame" );
		rejected += wrong > 0;
	}

	//7. The vectors judged against a shift one pixel out.
	{
		++controls;
		const VectorCase c = { 320, 180, kBlock16, 16, false };
		std::vector< int32_t > vectors;
		int bx = 0, by = 0;
		if( !vectorsFor( c, 3, -2, nullptr, vectors, bx, by ) )
			return 1;
		long long wrong = 0, judged = 0;
		for( int j = 1; j + 1 < by; ++j )
			for( int i = 1; i + 1 < bx; ++i )
			{
				const int32_t* v = vectors.data() + ( static_cast< size_t >( j ) * bx + i ) * 4;
				++judged;
				if( v[ 0 ] != 2 * 4 || v[ 1 ] != 2 * -2 )
					++wrong;
			}
		std::printf( "   a shift of (3,-2) judged as (4,-2): %lld of %lld interior blocks wrong\n", wrong, judged );
		check( wrong == judged, "the vectors check rejects an expectation one pixel out" );
		rejected += wrong == judged;
	}

	headline( "negative", "perturbations correctly rejected",
	          std::to_string( rejected ) + " of " + std::to_string( controls ) );
	(void)before;
	return 0;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( int width, int height, int frames, const std::vector< std::pair< std::string, float > >& settings,
                bool readback )
{
	Rig rig;
	if( !rig.begin( width, height ) )
		return 0.0;

	for( const auto& s : settings )
		rig.set( s.first, s.second );
	rig.plugin.SetReadbackForTest( readback );

	//Moving content, so the search has something to find and the scene-cut
	//readback happens every frame, as it would in a show.
	std::vector< Frame > sources;
	for( int k = 0; k < 8; ++k )
		sources.push_back( demoCard( width, height, k ) );

	const int warmup = 20;
	for( int k = 0; k < warmup; ++k )
		rig.frame( k, sources[ k % 8 ] );
	glFinish();

	const auto start = std::chrono::steady_clock::now();
	for( int k = 0; k < frames; ++k )
		rig.frame( warmup + k, sources[ k % 8 ] );
	//glFinish on both sides, because GL calls queue and an unsynchronised
	//version times how fast a `for` loop hands work to the driver.
	glFinish();
	const auto end = std::chrono::steady_clock::now();

	return std::chrono::duration< double >( end - start ).count() * 1000.0 / static_cast< double >( frames );
}

int runBench( int frames, const std::vector< std::pair< std::string, float > >& settings )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720 ", 1280, 720 },
		{ "1920x1080", 1920, 1080 },
		{ "3840x2160", 3840, 2160 },
	};

	//Say what was measured. Motion search is the cost and it depends on the
	//block size and the range, so a number without them is not a number.
	Residual probe;
	int blockOption = static_cast< int >( std::lround( probe.GetFloatParameter( Residual::PT_BLOCK_SIZE ) ) );
	int range       = static_cast< int >( std::lround( probe.GetFloatParameter( Residual::PT_SEARCH_RANGE ) ) );
	bool halfPel    = probe.GetFloatParameter( Residual::PT_HALF_PEL ) >= 0.5f;
	for( const auto& s : settings )
	{
		if( s.first == "Block Size" )
			blockOption = static_cast< int >( std::lround( s.second ) );
		if( s.first == "Search Range" )
			range = static_cast< int >( std::lround( s.second ) );
		if( s.first == "Half Pel" )
			halfPel = s.second >= 0.5f;
	}

	std::printf( "%d frames each, after a 20-frame warm-up, glFinish both sides.\n", frames );
	std::printf( "block %d, search range +-%d, half-pel %s, %d pyramid levels.\n\n",
	             codec::kBlockSizes[ std::clamp( blockOption, 0, 2 ) ], range, halfPel ? "on" : "off",
	             codec::levelsFor( codec::kBlockSizes[ std::clamp( blockOption, 0, 2 ) ] ) );
	std::printf( "resolution    ms/frame   equivalent fps   %% of a 60fps frame   without the readback\n" );

	for( const Size& s : sizes )
	{
		const double ms   = benchAt( s.width, s.height, frames, settings, true );
		const double noRb = benchAt( s.width, s.height, frames, settings, false );
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%          %7.3f\n", s.name, ms,
		             ms > 0.0 ? 1000.0 / ms : 0.0, ms / 16.667 * 100.0, noRb );
	}

	std::printf( "\nThe cost is the motion search plus a one-uint readback per frame for the\n"
	             "scene-cut decision, which is a CPU-GPU synchronisation point; the last\n"
	             "column is the same frame with that readback skipped. Run with\n"
	             "--set \"Block Size=0\" --set \"Search Range=32\" for the most expensive search.\n" );
	return 0;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"rstest -- render and check the Residual codec\n"
		"\n"
		"  --out PATH        render the demo card through the plugin (default /tmp/residual.png)\n"
		"  --card PATH       write the demo card alone, undecorated\n"
		"  --size WxH        raster (default 1280x720)\n"
		"  --frames N        frames to render before reading back (default 40)\n"
		"  --fps N           synthetic frame rate driving the clock (default 60)\n"
		"  --set \"Name=V\"    set a parameter by its display name. Repeatable.\n"
		"  --list            print every parameter, its kind and its range, then exit\n"
		"\n"
		"  --transform       YCoCg-R reversible, DCT orthonormal, round trip exact. No GL\n"
		"  --vectors         a translated texture returns exactly (dx, dy)\n"
		"  --lossless        Q 0 and gain 1 give the source back, bitwise\n"
		"  --mosh            no I-frame, no residual: decoded == block copy, bitwise\n"
		"  --gop             I-frames land where the GOP says and are functions of the source\n"
		"  --drift           closed-loop error grows one code a frame under the dead zone\n"
		"  --resize          a resize mid-run restarts with an I-frame, never black\n"
		"  --onset           the first onset after a clip trigger drops an I-frame\n"
		"  --negative        every check above can actually fail\n"
		"  --bench           time ProcessOpenGL at 720p, 1080p and 4K\n"
		"  --help\n" );
}

const char* kindOf( Residual& plugin, unsigned int index )
{
	if( index >= Residual::PT_ABOUT_FIRST )
		return "about";

	switch( plugin.GetParamType( index ) )
	{
	case FF_TYPE_OPTION:
		return "option";
	case FF_TYPE_BOOLEAN:
		return "boolean";
	case FF_TYPE_INTEGER:
		return "integer";
	case FF_TYPE_EVENT:
		return "event";
	case FF_TYPE_BUFFER:
		return "buffer";
	case FF_TYPE_TEXT:
		return "text";
	default:
		return "standard";
	}
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/residual.png";
	std::string cardPath;
	int width  = 1280;
	int height = 720;
	int frames = 40;
	double fps = 60.0;

	bool wantList      = false;
	bool wantTransform = false;
	bool wantVectors   = false;
	bool wantLossless  = false;
	bool wantMosh      = false;
	bool wantGop       = false;
	bool wantDrift     = false;
	bool wantResize    = false;
	bool wantOnset     = false;
	bool wantNegative  = false;
	bool wantBench     = false;

	std::vector< std::string > settings;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;

		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--card" && hasNext )
			cardPath = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t cross     = size.find( 'x' );
			if( cross == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH, e.g. 1280x720\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, cross ).c_str() );
			height = std::atoi( size.substr( cross + 1 ).c_str() );
		}
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--transform" )
			wantTransform = true;
		else if( argument == "--vectors" )
			wantVectors = true;
		else if( argument == "--lossless" )
			wantLossless = true;
		else if( argument == "--mosh" )
			wantMosh = true;
		else if( argument == "--gop" )
			wantGop = true;
		else if( argument == "--drift" )
			wantDrift = true;
		else if( argument == "--resize" )
			wantResize = true;
		else if( argument == "--onset" )
			wantOnset = true;
		else if( argument == "--negative" )
			wantNegative = true;
		else if( argument == "--bench" )
			wantBench = true;
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "size, frames and fps must all be positive\n" );
		return 2;
	}

	const bool glChecks = wantVectors || wantLossless || wantMosh || wantGop || wantDrift || wantResize
	                      || wantOnset || wantNegative;

	//No GL needed, so it is answered before a context is made -- which means
	//it still works on a machine where creating one fails, and in CI.
	if( wantTransform && !glChecks && !wantBench )
	{
		runTransform();
		printSummary();
		std::printf( "\n%s\n", g_failures == 0 ? "all checks passed" : "FAILURES above" );
		return g_failures == 0 ? 0 : 1;
	}

	if( !cardPath.empty() )
	{
		const Frame card = demoCard( width, height, 0 );
		if( !writePng( cardPath, width, height, flipRows( card, width, height ) ) )
		{
			std::fprintf( stderr, "could not write %s\n", cardPath.c_str() );
			return 1;
		}
		std::printf( "wrote %s\n", cardPath.c_str() );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	auto finish = [ & ]( int code ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return code;
	};

	if( wantTransform || glChecks )
	{
		if( wantTransform )
			runTransform();
		if( wantVectors )
			runVectors();
		if( wantLossless )
			runLossless();
		if( wantMosh )
			runMosh();
		if( wantGop )
			runGop();
		if( wantDrift )
			runDrift();
		if( wantResize )
			runResize();
		if( wantOnset )
			runOnset();
		if( wantNegative )
			runNegative();

		printSummary();
		std::printf( "\n%s\n", g_failures == 0 ? "all checks passed" : "FAILURES above" );
		return finish( g_failures == 0 ? 0 : 1 );
	}

	std::vector< std::pair< std::string, float > > parsed;
	for( const std::string& setting : settings )
	{
		const size_t equals = setting.find( '=' );
		if( equals == std::string::npos )
		{
			std::fprintf( stderr, "--set wants Name=Value\n" );
			return finish( 2 );
		}
		parsed.emplace_back( setting.substr( 0, equals ),
		                     std::strtof( setting.substr( equals + 1 ).c_str(), nullptr ) );
	}

	if( wantBench )
		return finish( runBench( frames, parsed ) );

	//--list and --out both need a plugin instance.
	Rig rig;
	rig.fps = fps;
	if( !rig.begin( width, height ) )
		return finish( 1 );

	for( const auto& s : parsed )
		if( !rig.set( s.first, s.second ) )
			return finish( 2 );

	if( wantList )
	{
		std::printf( "%-3s %-18s %-9s %-8s %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( rig.plugin ) )
		{
			const char* kind = kindOf( rig.plugin, p.index );
			if( std::string( kind ) == "about" )
			{
				std::printf( "%-3u %-18s %-9s\n", p.index, p.name.c_str(), "about" );
				continue;
			}

			//An option's range is its element VALUES, which is what --set and
			//the sweep have to use; an integer's is the real range declared.
			float low  = 0.0f;
			float high = 1.0f;
			if( rig.plugin.GetParamType( p.index ) == FF_TYPE_OPTION )
				high = static_cast< float >( rig.plugin.GetNumParamElements( p.index ) ) - 1.0f;
			else if( rig.plugin.GetParamType( p.index ) == FF_TYPE_INTEGER )
			{
				const RangeStruct r = rig.plugin.GetParamRange( p.index );
				low                 = r.min;
				high                = r.max;
			}

			std::printf( "%-3u %-18s %-9s %-8.4f [ %g .. %g ]\n", p.index, p.name.c_str(), kind, p.value, low,
			             high );
		}
		return finish( 0 );
	}

	for( int k = 0; k < frames; ++k )
	{
		if( rig.frame( k, demoCard( width, height, k ) ) )
			continue;
		std::fprintf( stderr, "ProcessOpenGL failed on frame %d\n", k );
		return finish( 1 );
	}

	if( !writePng( outPath, width, height, flipRows( rig.readOutput(), width, height ) ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}

	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
