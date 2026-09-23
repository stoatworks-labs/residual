#include "Codec.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace residual::codec
{

double quantStep( float q )
{
	if( q <= 0.0f )
		return 0.0;
	return std::pow( 2.0, 1.0 + 8.0 * std::min( 1.0, static_cast< double >( q ) ) );
}

void dctBasis( float out[ kTransform * kTransform ] )
{
	const double pi = 3.14159265358979323846;
	for( int u = 0; u < kTransform; ++u )
	{
		const double a = u == 0 ? std::sqrt( 1.0 / kTransform ) : std::sqrt( 2.0 / kTransform );
		for( int x = 0; x < kTransform; ++x )
			out[ u * kTransform + x ] =
				static_cast< float >( a * std::cos( ( 2.0 * x + 1.0 ) * u * pi / ( 2.0 * kTransform ) ) );
	}
}

double dctRoundTripBound()
{
	//Four separable passes, each a sum of eight products. The bound on a pass
	//is: the incoming error scaled by the largest absolute row sum of the
	//basis (0.5 * sum|cos| <= 4), plus this pass's own rounding -- eight
	//products and eight additions, each correctly rounded (GLSL 4.10 §4.5.1
	//requires add and multiply to be), plus the eight basis entries, which
	//were themselves rounded to float on upload. So 24 roundings of at most
	//half an ulp on a value no larger than the pass's magnitude bound.
	//
	//The magnitude bounds come from the transform being orthonormal, so no
	//intermediate exceeds the L2 norm of what it was computed from. A residual
	//sample is at most 510 in magnitude (Co and Cg span -255..255, and a
	//residual is a difference of two of them); a row of eight has norm at most
	//sqrt( 8 ) * 510, and the whole block at most 8 * 510.
	const double ulp       = std::ldexp( 1.0, -24 );
	const double rowNorm   = std::sqrt( 8.0 ) * 510.0;
	const double blockNorm = 8.0 * 510.0;
	const double bounds[ 4 ] = { rowNorm, blockNorm, blockNorm, blockNorm };

	double error = 0.0;
	for( double magnitude : bounds )
		error = 4.0 * error + 24.0 * ulp * magnitude;
	return error;
}

namespace
{
/// One 8-point orthonormal DCT, forward or inverse, in double.
void dct8( const double in[ 8 ], double out[ 8 ], bool inverse )
{
	const double pi = 3.14159265358979323846;
	for( int k = 0; k < 8; ++k )
	{
		double sum = 0.0;
		for( int n = 0; n < 8; ++n )
		{
			const int u    = inverse ? n : k;
			const int x    = inverse ? k : n;
			const double a = u == 0 ? std::sqrt( 1.0 / 8.0 ) : std::sqrt( 2.0 / 8.0 );
			sum += in[ n ] * a * std::cos( ( 2.0 * x + 1.0 ) * u * pi / 16.0 );
		}
		out[ k ] = sum;
	}
}

double quantise( double c, double step )
{
	if( step <= 0.0 )
		return c;
	return std::floor( c / step + 0.5 ) * step;
}
} // namespace

std::vector< uint8_t > intraReference( const std::vector< uint8_t >& rgba, int width, int height,
                                       double stepLuma, double stepChroma, double gain )
{
	std::vector< uint8_t > out( rgba.size() );

	auto at = [ & ]( int x, int y ) {
		x = std::clamp( x, 0, width - 1 );
		y = std::clamp( y, 0, height - 1 );
		return rgba.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
	};

	const int blocksX = blocksAcross( width, kTransform );
	const int blocksY = blocksAcross( height, kTransform );

	for( int by = 0; by < blocksY; ++by )
	{
		for( int bx = 0; bx < blocksX; ++bx )
		{
			//Three planes, replicate-padded at the picture's edge exactly as
			//the shader's clamped fetches pad them.
			double plane[ 3 ][ 8 ][ 8 ];
			for( int j = 0; j < 8; ++j )
				for( int i = 0; i < 8; ++i )
				{
					const uint8_t* p = at( bx * 8 + i, by * 8 + j );
					const YCoCg c    = toYCoCg( p[ 0 ], p[ 1 ], p[ 2 ] );
					plane[ 0 ][ j ][ i ] = c.y;
					plane[ 1 ][ j ][ i ] = c.co;
					plane[ 2 ][ j ][ i ] = c.cg;
				}

			for( int ch = 0; ch < 3; ++ch )
			{
				const double step = ch == 0 ? stepLuma : stepChroma;
				double tmp[ 8 ][ 8 ], coef[ 8 ][ 8 ];

				for( int j = 0; j < 8; ++j )
					dct8( plane[ ch ][ j ], tmp[ j ], false );
				for( int i = 0; i < 8; ++i )
				{
					double col[ 8 ], res[ 8 ];
					for( int j = 0; j < 8; ++j )
						col[ j ] = tmp[ j ][ i ];
					dct8( col, res, false );
					for( int j = 0; j < 8; ++j )
						coef[ j ][ i ] = quantise( res[ j ], step );
				}
				for( int i = 0; i < 8; ++i )
				{
					double col[ 8 ], res[ 8 ];
					for( int j = 0; j < 8; ++j )
						col[ j ] = coef[ j ][ i ];
					dct8( col, res, true );
					for( int j = 0; j < 8; ++j )
						tmp[ j ][ i ] = res[ j ];
				}
				for( int j = 0; j < 8; ++j )
					dct8( tmp[ j ], plane[ ch ][ j ], true );
			}

			for( int j = 0; j < 8; ++j )
				for( int i = 0; i < 8; ++i )
				{
					const int x = bx * 8 + i;
					const int y = by * 8 + j;
					if( x >= width || y >= height )
						continue;

					YCoCg c;
					c.y  = static_cast< int >( std::floor( plane[ 0 ][ j ][ i ] * gain + 0.5 ) );
					c.co = static_cast< int >( std::floor( plane[ 1 ][ j ][ i ] * gain + 0.5 ) );
					c.cg = static_cast< int >( std::floor( plane[ 2 ][ j ][ i ] * gain + 0.5 ) );

					int r, g, b;
					toRGB( c, r, g, b );

					uint8_t* o = out.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
					o[ 0 ]     = static_cast< uint8_t >( std::clamp( r, 0, 255 ) );
					o[ 1 ]     = static_cast< uint8_t >( std::clamp( g, 0, 255 ) );
					o[ 2 ]     = static_cast< uint8_t >( std::clamp( b, 0, 255 ) );
					o[ 3 ]     = at( x, y )[ 3 ];
				}
		}
	}

	return out;
}

std::vector< uint8_t > blockCopy( const std::vector< uint8_t >& reference, int width, int height,
                                  const std::vector< int32_t >& vectors, int blocksX, int blockSize )
{
	std::vector< uint8_t > out( reference.size() );

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const int block     = ( y / blockSize ) * blocksX + ( x / blockSize );
			const int32_t* v    = vectors.data() + static_cast< size_t >( block ) * 4;
			if( ( v[ 0 ] & 1 ) != 0 || ( v[ 1 ] & 1 ) != 0 )
				throw std::runtime_error( "blockCopy: a half-pel vector is not a pure fetch" );

			const int sx = std::clamp( x + v[ 0 ] / kHalfPelUnits, 0, width - 1 );
			const int sy = std::clamp( y + v[ 1 ] / kHalfPelUnits, 0, height - 1 );

			const uint8_t* s = reference.data() + ( static_cast< size_t >( sy ) * width + sx ) * 4;
			uint8_t* o       = out.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			o[ 0 ] = s[ 0 ];
			o[ 1 ] = s[ 1 ];
			o[ 2 ] = s[ 2 ];
			//Alpha is not coded: the decoded frame carries the current source's
			//alpha through untouched, so a copy is asked only about colour.
			o[ 3 ] = 255;
		}
	}

	return out;
}

} // namespace residual::codec
