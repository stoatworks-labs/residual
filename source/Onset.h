#pragma once

#include <array>

/**
	An onset detector on the host's 64-bin spectrum, counted in FRAMES.

	Resolume fills an FF_USAGE_FFT buffer parameter once per frame, so the
	finest thing this can resolve is a frame, and every rate in it is a
	per-frame coefficient rather than a time constant. That is a deliberate
	trade: the plugin then has no clock arithmetic at all -- nothing time-like
	reaches a shader, a phase or a filter -- and the one trap the fleet has
	measured Resolume's clock springing (499 million ms, where a float resolves
	0.03 s) has nothing here to spring. Between 30 and 60 fps the detector's
	time constants move by a factor of two, which for "did a hit just land" is
	nothing.

	Spectral flux with an adaptive floor, after macroblock: the positive
	change per bin between two RAW frames, summed, against a running mean of
	itself. Flux is never measured against an envelope -- an envelope is a
	low-pass, and asking a low-passed signal where its corners are makes the
	detector deaf exactly as it is made quicker.

	**It is primed on the first frame.** The first spectrum handed over
	becomes the "previous" one, so nothing reads as having risen from silence,
	and there is no Reset() for a clock jump to call -- a clip retrigger leaves
	the detector exactly as it was. Without that, every bin on the first frame
	is a rise from zero, the floor snaps to it, and the detector is deaf for a
	second and a half after every trigger. `SetPrimedForTest( false )` is the
	negative control that shows the difference.
*/
namespace residual::audio
{

constexpr int kBins = 64;

class Onset
{
public:
	/// One frame's spectrum. `count` may be less than kBins if the host handed
	/// over fewer; the rest read as silence.
	void Update( const float* bins, int count );

	/// True on the frame an onset was detected.
	bool Fired() const
	{
		return fired;
	}

	/// How many onsets since the plugin loaded. The harness counts these.
	unsigned long long Count() const
	{
		return onsets;
	}

	/// The flux this frame and the floor it was judged against, for the log
	/// and the harness.
	float Flux() const
	{
		return flux;
	}
	float Floor() const
	{
		return floorNow;
	}

	/// Negative control: hand the first frame to the detector unprimed, so it
	/// reads every bin as risen from silence.
	void SetPrimedForTest( bool primed )
	{
		primeOnFirstFrame = primed;
	}

private:
	std::array< float, kBins > previous {};
	float fluxMean    = 0.0f;
	float flux        = 0.0f;
	float floorNow    = 0.0f;
	int refractory    = 0;
	bool fired        = false;
	bool seen         = false;
	bool primeOnFirstFrame = true;
	unsigned long long onsets = 0;
};

} // namespace residual::audio
