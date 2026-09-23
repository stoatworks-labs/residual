#include "Onset.h"

#include <algorithm>
#include <cmath>

namespace residual::audio
{
namespace
{
/// The running mean of the flux follows it with this per-frame coefficient:
/// about a second's memory at 60 fps, two at 30.
constexpr float kFluxFall = 1.0f / 60.0f;

/// An onset fires when the flux exceeds its running mean by this factor. One
/// number rather than a control, because the plugin has only one thing to do
/// with an onset and a Sensitivity slider next to Drop I would be a second
/// thing to explain.
constexpr float kFluxMargin = 1.3f;

/// The absolute floor under the adaptive one. Without it the mean of nothing
/// is nothing, and the first faint sound after silence fires.
constexpr float kFluxFloor = 0.004f;

/// Frames after an onset before another can fire. Five frames is 83 ms at
/// 60 fps -- 720 bpm in straight quavers -- so it costs nothing musically and
/// stops one ragged hit from firing three times.
constexpr int kRefractoryFrames = 5;
} // namespace

void Onset::Update( const float* bins, int count )
{
	const int n = std::clamp( count, 0, kBins );

	//sqrt because bin magnitudes bunch hard against zero; a spectrum used raw
	//hears the kick drum and nothing else.
	std::array< float, kBins > raw {};
	for( int i = 0; i < n; ++i )
		raw[ i ] = std::sqrt( std::max( 0.0f, bins ? bins[ i ] : 0.0f ) );

	fired = false;

	if( !seen )
	{
		seen = true;
		if( primeOnFirstFrame )
		{
			//The first spectrum is the previous spectrum. Nothing has risen
			//from anything yet.
			previous = raw;
			flux     = 0.0f;
			floorNow = kFluxFloor;
			return;
		}
	}

	float rise = 0.0f;
	for( int i = 0; i < kBins; ++i )
	{
		//Positive differences only. A bin falling silent is not an onset, and
		//counting it as one makes the end of every note fire too.
		rise += std::max( 0.0f, raw[ i ] - previous[ i ] );
		previous[ i ] = raw[ i ];
	}
	flux = rise / static_cast< float >( kBins );

	floorNow = std::max( kFluxFloor, fluxMean * ( 1.0f + kFluxMargin ) );

	if( refractory > 0 )
		--refractory;

	if( refractory == 0 && flux > floorNow )
	{
		fired      = true;
		refractory = kRefractoryFrames;
		++onsets;
	}

	//The mean follows AFTER the decision, so a hit is judged against the
	//floor as it stood and does not lift the bar it has to clear.
	fluxMean += ( flux - fluxMean ) * kFluxFall;
}

} // namespace residual::audio
