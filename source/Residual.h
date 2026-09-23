#pragma once

#include "Buffer.h"
#include "Codec.h"
#include "Controls.h"
#include "Onset.h"
#include "StoatworksAboutParams.h"

#include <FFGLSDK.h>

#include <cstdint>
#include <string>
#include <vector>

/**
	Residual -- datamosh, built as a codec, as an FFGL effect.

	------------------------------------------------------------- the one idea

	Datamosh is not a glitch. It is a video decoder doing exactly its job with
	the wrong information. A P-frame is a field of motion vectors and a
	residual -- the correction -- applied to THE LAST FRAME THE DECODER
	RECONSTRUCTED. Take the I-frame away at a scene cut and the decoder keeps
	painting the old picture with the new scene's motion. Take the residual
	away and nothing ever corrects it. Repeat one frame's vectors and the
	pixels keep flowing that way. That is the bloom.

	So this is the codec: a block-matching motion estimator on luma, hierarchical
	over a small pyramid; a prediction from the decoder's own reconstructed
	frame, closed-loop, so error propagates the way it does in a real decoder; a
	quantised 8x8 DCT residual; a GOP with a scene-cut detector. And then
	controls that break exactly one piece at a time. Every datamosh look is a
	named failure of a named stage.

	------------------------------------------------------------- the state

	Two things persist across frames and both live on the GPU as integer
	textures: the DECODED frame, which is the next frame's reference, and the
	previous SOURCE frame, which is what the encoder searches against (an
	encoder matches source to source; the decoder applies the result to what
	it has). Everything else is rebuilt each frame.

	A resize reallocates both, and a reallocated buffer is cleared -- so the
	frame after a resize is an I-frame whatever Drop I says, and the log says
	so. There is nothing to predict from.

	------------------------------------------------------------- the clock

	Nothing here uses the host's clock for anything but a line in the log. The
	onset detector counts frames. See Onset.h for why that is a choice and not
	an omission.
*/
class Residual : public CFFGLPlugin
{
public:
	Residual();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// `instantiateGL` pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure. Omit this
	/// and the plugin cannot be created in any real host while every in-repo
	/// check still passes.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// The order the host shows them in: the encoder, the residual, the
	/// breaks, and the view.
	enum ParamID : FFUInt32
	{
		//Encoder
		PT_BLOCK_SIZE,
		PT_SEARCH_RANGE,
		PT_HALF_PEL,
		PT_GOP,
		PT_SCENE_CUT,
		PT_SCENE_THRESHOLD,

		//Residual
		PT_Q,
		PT_RESIDUAL_GAIN,
		PT_CHROMA_Q,

		//Mosh
		PT_DROP_I,
		PT_VECTOR_HOLD,
		PT_VECTOR_SCALE,
		PT_REFRESH,
		PT_AUDIO,

		//View
		PT_SHOW_VECTORS,
		PT_MIX,

		//About. FFGL has no window and cannot make one, so the name, the
		//version, the maker and the links are parameters the host draws with
		//everything else. Last in the enum, so no saved composition's
		//parameter ids shift when more arrive. See StoatworksAboutParams.h.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	//-----------------------------------------------------------------------
	// What the last frame did. The harness reads these; nothing in the
	// plugin's own operation does.
	//-----------------------------------------------------------------------

	/// True if the frame just rendered was an I-frame.
	bool LastFrameIntraForTest() const
	{
		return lastIntra;
	}
	/// True if an I-frame was due and was dropped.
	bool LastFrameDroppedForTest() const
	{
		return lastDropped;
	}
	/// True if the scene-cut detector fired on the frame just rendered.
	bool LastSceneCutForTest() const
	{
		return lastSceneCut;
	}
	/// True if vectors were estimated on the frame just rendered (as opposed
	/// to held from an earlier one).
	bool LastFrameEstimatedForTest() const
	{
		return lastEstimated;
	}
	/// The mean absolute luma difference per pixel after motion compensation.
	double LastMeanSadForTest() const
	{
		return lastMeanSad;
	}
	/// The onset detector.
	const residual::audio::Onset& OnsetForTest() const
	{
		return onset;
	}
	residual::audio::Onset& OnsetForTest()
	{
		return onset;
	}

	/// The decoded frame, RGBA8, row 0 at the bottom. Read straight out of
	/// the integer texture with GL_RGBA_INTEGER, so no conversion is involved.
	bool ReadDecodedForTest( std::vector< uint8_t >& out, int& width, int& height ) const;

