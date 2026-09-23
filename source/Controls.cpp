#include "Controls.h"

#include "Codec.h"

#include <algorithm>
#include <cmath>

namespace residual::controls
{
namespace
{
double geometric( double lo, double hi, double t )
{
	return lo * std::pow( hi / lo, std::clamp( t, 0.0, 1.0 ) );
}

int roundedInt( float value, int lo, int hi )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), lo, hi );
}
} // namespace

int BlockSize( float option )
{
	return codec::kBlockSizes[ roundedInt( option, 0, kBlockCount - 1 ) ];
}

int SearchRange( float value )
{
	return roundedInt( value, 1, 32 );
}

int GopFrames( float value )
{
	return roundedInt( value, 1, 250 );
}

int VectorHoldFrames( float value )
{
	return roundedInt( value, 1, 60 );
}

double SceneThreshold( float value )
{
	return geometric( 2.0, 64.0, static_cast< double >( value ) );
}

double QuantStep( float value )
{
	return codec::quantStep( value );
}

float ResidualGain( float value )
{
	return 2.0f * std::clamp( value, 0.0f, 1.0f );
}

float VectorScale( float value )
{
	return 4.0f * std::clamp( value, 0.0f, 1.0f );
}

} // namespace residual::controls
