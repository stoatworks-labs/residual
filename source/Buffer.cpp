#include "Buffer.h"

namespace residual
{
namespace
{
/// The (format, type) pair glTexImage2D needs for each internal format used
/// here. Integer internal formats need the _INTEGER format enum; passing
/// GL_RGBA for one of them is GL_INVALID_OPERATION and the texture is never
/// allocated, which is the reason this class exists.
struct Layout
{
	GLenum format;
	GLenum type;
	enum Kind
	{
		Unsigned,
		Signed,
		Float
	} kind;
};

bool layoutFor( GLenum internalFormat, Layout& out )
{
	switch( internalFormat )
	{
	case GL_RGBA8UI: out = { GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, Layout::Unsigned }; return true;
	case GL_R8UI:    out = { GL_RED_INTEGER, GL_UNSIGNED_BYTE, Layout::Unsigned }; return true;
	case GL_R32UI:   out = { GL_RED_INTEGER, GL_UNSIGNED_INT, Layout::Unsigned }; return true;
	case GL_RGBA32I: out = { GL_RGBA_INTEGER, GL_INT, Layout::Signed }; return true;
	case GL_RGBA32F: out = { GL_RGBA, GL_FLOAT, Layout::Float }; return true;
	default: return false;
	}
}
} // namespace

bool Buffer::Ensure( int requestedWidth, int requestedHeight, GLenum internalFormat, bool* reallocated )
{
	if( reallocated )
		*reallocated = false;

	if( requestedWidth <= 0 || requestedHeight <= 0 )
		return false;

	if( fbo != 0 && width == requestedWidth && height == requestedHeight && format == internalFormat )
		return true;

	Layout layout {};
	if( !layoutFor( internalFormat, layout ) )
		return false;

	Destroy();

	GLint previousTexture = 0;
	GLint previousFbo     = 0;
	glGetIntegerv( GL_TEXTURE_BINDING_2D, &previousTexture );
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previousFbo );

	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, static_cast< GLint >( internalFormat ), requestedWidth, requestedHeight, 0,
	              layout.format, layout.type, nullptr );
	//Nearest, always. Every read of these is a texelFetch, and an integer
	//texture cannot be filtered anyway -- a sampler with GL_LINEAR on one is
	//incomplete and returns zeros without a word.
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );

	const bool complete = glCheckFramebufferStatus( GL_FRAMEBUFFER ) == GL_FRAMEBUFFER_COMPLETE;

	//Restore rather than clear to 0: ffglex's scoped bindings clear on exit
	//and that is the trap; this class does not add to it.
	glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( previousFbo ) );
	glBindTexture( GL_TEXTURE_2D, static_cast< GLuint >( previousTexture ) );

	if( !complete )
	{
		Destroy();
		return false;
	}

	width  = requestedWidth;
	height = requestedHeight;
	format = internalFormat;

	Clear();

	if( reallocated )
		*reallocated = true;
	return true;
}

void Buffer::Clear()
{
	if( fbo == 0 )
		return;

	Layout layout {};
	layoutFor( format, layout );

	GLint previousFbo = 0;
	glGetIntegerv( GL_FRAMEBUFFER_BINDING, &previousFbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );

	//glClear ignores the integer-ness of the target -- its clear colour is a
	//float and the result on an integer buffer is undefined. glClearBuffer*
	//takes the right type.
	switch( layout.kind )
	{
	case Layout::Unsigned:
	{
		const GLuint zero[ 4 ] = { 0, 0, 0, 0 };
		glClearBufferuiv( GL_COLOR, 0, zero );
		break;
	}
	case Layout::Signed:
	{
		const GLint zero[ 4 ] = { 0, 0, 0, 0 };
		glClearBufferiv( GL_COLOR, 0, zero );
		break;
	}
	case Layout::Float:
	{
		const GLfloat zero[ 4 ] = { 0.0f, 0.0f, 0.0f, 0.0f };
		glClearBufferfv( GL_COLOR, 0, zero );
		break;
	}
	}

	glBindFramebuffer( GL_FRAMEBUFFER, static_cast< GLuint >( previousFbo ) );
}

void Buffer::BindAsTarget() const
{
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glViewport( 0, 0, width, height );
}

void Buffer::Destroy()
{
	if( fbo != 0 )
	{
		glDeleteFramebuffers( 1, &fbo );
		fbo = 0;
	}
	if( texture != 0 )
	{
		glDeleteTextures( 1, &texture );
		texture = 0;
	}
	width  = 0;
	height = 0;
	format = 0;
}

} // namespace residual
