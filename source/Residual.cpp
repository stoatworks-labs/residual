#include "Residual.h"

#include "Diag.h"
#include "Shaders.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace residual;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Residual >,// Create method
	"RS01",                   // Plugin unique ID of maximum length 4.
	"SW Residual",            // Plugin name
	2,                        // API major version number
	1,                        // API minor version number
	0,                        // Plugin major version number
	1,                        // Plugin minor version number
	FF_EFFECT,                // Plugin type
	"Datamosh, built as a real codec. A block-matching motion estimator, a prediction from the decoder's own last frame, a quantised DCT residual and a GOP - and then controls that break one stage at a time.\n\nDatamosh is not a glitch. It is a decoder doing exactly its job with the wrong information: take away the I-frame at a cut and it keeps painting the old picture with the new scene's motion; take away the residual and nothing ever corrects it; hold one frame's vectors and the pixels keep flowing. Every look is a named failure of a named stage.\n\nStart with Drop I on All and Residual Gain at zero, then cut to a different clip. Refresh is the clean-up button.",// Plugin description
	"Residual FFGL effect"    // About
);

namespace
{
/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour. A logging call must never be
/// the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

const char* const kBlockNames[] = { "8", "16", "32" };
const char* const kDropNames[]  = { "Off", "Next", "All", "On Onset" };

int optionValue( float stored, int count )
{
	return std::clamp( static_cast< int >( std::lround( stored ) ), 0, count - 1 );
}

/// The SDK's Set() has no integer-vector or array overloads.
void setIVec2( const FFGLShader& shader, const char* name, int x, int y )
{
	const GLint location = shader.FindUniform( name );
	if( location >= 0 )
		glUniform2i( location, x, y );
}

void setBasis( const FFGLShader& shader, const float* basis )
{
	const GLint location = shader.FindUniform( "Basis" );
	if( location >= 0 )
		glUniform1fv( location, codec::kTransform * codec::kTransform, basis );
}

void bindTexture( int unit, GLuint texture )
{
	glActiveTexture( GL_TEXTURE0 + unit );
	glBindTexture( GL_TEXTURE_2D, texture );
}

/// The (block, pad) pairs the motion search is compiled for. Level 0 matches
/// the block it codes; every coarser level matches a window half a block
/// wider on each side. 8 -> 4; 16 -> 8 -> 4; 32 -> 16 -> 8 -> 4.
struct MotionVariant
{
	int block;
	int pad;
};
constexpr MotionVariant kMotionVariants[ 6 ] = {
	{ 8, 0 }, { 16, 0 }, { 32, 0 }, { 4, 2 }, { 8, 4 }, { 16, 8 },
};

int motionVariantFor( int block, int pad )
{
	for( int i = 0; i < 6; ++i )
		if( kMotionVariants[ i ].block == block && kMotionVariants[ i ].pad == pad )
			return i;
	return -1;
}

/// Candidates per block along each axis of the SAD buffer. 9 covers the
/// +-2 refinement window (5) and one coarse search up to +-4 in a single
/// pass; wider coarse searches take several chunks, merged.
constexpr int kChunkSide = 9;
} // namespace

