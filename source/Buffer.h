#pragma once

#include <FFGLSDK.h>

/**
	A render target of a given INTEGER or float format.

	Not tinsel's PassBuffer, and the reason is the whole plugin: every frame of
	codec state here is an unsigned-integer texture (GL_RGBA8UI, GL_R8UI), the
	vectors are signed (GL_RGBA32I) and the scene-cut sum is GL_R32UI.
	`ffglex::FFGLFBO::Initialise` allocates its colour texture with
	`glTexImage2D( ..., GL_RGBA, GL_UNSIGNED_BYTE or GL_FLOAT, NULL )`, and for
	an integer internal format that pair is GL_INVALID_OPERATION -- the format
	must be GL_RGBA_INTEGER. So the SDK's FBO cannot hold what this plugin
	keeps, and PassBuffer, which subclasses it, cannot either.

	Why integers at all: the headline claims here are BITWISE -- a block copy
	is a pure fetch, Q 0 is lossless, an I-frame is a function of its source
	alone -- and a normalised 8-bit target puts a float-to-fixed conversion
	between the shader and the byte. The GL specification says that conversion
	returns "one of the two" nearest integers, rounding to nearest merely
	"preferred"; every implementation rounds, but a check that is exact by
	construction beats one that is exact on the drivers tried so far. With
	`out uvec4` there is no conversion: the shader writes the byte.

	What it keeps from PassBuffer: allocation only when the shape changes, a
	clear on allocation (an undefined buffer is not "a bit of noise", it is
	whatever the driver handed back, and one of these is the decoder's
	reference), no depth attachment, GL_NEAREST, and no leak on Destroy().

	Every Ensure() in ProcessOpenGL happens before anything binds a texture,
	for the same reason as everywhere else in the fleet: allocation binds and
	unbinds texture unit 0 as a side effect.
*/
namespace residual
{

class Buffer
{
public:
	Buffer() = default;
	Buffer( const Buffer& ) = delete;
	Buffer& operator=( const Buffer& ) = delete;

	/// Bring the buffer to this shape and format, reallocating -- and
	/// clearing -- only if something changed. Returns false if GL would not
	/// give us the memory. `reallocated`, if given, says whether the contents
	/// were lost, which matters for the buffer that is the decoder's reference.
	bool Ensure( int width, int height, GLenum internalFormat, bool* reallocated = nullptr );

	/// Clear to zero, whatever the format.
	void Clear();

	void Destroy();

	GLuint Texture() const
	{
		return texture;
	}
	GLuint Fbo() const
	{
		return fbo;
	}
	int Width() const
	{
		return width;
	}
	int Height() const
	{
		return height;
	}
	bool IsValid() const
	{
		return fbo != 0;
	}

	/// Bind as the render target and size the viewport to it.
	void BindAsTarget() const;

private:
	GLuint fbo     = 0;
	GLuint texture = 0;
	int width      = 0;
	int height     = 0;
	GLenum format  = 0;
};

} // namespace residual
