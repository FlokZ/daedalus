/*
Copyright (C) 2001 StrmnNrmn

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "stdafx.h"
#include "CachedTexture.h"

#include "TextureInfo.h"
#include "ConvertImage.h"
#include "ConfigOptions.h"
#include "Graphics/NativePixelFormat.h"
#include "Graphics/NativeTexture.h"
#include "Graphics/ColourValue.h"
#include "Graphics/PngUtil.h"

#include "OSHLE/ultra_gbi.h"

#include "Core/ROM.h"

#include "Debug/DBGConsole.h"
#include "Debug/Dump.h"

#include "Utility/Profiler.h"
#include "Utility/IO.h"
#include "Utility/AuxFunc.h"

#include "Math/MathUtil.h"
#include "Math/Math.h"

#include <vector>

//*****************************************************************************
//
//*****************************************************************************
namespace
{
	std::vector<u8>		gTexelBuffer;
	NativePf8888		gPaletteBuffer[ 256 ];

	template< typename T >
	T * AddByteOffset( T * p, s32 offset )
	{
		return reinterpret_cast< T * >( reinterpret_cast< u8 * >( p ) + offset );
	}
	template< typename T >
	const T * AddByteOffset( const T * p, s32 offset )
	{
		return reinterpret_cast< const T * >( reinterpret_cast< const u8 * >( p ) + offset );
	}


	bool	GenerateTexels( void ** p_texels, void ** p_palette, const TextureInfo & texture_info, ETextureFormat texture_format, u32 pitch, u32 buffer_size )
	{
		TextureDestInfo dst( texture_format );

		if( gTexelBuffer.size() < buffer_size ) //|| gTexelBuffer.size() > (128 * 1024))//Cut off for downsizing may need to be adjusted to prevent some thrashing
		{
#ifdef DAEDALUS_DEBUG_DISPLAYLIST
			printf( "Resizing texel buffer to %d bytes. Texture is %dx%d\n", buffer_size, texture_info.GetWidth(), texture_info.GetHeight() );
#endif
			gTexelBuffer.resize( buffer_size );
		}

		void *			texels  = &gTexelBuffer[0];
		NativePf8888 *	palette = IsTextureFormatPalettised( dst.Format ) ? gPaletteBuffer : NULL;

		//memset( texels, 0, buffer_size );

		// Return a temporary buffer to use
		dst.pSurface = texels;
		dst.Width    = texture_info.GetWidth();
		dst.Height   = texture_info.GetHeight();
		dst.Pitch    = pitch;
		dst.Palette  = palette;

		//Do nothing if palette address is NULL or close to NULL in a palette texture //Corn
		//Loading a SaveState (OOT -> SSV) dont bring back our TMEM data which causes issues for the first rendered frame.
		//Checking if the palette pointer is less than 0x1000 (rather than just NULL) fixes it.
		if( palette && (texture_info.GetTlutAddress() < 0x1000) ) return false;

		const ConvertFunction fn( gConvertFunctions[ (texture_info.GetFormat() << 2) | texture_info.GetSize() ] );
		if( fn )
		{
			fn( dst, texture_info );

			*p_texels = texels;
			*p_palette = palette;
			return true;
		}

		return false;
	}

	// This is intended for use with swizzled and unswizzled textures, the
	// assumption being that 2 and 4 byte pixels are swizzled around in
	// such a way that their bytes remain in the same order in memory.
	template< typename T >
	void RecolourTexture( void * p_data, u32 width, u32 height, u32 stride, c32 c )
	{
		u8		r = c.GetR();
		u8		g = c.GetG();
		u8		b = c.GetB();

		T *		data = reinterpret_cast< T * >( p_data );

		for( u32 y = 0; y < height; ++y )
		{
			for( u32 x = 0; x < width; ++x )
			{
				data[x] = T( r, g, b, data[x].GetA() );
			}

			data = AddByteOffset( data, stride );
		}
	}

	template< typename T >
	void RecolourPalette( void * p_data, u32 num_entries, c32 c )
	{
		u8		r = c.GetR();
		u8		g = c.GetG();
		u8		b = c.GetB();

		T *		data = reinterpret_cast< T * >( p_data );

		for( u32 x = 0; x < num_entries; ++x )
		{
			data[x] = T( r, g, b, data[x].GetA() );
		}
	}

	void	Recolour( void * data, void * palette, u32 width, u32 height, u32 stride, ETextureFormat texture_format, c32 colour )
	{
		switch( texture_format )
		{
		case TexFmt_5650:		RecolourTexture< NativePf5650 >( data, width, height, stride, colour );	return;
		case TexFmt_5551:		RecolourTexture< NativePf5551 >( data, width, height, stride, colour );	return;
		case TexFmt_4444:		RecolourTexture< NativePf4444 >( data, width, height, stride, colour );	return;
		case TexFmt_8888:		RecolourTexture< NativePf8888 >( data, width, height, stride, colour );	return;
		case TexFmt_CI4_8888:	RecolourPalette< NativePf8888 >( palette, 16, colour );					return;
		case TexFmt_CI8_8888:	RecolourPalette< NativePf8888 >( palette, 256, colour );				return;
		}
		DAEDALUS_ERROR( "Unhandled texture format" );
	}

	template< typename T >
	void ClampTexels( void * texels, u32 n64_width, u32 n64_height, u32 native_width, u32 native_height, u32 native_stride )
	{
		DAEDALUS_ASSERT( native_stride >= native_width * sizeof( T ), "Native stride isn't big enough" );
		DAEDALUS_ASSERT( n64_width <= native_width, "n64 width greater than native width?" );
		DAEDALUS_ASSERT( n64_height <= native_height, "n64 height greater than native height?" );

		T * data = reinterpret_cast< T * >( texels );

		//
		//	If any of the rows are short, we need to duplicate the last pixel on the row
		//	Stick this in an outer predicate incase they match
		//
		if( native_width > n64_width )
		{
			for( u32 y = 0; y < n64_height; ++y )
			{
				T	colour( data[ n64_width - 1 ] );

				for( u32 x = n64_width; x < native_width; ++x )
				{
					data[ x ] = colour;
				}

				data = AddByteOffset( data, native_stride );
			}
		}
		else
		{
			data = AddByteOffset( data, n64_height * native_stride );
		}

		//
		//	At this point all the rows up to the n64 height have been padded out.
		//	We need to duplicate the last row for every additional native row.
		//
		if( native_height > n64_height )
		{
			const void * last_row = AddByteOffset( texels, ( n64_height - 1 ) * native_stride );

			for( u32 y = n64_height; y < native_height; ++y )
			{
				memcpy( data, last_row, native_stride );

				data = AddByteOffset( data, native_stride );
			}
		}
	}

	template<>
	void ClampTexels< NativePfCI44 >( void * texels, u32 n64_width, u32 n64_height, u32 native_width, u32 native_height, u32 native_stride )
	{
		NativePfCI44  * data = reinterpret_cast<  NativePfCI44  * >( texels );

		//
		//	If any of the rows are short, we need to duplicate the last pixel on the row
		//	Stick this in an outer predicate incase they match
		//
		if( native_width > n64_width )
		{
			for( u32 y = 0; y < n64_height; ++y )
			{
				NativePfCI44	colour0( data[ (n64_width - 1)] );
				u8				colour;

				if (n64_width & 1)
				{
					// even
					colour = colour0.GetIdxB();
				}
				else
				{
					colour = colour0.GetIdxA();
				}

				for( u32 x = n64_width; x < native_width; ++x )
				{
					if (x & 1)
						data[ x >> 1 ].SetIdxB(colour);
					else
						data[ x >> 1 ].SetIdxA(colour);
				}

				data = AddByteOffset( data, native_stride );
			}
		}
		else
		{
			data = AddByteOffset( data, n64_height * native_stride );
		}

		//
		//	At this point all the rows up to the n64 height have been padded out.
		//	We need to duplicate the last row for every additional native row.
		//
		if( native_height > n64_height )
		{
			const void * last_row = AddByteOffset( texels, ( n64_height - 1 ) * native_stride);

			for( u32 y = n64_height; y < native_height; ++y )
			{
				memcpy( data, last_row, native_stride );

				data = AddByteOffset( data, native_stride );
			}
		}
	}

	void ClampTexels( void * texels, u32 n64_width, u32 n64_height, u32 native_width, u32 native_height, u32 native_stride, ETextureFormat texture_format )
	{
		switch( texture_format )
		{
		case TexFmt_5650:		ClampTexels< NativePf5650 >( texels, n64_width, n64_height, native_width, native_height, native_stride ); return;
		case TexFmt_5551:		ClampTexels< NativePf5551 >( texels, n64_width, n64_height, native_width, native_height, native_stride ); return;
		case TexFmt_4444:		ClampTexels< NativePf4444 >( texels, n64_width, n64_height, native_width, native_height, native_stride ); return;
		case TexFmt_8888:		ClampTexels< NativePf8888 >( texels, n64_width, n64_height, native_width, native_height, native_stride ); return;
		case TexFmt_CI4_8888:	ClampTexels< NativePfCI44 >( texels, n64_width, n64_height, native_width, native_height, native_stride ); return;
		case TexFmt_CI8_8888:	ClampTexels< NativePfCI8 > ( texels, n64_width, n64_height, native_width, native_height, native_stride ); return;
		}
		DAEDALUS_ERROR( "Unhandled texture format" );
	}


	template< typename T >
	void CopyRow( T * dst, const T * src, u32 pixels )
	{
		memcpy( dst, src, pixels * sizeof( T ) );
	}

	template<>
	void CopyRow( NativePfCI44 * dst, const NativePfCI44 * src, u32 pixels )
	{
		for( u32 i = 0; i+1 < pixels; i += 2 )
		{
			dst[ i/2 ] = src[ i/2 ];
		}

		// Handle odd pixel..
		if( pixels & 1 )
		{
			u8	s = src[ pixels / 2 ].Bits;

			dst[ pixels/2 ].Bits &=     ~NativePfCI44::MaskPixelA;
			dst[ pixels/2 ].Bits |= (s & NativePfCI44::MaskPixelA );
		}
	}

	template< typename T >
	void CopyRowReverse( T * dst, const T * src, u32 pixels )
	{
		u32 last_pixel = pixels * 2 - 1;

		for( u32 i = 0; i < pixels; ++i )
		{
			dst[ last_pixel - i ] = src[ i ];
		}
	}

	template<>
	void CopyRowReverse( NativePfCI44 * dst, const NativePfCI44 * src, u32 pixels )
	{
		if( pixels & 1 )
		{
			// Odd
			DAEDALUS_ERROR( "MirrorS unsupported for odd-width CI4 textures" );
		}
		else
		{
			// Even number of pixels

			const u32	first_pair_idx( 0 );
			const u32	last_pair_idx( pixels * 2 - 2 );

			for( u32 i = 0; i < pixels; i += 2 )
			{
				u8		s( src[ (first_pair_idx + i) / 2 ].Bits );
				u8		d( (s>>4) | (s<<4) );		// Swap

				dst[ (last_pair_idx - i) / 2 ].Bits = d;
			}

		}
	}


	// Assumes width p_dst = 2*width p_src and height p_dst = 2*height p_src
	template< typename T, bool MirrorS, bool MirrorT >
	void	MirrorTexelsST( void * dst, u32 dst_stride, const void * src, u32 src_stride, u32 width, u32 height )
	{
		T *			p_dst = reinterpret_cast< T * >( dst );
		const T *	p_src = reinterpret_cast< const T * >( src );

		for( u32 y = 0; y < height; ++y )
		{
			// Copy regular pixels
			CopyRow< T >( p_dst, p_src, width );

			if( MirrorS )
			{
				CopyRowReverse< T >( p_dst, p_src, width );
			}

			p_dst = AddByteOffset< T >( p_dst, dst_stride );
			p_src = AddByteOffset< T >( p_src, src_stride );
		}

		if( MirrorT )
		{
			// Copy remaining rows in reverse order
			for( u32 y = 0; y < height; ++y )
			{
				p_src = AddByteOffset( p_src, -s32(src_stride) );

				// Copy regular pixels
				CopyRow< T >( p_dst, p_src, width );

				if( MirrorS )
				{
					CopyRowReverse< T >( p_dst, p_src, width );
				}

				p_dst = AddByteOffset< T >( p_dst, dst_stride );
			}
		}
	}

	template< bool MirrorS, bool MirrorT >
	void	MirrorTexels( void * dst, u32 dst_stride, const void * src, u32 src_stride, ETextureFormat tex_fmt, u32 width, u32 height )
	{
		bool handled = false;

		switch(tex_fmt)
		{
		case TexFmt_5650:		MirrorTexelsST< NativePf5650, MirrorS, MirrorT >( dst, dst_stride, src, src_stride, width, height ); handled = true; break;
		case TexFmt_5551:		MirrorTexelsST< NativePf5551, MirrorS, MirrorT >( dst, dst_stride, src, src_stride, width, height ); handled = true; break;
		case TexFmt_4444:		MirrorTexelsST< NativePf4444, MirrorS, MirrorT >( dst, dst_stride, src, src_stride, width, height ); handled = true; break;
		case TexFmt_8888:		MirrorTexelsST< NativePf8888, MirrorS, MirrorT >( dst, dst_stride, src, src_stride, width, height ); handled = true; break;

		case TexFmt_CI4_8888:	MirrorTexelsST< NativePfCI44, MirrorS, MirrorT >( dst, dst_stride, src, src_stride, width, height ); handled = true; break;
		case TexFmt_CI8_8888:	MirrorTexelsST< NativePfCI8 , MirrorS, MirrorT >( dst, dst_stride, src, src_stride, width, height ); handled = true; break;
		}

		DAEDALUS_ASSERT( handled, "Unhandled format" ); use(handled);
	}
}


#define DEFTEX	TexFmt_8888

static const ETextureFormat TFmt[ 32 ] =
{
//	4bpp				8bpp				16bpp				32bpp
	DEFTEX,				DEFTEX,				TexFmt_5551,		TexFmt_8888,		// RGBA
	DEFTEX,				DEFTEX,				DEFTEX,				DEFTEX,				// YUV
	TexFmt_CI4_8888,	TexFmt_CI8_8888,	DEFTEX,				DEFTEX,				// CI
	TexFmt_4444,		TexFmt_4444,		TexFmt_8888,		DEFTEX,				// IA
	TexFmt_4444,		TexFmt_8888,		DEFTEX,				DEFTEX,				// I
	DEFTEX,				DEFTEX,				DEFTEX,				DEFTEX,				// ?
	DEFTEX,				DEFTEX,				DEFTEX,				DEFTEX,				// ?
	DEFTEX,				DEFTEX,				DEFTEX,				DEFTEX				// ?
};

static const ETextureFormat TFmt_hack[ 32 ] =
{
//	4bpp				8bpp				16bpp				32bpp
	DEFTEX,				DEFTEX,				TexFmt_4444,		TexFmt_8888,		// RGBA
	DEFTEX,				DEFTEX,				DEFTEX,				DEFTEX,				// YUV
	TexFmt_CI4_8888,	TexFmt_CI8_8888,	DEFTEX,				DEFTEX,				// CI
	TexFmt_4444,		TexFmt_4444,		TexFmt_8888,		DEFTEX,				// IA
	TexFmt_4444,		TexFmt_4444,		DEFTEX,				DEFTEX,				// I
	DEFTEX,				DEFTEX,				DEFTEX,				DEFTEX,				// ?
	DEFTEX,				DEFTEX,				DEFTEX,				DEFTEX,				// ?
	DEFTEX,				DEFTEX,				DEFTEX,				DEFTEX				// ?
};

static ETextureFormat SelectNativeFormat(const TextureInfo & ti)
{
	u32 idx = (ti.GetFormat() << 2) | ti.GetSize();
	return g_ROM.LOAD_T1_HACK ? TFmt_hack[idx] : TFmt[idx];
}


static void UpdateTexture( const TextureInfo & texture_info, CNativeTexture * texture, const c32 * recolour )
{
	DAEDALUS_PROFILE( "Texture Conversion" );

	DAEDALUS_ASSERT( texture != NULL, "No texture" );

	if ( texture != NULL && texture->HasData() )
	{
		void *	texels;
		void *	palette;

		if( GenerateTexels( &texels, &palette, texture_info, texture->GetFormat(), texture->GetStride(), texture->GetBytesRequired() ) )
		{
			//
			//	Recolour the texels
			//
			if( recolour )
			{
				Recolour( texels, palette, texture_info.GetWidth(), texture_info.GetHeight(), texture->GetStride(), texture->GetFormat(), *recolour );
			}

			//
			//	Clamp edges. We do this so that non power-of-2 textures whose whose width/height
			//	is less than the mask value clamp correctly. It still doesn't fix those
			//	textures with a width which is greater than the power-of-2 size.
			//
			ClampTexels( texels, texture_info.GetWidth(), texture_info.GetHeight(), texture->GetCorrectedWidth(), texture->GetCorrectedHeight(), texture->GetStride(), texture->GetFormat() );

			//
			//	Mirror the texels if required (in-place)
			//
			if( texture_info.GetMirrorS() || texture_info.GetMirrorT() )
			{
				if( texture_info.GetMirrorS() && texture_info.GetMirrorT() )
				{
					MirrorTexels< true, true >( texels, texture->GetStride(), texels, texture->GetStride(), texture->GetFormat(), texture_info.GetWidth(), texture_info.GetHeight() );
				}
				else if( texture_info.GetMirrorS() )
				{
					MirrorTexels< true, false >( texels, texture->GetStride(), texels, texture->GetStride(), texture->GetFormat(), texture_info.GetWidth(), texture_info.GetHeight() );
				}
				else if( texture_info.GetMirrorT() )
				{
					MirrorTexels< false, true >( texels, texture->GetStride(), texels, texture->GetStride(), texture->GetFormat(), texture_info.GetWidth(), texture_info.GetHeight() );
				}
				else
				{
					DAEDALUS_ERROR( "Logic error" );
				}
			}

			texture->SetData( texels, palette );
		}
	}
}

CRefPtr<CachedTexture> CachedTexture::Create( const TextureInfo & ti )
{
	if( ti.GetWidth() == 0 || ti.GetHeight() == 0 )
	{
		DAEDALUS_ERROR( "Trying to create 0 width/height texture" );
		return NULL;
	}

	CRefPtr<CachedTexture>	texture( new CachedTexture( ti ) );

	if (!texture->Initialise())
	{
		texture = NULL;
	}

	return texture;
}

CachedTexture::CachedTexture( const TextureInfo & ti )
:	mTextureInfo( ti )
,	mpTexture(NULL)
,	mpWhiteTexture(NULL)
,	mTextureContentsHash( ti.GenerateHashValue() )
,	mFrameLastUpToDate( gRDPFrame )
,	mFrameLastUsed( gRDPFrame )
{
}

CachedTexture::~CachedTexture()
{
}

bool CachedTexture::Initialise()
{
	DAEDALUS_ASSERT_Q(mpTexture == NULL);

	u32 width  = mTextureInfo.GetWidth();
	u32 height = mTextureInfo.GetHeight();

	if (mTextureInfo.GetMirrorS()) width  *= 2;
	if (mTextureInfo.GetMirrorT()) height *= 2;

	mpTexture = CNativeTexture::Create( width, height, SelectNativeFormat(mTextureInfo) );
	if( mpTexture != NULL )
	{
		// If this we're performing Texture updated checks, randomly offset the
		// 'FrameLastUpToDate' time. This ensures when lots of textures are
		// created on the same frame we update them over a nice distribution of frames.

		if(gCheckTextureHashFrequency > 0)
		{
			mFrameLastUpToDate += FastRand() & (gCheckTextureHashFrequency - 1);
		}
		UpdateTexture( mTextureInfo, mpTexture, NULL );
	}

	return mpTexture != NULL;
}

void CachedTexture::UpdateIfNecessary()
{
	if( !IsFresh() )
	{
		//
		// Generate and check the current crc
		//
		u32 hash_value( mTextureInfo.GenerateHashValue() );

		if (mTextureContentsHash != hash_value)
		{
			UpdateTexture( mTextureInfo, mpTexture, NULL );
			mTextureContentsHash = hash_value;
		}

		//
		//	One way or another, this is up to date now
		//
		mFrameLastUpToDate = gRDPFrame;
	}

	mFrameLastUsed = gRDPFrame;
}

bool CachedTexture::IsFresh() const
{
	return (gRDPFrame == mFrameLastUsed ||
			gCheckTextureHashFrequency == 0 ||
			gRDPFrame < mFrameLastUpToDate + gCheckTextureHashFrequency);
}

bool CachedTexture::HasExpired() const
{
	if(!IsFresh())
	{
		//Hack to make WONDER PROJECT J2 work (need to reload some textures every frame!) //Corn
		if( (g_ROM.GameHacks == WONDER_PROJECTJ2) && (mTextureInfo.GetTLutFormat() == kTT_RGBA16) && (mTextureInfo.GetSize() == G_IM_SIZ_8b) ) return true;

		//Hack for Worms Armageddon
		if( (g_ROM.GameHacks == WORMS_ARMAGEDDON) && (mTextureInfo.GetSize() == G_IM_SIZ_8b) && (mTextureContentsHash != mTextureInfo.GenerateHashValue()) ) return true;

		//Hack for Zelda OOT & MM text (only needed if there is not a general hash check) //Corn
		if( g_ROM.ZELDA_HACK && (mTextureInfo.GetSize() == G_IM_SIZ_4b) && mTextureContentsHash != mTextureInfo.GenerateHashValue() ) return true;

		//Check if texture has changed
		//if( mTextureContentsHash != mTextureInfo.GenerateHashValue() ) return true;
	}

	//Otherwise we wait 20+random(0-3) frames before trashing the texture if unused
	//Spread trashing them over time so not all get killed at once (lower value uses less VRAM) //Corn
	return gRDPFrame - mFrameLastUsed > (20 + (FastRand() & 0x3));
}

const CRefPtr<CNativeTexture> &	CachedTexture::GetWhiteTexture() const
{
	if(mpWhiteTexture == NULL)
	{
		DAEDALUS_ASSERT( mpTexture != NULL, "There is no existing texture" );

		CRefPtr<CNativeTexture>	new_texture( CNativeTexture::Create( mpTexture->GetWidth(), mpTexture->GetHeight(), mpTexture->GetFormat() ) );
		if( new_texture && new_texture->HasData() )
		{
			UpdateTexture( mTextureInfo, new_texture, &c32::White );
		}
		mpWhiteTexture = new_texture;
	}

	return mpWhiteTexture;
}

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
void CachedTexture::DumpTexture() const
{
	if( mpTexture != NULL && mpTexture->HasData() )
	{
		char filename[MAX_PATH+1];
		char filepath[MAX_PATH+1];
		char dumpdir[MAX_PATH+1];

		IO::Path::Combine( dumpdir, g_ROM.settings.GameName.c_str(), "Textures" );

		Dump_GetDumpDirectory( filepath, dumpdir );

		sprintf( filename, "%08x-%s_%dbpp-%dx%d-%dx%d.png",
							mTextureInfo.GetLoadAddress(), mTextureInfo.GetFormatName(), mTextureInfo.GetSizeInBits(),
							0, 0,		// Left/Top
							mTextureInfo.GetWidth(), mTextureInfo.GetHeight() );

		IO::Path::Append( filepath, filename );

		void *	texels;
		void *	palette;

		// Note that we re-convert the texels because those in the native texture may well already
		// be swizzle. Maybe we should just have an unswizzle routine?
		if( GenerateTexels( &texels, &palette, mTextureInfo, mpTexture->GetFormat(), mpTexture->GetStride(), mpTexture->GetBytesRequired() ) )
		{
			// NB - this does not include the mirrored texels

			// NB we use the palette from the native texture. This is a total hack.
			// We have to do this because the palette texels come from emulated tmem, rather
			// than ram. This means that when we dump out the texture here, tmem won't necessarily
			// contain our pixels.
			#ifdef DAEDALUS_PSP
			const void * native_palette = mpTexture->GetPalette();
			#else
			const void * native_palette = NULL;
			#endif

			PngSaveImage( filepath, texels, native_palette, mpTexture->GetFormat(), mpTexture->GetStride(), mTextureInfo.GetWidth(), mTextureInfo.GetHeight(), true );
		}
	}
}
#endif // DAEDALUS_DEBUG_DISPLAYLIST