//---------------------------------------------------------------------------
Residual::Residual()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The clock is recorded for the log and used for nothing. Declared
	//supported all the same, as every sibling does: it costs nothing and a
	//host that keys buffer delivery to it is not something to find out about
	//in a show.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. SetParamInfo reads each one back out of GetFloatParameter,
	// so these assignments are what the host is told the defaults are.
	//
	// They add up to a working codec at a moderate quantiser: 16-pixel
	// blocks searched over +-16, an I-frame every 30 frames with a scene-cut
	// detector, and the residual added back at unity. Drop I is Off, so out
	// of the box this is a slightly compressed picture, and the effect
	// begins when one thing is turned off.
	//---------------------------------------------------------------------
	params[ PT_BLOCK_SIZE ]      = static_cast< float >( kBlock16 );
	params[ PT_SEARCH_RANGE ]    = 16.0f;
	params[ PT_HALF_PEL ]        = 0.0f;
	params[ PT_GOP ]             = 30.0f;
	params[ PT_SCENE_CUT ]       = 1.0f;
	params[ PT_SCENE_THRESHOLD ] = 0.5f;

	params[ PT_Q ]             = 0.3f;
	params[ PT_RESIDUAL_GAIN ] = 0.5f;//1.0 after mapping
	params[ PT_CHROMA_Q ]      = 0.4f;

	params[ PT_DROP_I ]       = static_cast< float >( kDropOff );
	params[ PT_VECTOR_HOLD ]  = 1.0f;
	params[ PT_VECTOR_SCALE ] = 0.25f;//1.0 after mapping
	params[ PT_REFRESH ]      = 0.0f;

	params[ PT_SHOW_VECTORS ] = 0.0f;
	params[ PT_MIX ]          = 1.0f;

	//---------------------------------------------------------------------
	// Declaration.
	//
	// Ranged sliders are 0..1 and Controls.cpp converts; the genuine counts
	// are FF_TYPE_INTEGER, which SetParamInfo does not clamp, declared with
	// their real ranges. Both option lists are in the order declared: 8, 16,
	// 32 is a progression and Off, Next, All, On Onset reads as one.
	//---------------------------------------------------------------------
	auto integer = [ this ]( unsigned int id, const char* name, float lo, float hi ) {
		SetParamInfo( id, name, FF_TYPE_INTEGER, params[ id ] );
		SetParamRange( id, lo, hi );
	};

	SetOptionParamInfo( PT_BLOCK_SIZE, "Block Size", kBlockCount, params[ PT_BLOCK_SIZE ] );
	for( int i = 0; i < kBlockCount; ++i )
		SetParamElementInfo( PT_BLOCK_SIZE, i, kBlockNames[ i ], static_cast< float >( i ) );

	integer( PT_SEARCH_RANGE, "Search Range", 1.0f, 32.0f );
	SetParamInfo( PT_HALF_PEL, "Half Pel", FF_TYPE_BOOLEAN, false );
	integer( PT_GOP, "GOP", 1.0f, 250.0f );
	SetParamInfo( PT_SCENE_CUT, "Scene Cut", FF_TYPE_BOOLEAN, true );
	SetParamInfo( PT_SCENE_THRESHOLD, "Scene Threshold", FF_TYPE_STANDARD, params[ PT_SCENE_THRESHOLD ] );

	SetParamInfo( PT_Q, "Q", FF_TYPE_STANDARD, params[ PT_Q ] );
	SetParamInfo( PT_RESIDUAL_GAIN, "Residual Gain", FF_TYPE_STANDARD, params[ PT_RESIDUAL_GAIN ] );
	SetParamInfo( PT_CHROMA_Q, "Chroma Q", FF_TYPE_STANDARD, params[ PT_CHROMA_Q ] );

	SetOptionParamInfo( PT_DROP_I, "Drop I", kDropCount, params[ PT_DROP_I ] );
	for( int i = 0; i < kDropCount; ++i )
		SetParamElementInfo( PT_DROP_I, i, kDropNames[ i ], static_cast< float >( i ) );

	integer( PT_VECTOR_HOLD, "Vector Hold", 1.0f, 60.0f );
	SetParamInfo( PT_VECTOR_SCALE, "Vector Scale", FF_TYPE_STANDARD, params[ PT_VECTOR_SCALE ] );

	//A trigger: the host draws a button, and pressing it sends 1 then 0.
	SetParamInfo( PT_REFRESH, "Refresh", FF_TYPE_EVENT, false );

	//The spectrum. Resolume shows an FFT buffer as an audio-source picker and
	//writes one bin per element, low frequencies first. Element defaults are
	//zero on purpose: with nothing routed there are no onsets.
	SetBufferParamInfo( PT_AUDIO, "Audio", audio::kBins, FF_USAGE_FFT );
	for( int i = 0; i < audio::kBins; ++i )
		SetParamElementInfo( PT_AUDIO, i, "", 0.0f );

	SetParamInfo( PT_SHOW_VECTORS, "Show Vectors", FF_TYPE_BOOLEAN, false );
	SetParamInfo( PT_MIX, "Mix", FF_TYPE_STANDARD, params[ PT_MIX ] );

	for( FFUInt32 i = PT_BLOCK_SIZE; i <= PT_SCENE_THRESHOLD; ++i )
		SetParamGroup( i, "Encoder" );
	for( FFUInt32 i = PT_Q; i <= PT_CHROMA_Q; ++i )
		SetParamGroup( i, "Residual" );
	for( FFUInt32 i = PT_DROP_I; i <= PT_AUDIO; ++i )
		SetParamGroup( i, "Mosh" );
	for( FFUInt32 i = PT_SHOW_VECTORS; i <= PT_MIX; ++i )
		SetParamGroup( i, "View" );

	// The About block. Declared inline rather than through a helper, because
	// SetParamInfo is protected on CFFGLPlugin and nothing outside the class
	// can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	codec::dctBasis( basis );

	FFGLLog::LogToHost( "Created Residual effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Residual::InitGL( const FFGLViewportStruct* vp )
{
	//The GL strings first, and unconditionally: when a shader will not compile
	//it is almost always the driver or the GL version, and knowing which
	//machine reported what is most of the diagnosis.
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct Stage
	{
		FFGLShader* shader;
		const char* body;
		const char* name;
	};
	const Stage stages[] = {
		{ &copyShader, shaders::kCopyBody, "copy" },
		{ &lumaShader, shaders::kLumaBody, "luma" },
		{ &downsampleShader, shaders::kDownsampleBody, "downsample" },
		{ &sadRowsShader, shaders::kSadRowsBody, "sadRows" },
		{ &sadTotalShader, shaders::kSadTotalBody, "sadTotal" },
		{ &predictShader, shaders::kPredictBody, "predict" },
		{ &dctRowShader, shaders::kDctRowBody, "dctRow" },
		{ &dctColShader, shaders::kDctColBody, "dctCol" },
		{ &idctColShader, shaders::kIdctColBody, "idctCol" },
		{ &idctRowShader, shaders::kIdctRowBody, "idctRow" },
		{ &compositeShader, shaders::kCompositeBody, "composite" },
	};

	for( const Stage& stage : stages )
	{
		const std::string fragment = shaders::assemble( stage.body );
		if( stage.shader->Compile( shaders::kVertexShader, fragment.c_str() ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume, with no message anywhere. These two
		//lines are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name
		             + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Residual: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	for( int i = 0; i < 6; ++i )
	{
		const std::string sad = shaders::assembleMotion( shaders::kMotionSadBody, kMotionVariants[ i ].block,
		                                                 kMotionVariants[ i ].pad );
		const std::string select = shaders::assembleMotion( shaders::kMotionSelectBody, kMotionVariants[ i ].block,
		                                                    kMotionVariants[ i ].pad );
		if( motionSadShaders[ i ].Compile( shaders::kVertexShader, sad.c_str() )
		    && motionSelectShaders[ i ].Compile( shaders::kVertexShader, select.c_str() ) )
			continue;

		diag::error( "a motion shader for block " + std::to_string( kMotionVariants[ i ].block ) + " pad "
		             + std::to_string( kMotionVariants[ i ].pad )
		             + " failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Residual: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Residual: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	referenceValid   = false;
	framesSinceIntra = 0;
	holdRemaining    = 0;
	frameCount       = 0;

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
bool Residual::ensureBuffers( int width, int height, int blockSize, int levels, bool& referenceLost )
{
	referenceLost = !referenceValid;

	bool lost = false;
	for( int i = 0; i < 2; ++i )
	{
		bool reallocated = false;
		if( !source[ i ].Ensure( width, height, GL_RGBA8UI, &reallocated ) )
			return false;
		lost = lost || reallocated;
		if( !decoded[ i ].Ensure( width, height, GL_RGBA8UI, &reallocated ) )
			return false;
		lost = lost || reallocated;

		for( int l = 0; l < codec::kMaxLevels; ++l )
			if( !pyramid[ i ][ l ].Ensure( codec::levelSize( width, l ), codec::levelSize( height, l ), GL_R8UI ) )
				return false;
	}

	if( lost && referenceValid )
	{
		//See the header: a reallocated buffer is cleared, and one of them was
		//the decoder's reference. Say so -- a resize mid-show that restarts
		//the GOP is worth a line, and nothing in the picture explains it.
		diag::info( "picture " + std::to_string( pictureWidth ) + "x" + std::to_string( pictureHeight )
		            + " -> " + std::to_string( width ) + "x" + std::to_string( height )
		            + ": the reference is gone, restarting with an I-frame" );
	}
	referenceLost = referenceLost || lost;

	pictureWidth  = width;
	pictureHeight = height;

	const int bx = codec::blocksAcross( width, blockSize );
	const int by = codec::blocksAcross( height, blockSize );
	for( int l = 0; l < codec::kMaxLevels; ++l )
	{
		bool reallocated = false;
		if( !vectors[ l ].Ensure( bx, by, GL_RGBA32I, &reallocated ) )
			return false;
		//New vectors are zero, which is nothing to hold on to.
		if( reallocated )
			holdRemaining = 0;
	}
	(void)levels;
	if( !vectorsScratch.Ensure( bx, by, GL_RGBA32I ) )
		return false;
	//9x the block grid: one 9x9 chunk of candidates per block, shared by
	//every level and every chunk pass. 4 bytes each, so 42 MB at 4K with
	//8-pixel blocks and a tenth of that with 16.
	if( !sads.Ensure( bx * kChunkSide, by * kChunkSide, GL_R32I ) )
		return false;
	blocksX      = bx;
	blocksY      = by;
	blockSizeWas = blockSize;

	if( !predicted.Ensure( width, height, GL_RGBA8UI ) )
		return false;

	const int paddedW = codec::blocksAcross( width, codec::kTransform ) * codec::kTransform;
	const int paddedH = codec::blocksAcross( height, codec::kTransform ) * codec::kTransform;
	for( Buffer& c : coef )
		if( !c.Ensure( paddedW, paddedH, GL_RGBA32F ) )
			return false;

	if( !sadRows.Ensure( 1, by, GL_R32UI ) || !sadTotal.Ensure( 1, 1, GL_R32UI ) )
		return false;

	return true;
}

//---------------------------------------------------------------------------
void Residual::searchLevel( int level, int levels, int blockSize, int range, bool halfPel, int cur, int prv )
{
	const int levelBlock = blockSize >> level;
	//Level 0 matches the block it codes. Every coarser level matches a
	//window twice the block, centred on it -- see the shader.
	const int levelPad   = level == 0 ? 0 : levelBlock / 2;
	const int variant    = motionVariantFor( levelBlock, levelPad );
	FFGLShader& sadShader    = motionSadShaders[ variant ];
	FFGLShader& selectShader = motionSelectShaders[ variant ];

	const bool coarsest  = level == levels - 1;
	const int levelRange = codec::rangeAtLevel( range, level );
	const int window     = coarsest ? levelRange : codec::kRefineWindow;
	const int side       = 2 * window + 1;
	const int chunks     = ( side + kChunkSide - 1 ) / kChunkSide;
	const int passes     = chunks * chunks;

	const int imageW = pyramid[ cur ][ level ].Width();
	const int imageH = pyramid[ cur ][ level ].Height();

	//Where the whole-pel result has to land. With a half-pel refinement to
	//follow it lands in the scratch buffer and the refinement writes the
	//real one; otherwise it lands in the real one directly. The chunk merge
	//ping-pongs between the two, so the FIRST pass's target is chosen so
	//that the last one lands where it should.
	const bool refine    = level == 0 && halfPel;
	Buffer* landing      = refine ? &vectorsScratch : &vectors[ level ];
	Buffer* other        = refine ? &vectors[ level ] : &vectorsScratch;
	Buffer* previous     = nullptr;

	for( int pass = 0; pass < passes; ++pass )
	{
		const int cx           = pass % chunks;
		const int cy           = pass / chunks;
		const int offsetX      = -window + cx * kChunkSide;
		const int offsetY      = -window + cy * kChunkSide;
		const bool last        = pass == passes - 1;
		Buffer* target         = last ? landing : ( ( ( passes - 1 - pass ) % 2 == 0 ) ? landing : other );

		//The SAD of every candidate in this chunk, one fragment each.
		{
			sads.BindAsTarget();
			glViewport( 0, 0, blocksX * kChunkSide, blocksY * kChunkSide );
			ScopedShaderBinding shader( sadShader.GetGLID() );
			bindTexture( 0, pyramid[ cur ][ level ].Texture() );
			bindTexture( 1, pyramid[ prv ][ level ].Texture() );
			bindTexture( 2, vectors[ std::min( level + 1, codec::kMaxLevels - 1 ) ].Texture() );
			bindTexture( 3, vectorsScratch.Texture() );
			sadShader.Set( "Cur", 0 );
			sadShader.Set( "Prev", 1 );
			sadShader.Set( "Coarse", 2 );
			sadShader.Set( "Selected", 3 );
			sadShader.Set( "HasCoarse", coarsest ? 0 : 1 );
			setIVec2( sadShader, "ImageSize", imageW, imageH );
			sadShader.Set( "Range", levelRange );
			sadShader.Set( "Window", window );
			sadShader.Set( "ChunkSide", kChunkSide );
			setIVec2( sadShader, "ChunkOffset", offsetX, offsetY );
			sadShader.Set( "HalfPelMode", 0 );
			quad.Draw();
		}

		//The best so far, one fragment per block.
		{
			target->BindAsTarget();
			ScopedShaderBinding shader( selectShader.GetGLID() );
			bindTexture( 0, pyramid[ cur ][ level ].Texture() );
			bindTexture( 1, pyramid[ prv ][ level ].Texture() );
			bindTexture( 2, sads.Texture() );
			bindTexture( 3, vectors[ std::min( level + 1, codec::kMaxLevels - 1 ) ].Texture() );
			//An inactive sampler still has to point at SOMETHING integer, or
			//the driver logs about an incomplete texture on every frame.
			bindTexture( 4, previous ? previous->Texture() : vectorsScratch.Texture() );
			selectShader.Set( "Cur", 0 );
			selectShader.Set( "Prev", 1 );
			selectShader.Set( "Sads", 2 );
			selectShader.Set( "Coarse", 3 );
			selectShader.Set( "Previous", 4 );
			selectShader.Set( "HasCoarse", coarsest ? 0 : 1 );
			selectShader.Set( "HasPrevious", previous ? 1 : 0 );
			setIVec2( selectShader, "ImageSize", imageW, imageH );
			selectShader.Set( "Range", levelRange );
			selectShader.Set( "ChunkSide", kChunkSide );
			setIVec2( selectShader, "ChunkOffset", offsetX, offsetY );
			selectShader.Set( "TieBreak", tieBreakForTest ? 1 : 0 );
			selectShader.Set( "HalfPelMode", 0 );
			selectShader.Set( "OutputHalfPel", ( level == 0 && !refine ) ? 1 : 0 );
			quad.Draw();
		}

		previous = target;
	}

	if( !refine )
		return;

	//The eight half-pel neighbours of the whole-pel winner, then the merge
	//that writes the level's real result in half-pel units.
	{
		sads.BindAsTarget();
		glViewport( 0, 0, blocksX * 3, blocksY * 3 );
		ScopedShaderBinding shader( sadShader.GetGLID() );
		bindTexture( 0, pyramid[ cur ][ level ].Texture() );
		bindTexture( 1, pyramid[ prv ][ level ].Texture() );
		bindTexture( 2, vectors[ 1 ].Texture() );
		bindTexture( 3, vectorsScratch.Texture() );
		sadShader.Set( "Cur", 0 );
		sadShader.Set( "Prev", 1 );
		sadShader.Set( "Coarse", 2 );
		sadShader.Set( "Selected", 3 );
		sadShader.Set( "HasCoarse", 0 );
		setIVec2( sadShader, "ImageSize", imageW, imageH );
		sadShader.Set( "Range", levelRange );
		sadShader.Set( "Window", 1 );
		sadShader.Set( "ChunkSide", 3 );
		setIVec2( sadShader, "ChunkOffset", -1, -1 );
		sadShader.Set( "HalfPelMode", 1 );
		quad.Draw();
	}
	{
		vectors[ 0 ].BindAsTarget();
		ScopedShaderBinding shader( selectShader.GetGLID() );
		bindTexture( 0, pyramid[ cur ][ level ].Texture() );
		bindTexture( 1, pyramid[ prv ][ level ].Texture() );
		bindTexture( 2, sads.Texture() );
		bindTexture( 3, vectors[ 1 ].Texture() );
		bindTexture( 4, vectorsScratch.Texture() );
		selectShader.Set( "Cur", 0 );
		selectShader.Set( "Prev", 1 );
		selectShader.Set( "Sads", 2 );
		selectShader.Set( "Coarse", 3 );
		selectShader.Set( "Previous", 4 );
		selectShader.Set( "HasCoarse", 0 );
		selectShader.Set( "HasPrevious", 1 );
		setIVec2( selectShader, "ImageSize", imageW, imageH );
		selectShader.Set( "Range", levelRange );
		selectShader.Set( "ChunkSide", 3 );
		setIVec2( selectShader, "ChunkOffset", -1, -1 );
		selectShader.Set( "TieBreak", tieBreakForTest ? 1 : 0 );
		selectShader.Set( "HalfPelMode", 1 );
		selectShader.Set( "OutputHalfPel", 1 );
		quad.Draw();
	}
}

//---------------------------------------------------------------------------
bool Residual::decideIntra( bool referenceLost, bool estimated, double meanSad )
{
	const int drop  = optionValue( params[ PT_DROP_I ], kDropCount );
	const int gop   = controls::GopFrames( params[ PT_GOP ] );
	const bool cuts = params[ PT_SCENE_CUT ] >= 0.5f;

	//A cut is judged only on a frame whose vectors are fresh. Held vectors
	//carry the SAD of the frame they were found on, which says nothing about
	//this one.
	lastSceneCut = estimated && cuts && !referenceLost
	               && meanSad > controls::SceneThreshold( params[ PT_SCENE_THRESHOLD ] );

	const bool due    = framesSinceIntra >= gop;
	const bool wanted = referenceLost || refreshPending || due || lastSceneCut;

	bool intra  = wanted;
	lastDropped = false;

	//Two I-frames cannot be dropped: the one with nothing to predict from,
	//and the one the operator just pressed the button for.
	if( wanted && !referenceLost && !refreshPending )
	{
		switch( drop )
		{
		case kDropNext:
		case kDropOnOnset:
			if( dropArmed )
			{
				intra       = false;
				dropArmed   = false;
				lastDropped = true;
			}
			break;
		case kDropAll:
			intra       = false;
			lastDropped = true;
			break;
		default:
			break;
		}
	}

	refreshPending = false;

	//A dropped I-frame still ends the GOP: the encoder made one, the decoder
	//never saw it, and the next comes a GOP later, as in the real thing.
	//
	//The count starts at ONE on the I-frame itself -- this frame is the
	//first of its GOP -- so that frame G after it reads G and is due. Started
	//at zero it read G - 1 there and every I-frame landed a frame late, which
	//--gop found on its first run.
	if( intra || lastDropped )
		framesSinceIntra = 1;
	else
		++framesSinceIntra;

	lastIntra = intra;
	return intra;
}

//---------------------------------------------------------------------------
FFResult Residual::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	const int width  = static_cast< int >( picture.Width );
	const int height = static_cast< int >( picture.Height );

	//The host's viewport, read before anything of ours changes it. Every
	//pass here sizes its own viewport, and the composite draws to the host's
	//framebuffer, which has no buffer of ours to size itself from.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//---------------------------------------------------------------------
	// What the controls say.
	//---------------------------------------------------------------------
	const int blockSize   = controls::BlockSize( params[ PT_BLOCK_SIZE ] );
	const int range       = controls::SearchRange( params[ PT_SEARCH_RANGE ] );
	const bool halfPel    = params[ PT_HALF_PEL ] >= 0.5f;
	const int hold        = controls::VectorHoldFrames( params[ PT_VECTOR_HOLD ] );
	const float stepLuma  = static_cast< float >( controls::QuantStep( params[ PT_Q ] ) );
	const float stepChroma = static_cast< float >( controls::QuantStep( params[ PT_CHROMA_Q ] ) );
	const float gain      = controls::ResidualGain( params[ PT_RESIDUAL_GAIN ] );
	const float scale     = controls::VectorScale( params[ PT_VECTOR_SCALE ] );
	const int levels      = codec::levelsFor( blockSize );

	//---------------------------------------------------------------------
	// Buffers. Every Ensure() happens here, before anything binds a texture:
	// allocation binds and unbinds unit 0 as a side effect.
	//---------------------------------------------------------------------
	bool referenceLost = false;
	if( !ensureBuffers( width, height, blockSize, levels, referenceLost ) )
	{
		diag::error( "could not allocate the codec's buffers at " + std::to_string( width ) + "x"
		             + std::to_string( height ) );
		return FF_FAIL;
	}
	if( referenceLost )
		holdRemaining = 0;

	current       = 1 - current;
	const int cur = current;
	const int prv = 1 - current;

	//---------------------------------------------------------------------
	// 1. The picture, exactly, into an integer texture of ours.
	//---------------------------------------------------------------------
	{
		source[ cur ].BindAsTarget();
		ScopedShaderBinding shader( copyShader.GetGLID() );
		bindTexture( 0, picture.Handle );
		copyShader.Set( "Source", 0 );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 2. The luma pyramid. Every level, every frame: a block size change
	//    next frame needs the previous frame's coarse levels to exist.
	//---------------------------------------------------------------------
	{
		pyramid[ cur ][ 0 ].BindAsTarget();
		ScopedShaderBinding shader( lumaShader.GetGLID() );
		bindTexture( 0, source[ cur ].Texture() );
		lumaShader.Set( "Source", 0 );
		quad.Draw();
	}
	for( int l = 1; l < codec::kMaxLevels; ++l )
	{
		pyramid[ cur ][ l ].BindAsTarget();
		ScopedShaderBinding shader( downsampleShader.GetGLID() );
		bindTexture( 0, pyramid[ cur ][ l - 1 ].Texture() );
		downsampleShader.Set( "Fine", 0 );
		setIVec2( downsampleShader, "FineSize", pyramid[ cur ][ l - 1 ].Width(), pyramid[ cur ][ l - 1 ].Height() );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 3. Audio. One spectrum per frame; an onset arms the Drop I latch when
	//    the mode asks for it.
	//---------------------------------------------------------------------
	{
		float bins[ audio::kBins ] = {};
		int count                  = 0;
		if( const ParamInfo* info = FindParamInfo( PT_AUDIO ) )
		{
			count = static_cast< int >( std::min< size_t >( info->elements.size(), audio::kBins ) );
			for( int i = 0; i < count; ++i )
				bins[ i ] = info->elements[ i ].value;
		}
		onset.Update( bins, count );

		if( onset.Fired() && optionValue( params[ PT_DROP_I ], kDropCount ) == kDropOnOnset )
			dropArmed = true;
	}

	//---------------------------------------------------------------------
	// 4. Motion. Coarsest level first, with the whole range; each level
	//    below it a small window around the doubled coarse vector. Skipped
	//    while vectors are held, and when there is no previous frame.
	//---------------------------------------------------------------------
	const bool estimate = !referenceLost && holdRemaining == 0;
	double meanSad      = lastMeanSad;

	if( estimate )
	{
		for( int l = levels - 1; l >= 0; --l )
			searchLevel( l, levels, blockSize, range, halfPel, cur, prv );

		//The SADs of the chosen vectors, summed to one number for the
		//scene-cut decision. Two passes, so no fragment sums more than a row.
		{
			sadRows.BindAsTarget();
			ScopedShaderBinding shader( sadRowsShader.GetGLID() );
			bindTexture( 0, vectors[ 0 ].Texture() );
			sadRowsShader.Set( "Vectors", 0 );
			sadRowsShader.Set( "BlocksX", blocksX );
			quad.Draw();
		}
		{
			sadTotal.BindAsTarget();
			ScopedShaderBinding shader( sadTotalShader.GetGLID() );
			bindTexture( 0, sadRows.Texture() );
			sadTotalShader.Set( "Rows", 0 );
			sadTotalShader.Set( "BlocksY", blocksY );
			quad.Draw();
		}

		//One unsigned int back from the GPU. This is a synchronisation point
		//-- the CPU waits for the motion search to finish -- and it is here on
		//purpose: the frame-type decision is made once, on the CPU, from one
		//number, rather than being split across the two sides a frame apart.
		//The bench measures what it costs.
		GLuint sum = 0;
		if( readbackForTest )
		{
			glBindFramebuffer( GL_FRAMEBUFFER, sadTotal.Fbo() );
			glReadPixels( 0, 0, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &sum );
		}
		meanSad = static_cast< double >( sum ) / ( static_cast< double >( width ) * height );

		holdRemaining = hold - 1;
	}
	else if( holdRemaining > 0 )
	{
		--holdRemaining;
	}

	lastEstimated = estimate;
	lastMeanSad   = meanSad;

	//---------------------------------------------------------------------
	// 5. I or P.
	//---------------------------------------------------------------------
	const bool intra = decideIntra( referenceLost, estimate, meanSad );

	//---------------------------------------------------------------------
	// 6. The prediction: the reference, block-copied by the vectors.
	//---------------------------------------------------------------------
	if( !intra )
	{
		predicted.BindAsTarget();
		ScopedShaderBinding shader( predictShader.GetGLID() );
		bindTexture( 0, openLoopForTest ? source[ prv ].Texture() : decoded[ prv ].Texture() );
		bindTexture( 1, vectors[ 0 ].Texture() );
		predictShader.Set( "Reference", 0 );
		predictShader.Set( "Vectors", 1 );
		setIVec2( predictShader, "ImageSize", width, height );
		predictShader.Set( "Block", blockSize );
		predictShader.Set( "Scale", scale );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 7. The residual, forward, quantised.
	//---------------------------------------------------------------------
	{
		coef[ 0 ].BindAsTarget();
		ScopedShaderBinding shader( dctRowShader.GetGLID() );
		bindTexture( 0, source[ cur ].Texture() );
		bindTexture( 1, predicted.Texture() );
		dctRowShader.Set( "Source", 0 );
		dctRowShader.Set( "Predicted", 1 );
		setIVec2( dctRowShader, "ImageSize", width, height );
		dctRowShader.Set( "Intra", intra ? 1 : 0 );
		setBasis( dctRowShader, basis );
		quad.Draw();
	}
	{
		coef[ 1 ].BindAsTarget();
		ScopedShaderBinding shader( dctColShader.GetGLID() );
		bindTexture( 0, coef[ 0 ].Texture() );
		dctColShader.Set( "Rows", 0 );
		dctColShader.Set( "StepLuma", stepLuma );
		dctColShader.Set( "StepChroma", stepChroma );
		setBasis( dctColShader, basis );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 8. Back, and reconstructed: the decoded frame.
	//---------------------------------------------------------------------
	{
		coef[ 0 ].BindAsTarget();
		ScopedShaderBinding shader( idctColShader.GetGLID() );
		bindTexture( 0, coef[ 1 ].Texture() );
		idctColShader.Set( "Coef", 0 );
		setBasis( idctColShader, basis );
		quad.Draw();
	}
	{
		decoded[ cur ].BindAsTarget();
		ScopedShaderBinding shader( idctRowShader.GetGLID() );
		bindTexture( 0, coef[ 0 ].Texture() );
		bindTexture( 1, source[ cur ].Texture() );
		bindTexture( 2, predicted.Texture() );
		idctRowShader.Set( "Cols", 0 );
		idctRowShader.Set( "Source", 1 );
		idctRowShader.Set( "Predicted", 2 );
		setIVec2( idctRowShader, "ImageSize", width, height );
		idctRowShader.Set( "Intra", intra ? 1 : 0 );
		//Residual Gain scales what a P-frame adds back. An I-frame is ALL
		//residual -- there is no prediction under it -- so scaling it would
		//make Refresh at gain 0 paint a black frame, and the operator's clean-
		//up button has to produce a picture. I-frames reconstruct at unity.
		idctRowShader.Set( "Gain", intra ? 1.0f : gain );
		setBasis( idctRowShader, basis );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 9. To the host.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( compositeShader.GetGLID() );
		bindTexture( 0, source[ cur ].Texture() );
		bindTexture( 1, decoded[ cur ].Texture() );
		bindTexture( 2, vectors[ 0 ].Texture() );
		compositeShader.Set( "Source", 0 );
		compositeShader.Set( "Decoded", 1 );
		compositeShader.Set( "Vectors", 2 );
		setIVec2( compositeShader, "ImageSize", width, height );
		compositeShader.Set( "Block", blockSize );
		compositeShader.Set( "Scale", scale );
		compositeShader.Set( "MixAmount", std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) );
		compositeShader.Set( "ShowVectors", params[ PT_SHOW_VECTORS ] >= 0.5f ? 1 : 0 );
		quad.Draw();
	}

	for( int unit = 4; unit >= 0; --unit )
		bindTexture( unit, 0 );

	referenceValid = true;

	if( ++frameCount == 60 )
	{
		//Once. The clock's unit is not used for anything here, but what a
		//host sends is worth a line for whoever does need it next.
		diag::info( "frame 60: host time=" + std::to_string( hostTime ) + " picture="
		            + std::to_string( width ) + "x" + std::to_string( height ) + " blocks="
		            + std::to_string( blocksX ) + "x" + std::to_string( blocksY ) + " meanSad="
		            + std::to_string( meanSad ) );
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
void Residual::releaseBuffers()
{
	for( int i = 0; i < 2; ++i )
	{
		source[ i ].Destroy();
		decoded[ i ].Destroy();
		coef[ i ].Destroy();
		for( Buffer& b : pyramid[ i ] )
			b.Destroy();
	}
	for( Buffer& b : vectors )
		b.Destroy();
	vectorsScratch.Destroy();
	sads.Destroy();
	predicted.Destroy();
	sadRows.Destroy();
	sadTotal.Destroy();
}

FFResult Residual::DeInitGL()
{
	copyShader.FreeGLResources();
	lumaShader.FreeGLResources();
	downsampleShader.FreeGLResources();
	for( FFGLShader& shader : motionSadShaders )
		shader.FreeGLResources();
	for( FFGLShader& shader : motionSelectShaders )
		shader.FreeGLResources();
	sadRowsShader.FreeGLResources();
	sadTotalShader.FreeGLResources();
	predictShader.FreeGLResources();
	dctRowShader.FreeGLResources();
	dctColShader.FreeGLResources();
	idctColShader.FreeGLResources();
	idctRowShader.FreeGLResources();
	compositeShader.FreeGLResources();
	quad.Release();

	releaseBuffers();

	referenceValid = false;
	pictureWidth   = 0;
	pictureHeight  = 0;
	blocksX        = 0;
	blocksY        = 0;
	holdRemaining  = 0;

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Residual::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing, so they are handled
	// before the params[] write below -- there is no value to keep.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_REFRESH )
	{
		//An event arrives as 1 on press and 0 on release. It is not stored:
		//the button is a trigger, and the frame after it is an I-frame.
		if( value >= 0.5f )
			refreshPending = true;
		return FF_SUCCESS;
	}

	if( index == PT_DROP_I )
	{
		//Next arms on SELECTION, so choosing it again after it has spent
		//itself arms it again; a host restating the same value is not a
		//selection. Leaving the mode disarms, so a latch set under On Onset
		//does not fire under Off.
		const int was = optionValue( params[ PT_DROP_I ], kDropCount );
		const int now = optionValue( value, kDropCount );
		if( now != was )
			dropArmed = now == kDropNext;
	}

	params[ index ] = value;
	return FF_SUCCESS;
}

float Residual::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Residual::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Residual::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

FFResult Residual::SetTime( double time )
{
	//Recorded for the log at frame 60 and for nothing else. A jump backwards
	//-- a clip retrigger -- changes nothing here on purpose; see Onset.h.
	lastHostTime = hostTime;
	hostTime     = time;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
bool Residual::ReadDecodedForTest( std::vector< uint8_t >& out, int& width, int& height ) const
{
	const Buffer& b = decoded[ current ];
	if( !b.IsValid() )
		return false;

	width  = b.Width();
	height = b.Height();
	out.resize( static_cast< size_t >( width ) * height * 4 );

	GLint previousFbo = 0;
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previousFbo );
	glBindFramebuffer( GL_FRAMEBUFFER, b.Fbo() );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glReadPixels( 0, 0, width, height, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, out.data() );
	glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( previousFbo ) );
	return true;
}

bool Residual::ReadVectorsForTest( std::vector< int32_t >& out, int& bx, int& by ) const
{
	const Buffer& b = vectors[ 0 ];
	if( !b.IsValid() )
		return false;

	bx = b.Width();
	by = b.Height();
	out.resize( static_cast< size_t >( bx ) * by * 4 );

	GLint previousFbo = 0;
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previousFbo );
	glBindFramebuffer( GL_FRAMEBUFFER, b.Fbo() );
	glPixelStorei( GL_PACK_ALIGNMENT, 4 );
	glReadPixels( 0, 0, bx, by, GL_RGBA_INTEGER, GL_INT, out.data() );
	glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( previousFbo ) );
	return true;
}