	/// The level-0 vectors: four ints per block (x, y in HALF-PEL units, the
	/// SAD at that vector, spare), `blocksX` across, row 0 at the bottom.
	bool ReadVectorsForTest( std::vector< int32_t >& out, int& blocksX, int& blocksY ) const;

	//-----------------------------------------------------------------------
	// Negative controls. Each removes one term of the model so the harness
	// can show the check that depends on it fails.
	//-----------------------------------------------------------------------

	/// Predict from the previous SOURCE frame instead of the decoded one:
	/// open-loop. --drift must then fail.
	void SetOpenLoopForTest( bool openLoop )
	{
		openLoopForTest = openLoop;
	}

	/// Remove the tie-break: equal SADs go to the last candidate tried.
	/// --vectors on flat blocks must then fail.
	void SetTieBreakForTest( bool tieBreak )
	{
		tieBreakForTest = tieBreak;
	}

private:
	/// Bring every buffer to this frame's shape. Returns false if GL refused;
	/// sets `referenceLost` if the decoded frame was reallocated.
	bool ensureBuffers( int width, int height, int blockSize, int levels, bool& referenceLost );

	/// Decide whether this frame is intra, from the GOP, the scene cut, the
	/// Refresh trigger, the Drop I latch and whether there is a reference at
	/// all. Records why in the lastXxx fields.
	bool decideIntra( bool referenceLost, bool estimated, double meanSad );

	void releaseBuffers();

	ffglex::FFGLShader copyShader;
	ffglex::FFGLShader lumaShader;
	ffglex::FFGLShader downsampleShader;
	ffglex::FFGLShader motionShader;
	ffglex::FFGLShader sadRowsShader;
	ffglex::FFGLShader sadTotalShader;
	ffglex::FFGLShader predictShader;
	ffglex::FFGLShader dctRowShader;
	ffglex::FFGLShader dctColShader;
	ffglex::FFGLShader idctColShader;
	ffglex::FFGLShader idctRowShader;
	ffglex::FFGLShader compositeShader;
	ffglex::FFGLScreenQuad quad;

	//-----------------------------------------------------------------------
	// Buffers. Fixed arrays, never std::vector: a Buffer owns GL ids and must
	// not be copied.
	//-----------------------------------------------------------------------
	residual::Buffer source[ 2 ];   ///< RGBA8UI, this frame and the last
	residual::Buffer decoded[ 2 ];  ///< RGBA8UI, the reconstruction, ping-ponged
	residual::Buffer pyramid[ 2 ][ residual::codec::kMaxLevels ]; ///< R8UI luma, per source
	residual::Buffer vectors[ residual::codec::kMaxLevels ];      ///< RGBA32I, one per level
	residual::Buffer predicted;     ///< RGBA8UI
	residual::Buffer coef[ 2 ];     ///< RGBA32F, padded to whole 8x8 blocks
	residual::Buffer sadRows;       ///< R32UI, 1 x blocksY
	residual::Buffer sadTotal;      ///< R32UI, 1 x 1

	int current = 0;///< which of source[] / decoded[] / pyramid[] is THIS frame

	/// Is decoded[ 1 - current ] a real reconstruction? False on the first
	/// frame, after InitGL, and after a resize.
	bool referenceValid = false;

	/// The picture size and block grid the buffers were last shaped for.
	int pictureWidth  = 0;
	int pictureHeight = 0;
	int blockSizeWas  = 0;
	int blocksX       = 0;
	int blocksY       = 0;

	/// The DCT basis, uploaded once per pass.
	float basis[ residual::codec::kTransform * residual::codec::kTransform ] = {};

	//-----------------------------------------------------------------------
	// The GOP and the breaks.
	//-----------------------------------------------------------------------
	int framesSinceIntra = 0;   ///< 1 on an I-frame (or a dropped one), then counting up
	int holdRemaining    = 0;   ///< frames the current vectors are still held for
	bool refreshPending  = false;///< the Refresh trigger, consumed by the next frame
	bool dropArmed       = false;///< Drop I = Next or On Onset: one I-frame will be dropped

	bool lastIntra     = false;
	bool lastDropped   = false;
	bool lastSceneCut  = false;
	bool lastEstimated = false;
	double lastMeanSad = 0.0;

	residual::audio::Onset onset;

	bool openLoopForTest = false;
	bool tieBreakForTest = true;

	//-----------------------------------------------------------------------
	// The host's clock, recorded and logged, used for nothing else.
	//-----------------------------------------------------------------------
	double hostTime     = -1.0;
	double lastHostTime = -1.0;
	int frameCount      = 0;

	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
