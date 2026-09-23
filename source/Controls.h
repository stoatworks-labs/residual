#pragma once

/**
	Host parameters to what they mean.

	Every FF_TYPE_STANDARD parameter here is 0..1 and converted in this file.
	`CFFGLPluginManager::SetParamInfo` clamps a standard default into 0..1
	*before* returning, and `SetParamRange` can only be called afterwards -- so
	a slider declared in quantiser steps could not declare a default in
	quantiser steps. The counts that are genuinely integers -- the search range,
	the GOP length, the vector hold -- are FF_TYPE_INTEGER, which is exempt from
	the clamp and is declared with its real range; those need no conversion and
	are read straight out of params[].

	The harness calls these rather than re-deriving them. A test that worked
	out for itself what a Q of 0.3 means would agree with itself and prove
	nothing about the plugin.
*/
namespace residual
{
namespace controls
{

/// The motion block size in pixels for a Block Size option value: 8, 16, 32.
int BlockSize( float option );

/// The search range in whole pixels, straight from the integer parameter.
int SearchRange( float value );

/// GOP length in frames, straight from the integer parameter. 1 means every
/// frame is an I-frame, which is a codec with no prediction in it.
int GopFrames( float value );

/// Frames a set of vectors is reused for, straight from the integer parameter.
/// 1 means fresh vectors every frame.
int VectorHoldFrames( float value );

/// The scene-cut threshold as a mean absolute luma difference per pixel AFTER
/// motion compensation, 2 to 64 geometrically. A cut between unrelated
/// pictures leaves tens of codes per pixel that no vector can explain;
/// ordinary motion leaves a few.
double SceneThreshold( float value );

/// The quantiser step for Q and Chroma Q: 0 (bypass) at 0, then 2 .. 512.
/// See codec::quantStep, which is what this calls.
double QuantStep( float value );

/// What the dequantised residual is multiplied by before it is added back:
/// 0 .. 2, so 0.5 on the slider is exactly 1.0 and the codec is honest.
float ResidualGain( float value );

/// What the vectors are multiplied by before the block copy: 0 .. 4, so 0.25
/// on the slider is exactly 1.0.
float VectorScale( float value );

} // namespace controls

/// What Drop I stores. Option VALUES, declared in this order.
enum DropMode
{
	kDropOff     = 0,///< I-frames arrive as the encoder makes them
	kDropNext    = 1,///< the next I-frame is dropped, then the latch is spent
	kDropAll     = 2,///< no I-frame ever arrives
	kDropOnOnset = 3,///< an audio onset arms the latch
	kDropCount
};

/// What Block Size stores.
enum BlockOption
{
	kBlock8  = 0,
	kBlock16 = 1,
	kBlock32 = 2,
	kBlockCount
};

} // namespace residual
