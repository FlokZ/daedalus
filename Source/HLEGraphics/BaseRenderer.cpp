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

#include "BaseRenderer.h"
#include "TextureCache.h"
#include "RDPStateManager.h"
#include "DLDebug.h"

#include "Graphics/NativeTexture.h"
#include "Graphics/GraphicsContext.h"

#include "Math/MathUtil.h"

#include "Debug/Dump.h"
#include "Debug/DBGConsole.h"

#include "Core/Memory.h"		// We access the memory buffers
#include "Core/ROM.h"

#include "OSHLE/ultra_gbi.h"

#include "Math/Math.h"			// VFPU Math
#include "Math/MathUtil.h"

#include "Utility/Profiler.h"
#include "Utility/AuxFunc.h"

#include <pspgu.h>
#include <vector>

#include "PushStructPack1.h"

#include "PopStructPack.h"

// Vertex allocation.
// AllocVerts/FreeVerts:
//   Allocate vertices whose lifetime must extend beyond the current scope.
//   On OSX we just use malloc, though we could use a scratch allocator to simplify.
//   On PSP we again use sceGuGetMemory.
struct TempVerts
{
	TempVerts()
	:	Verts(NULL)
	,	Count(0)
	{
	}

	~TempVerts()
	{
#ifdef DAEDALUS_GL
		free(Verts);
#endif
	}

	DaedalusVtx * Alloc(u32 count)
	{
		u32 bytes = count * sizeof(DaedalusVtx);
#ifdef DAEDALUS_PSP
		Verts = static_cast<DaedalusVtx*>(sceGuGetMemory(bytes));
#endif
#ifdef DAEDALUS_GL
		Verts = static_cast<DaedalusVtx*>(malloc(bytes));
#endif

		Count = count;
		return Verts;
	}

	DaedalusVtx *	Verts;
	u32				Count;
};




extern "C"
{
void	_TnLVFPU( const Matrix4x4 * world_matrix, const Matrix4x4 * projection_matrix, const FiddledVtx * p_in, const DaedalusVtx4 * p_out, u32 num_vertices, const TnLParams * params );
void	_TnLVFPUDKR( u32 num_vertices, const Matrix4x4 * projection_matrix, const FiddledVtx * p_in, const DaedalusVtx4 * p_out );
void	_TnLVFPUDKRB( u32 num_vertices, const Matrix4x4 * projection_matrix, const FiddledVtx * p_in, const DaedalusVtx4 * p_out );
void	_TnLVFPUCBFD( const Matrix4x4 * world_matrix, const Matrix4x4 * projection_matrix, const FiddledVtx * p_in, const DaedalusVtx4 * p_out, u32 num_vertices, const TnLParams * params, const s8 * model_norm, u32 v0 );
void	_TnLVFPUPD( const Matrix4x4 * world_matrix, const Matrix4x4 * projection_matrix, const FiddledVtxPD * p_in, const DaedalusVtx4 * p_out, u32 num_vertices, const TnLParams * params, const u8 * model_norm );

void	_ConvertVertice( DaedalusVtx * dest, const DaedalusVtx4 * source );
void	_ConvertVerticesIndexed( DaedalusVtx * dest, const DaedalusVtx4 * source, u32 num_vertices, const u16 * indices );

u32		_ClipToHyperPlane( DaedalusVtx4 * dest, const DaedalusVtx4 * source, const v4 * plane, u32 num_verts );
}

#define GL_TRUE                           1
#define GL_FALSE                          0

#undef min
#undef max

extern bool gRumblePakActive;
extern u32 gAuxAddr;

static f32 fViWidth = 320.0f;
static f32 fViHeight = 240.0f;
u32 uViWidth = 320;
u32 uViHeight = 240;

f32 gZoomX=1.0;	//Default is 1.0f

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
// General purpose variable used for debugging
f32 TEST_VARX = 0.0f;
f32 TEST_VARY = 0.0f;
#endif


const float kShiftScales[] = {
	(float)(1 << 0),
	(float)(1 << 1),
	(float)(1 << 2),
	(float)(1 << 3),
	(float)(1 << 4),
	(float)(1 << 5),
	(float)(1 << 6),
	(float)(1 << 7),
	(float)(1 << 8),
	(float)(1 << 9),
	(float)(1 << 10),
	1.f / (float)(1 << 5),
	1.f / (float)(1 << 4),
	1.f / (float)(1 << 3),
	1.f / (float)(1 << 2),
	1.f / (float)(1 << 1),
};
DAEDALUS_STATIC_ASSERT(ARRAYSIZE(kShiftScales) == 16);

//*****************************************************************************
//
//*****************************************************************************
BaseRenderer::BaseRenderer()
:	mN64ToScreenScale( 2.0f, 2.0f )
,	mN64ToScreenTranslate( 0.0f, 0.0f )
,	mMux( 0 )

,	mTextureTile(0)

,	mAlphaThreshold(0)

,	mPrimDepth( 0.0f )
,	mPrimLODFraction( 0.f )

,	mFogColour(0x00FFFFFF)

,	mModelViewTop(0)
,	mWorldProjectValid(false)
,	mReloadProj(true)
,	mWPmodified(false)

,	mScreenWidth(0.f)
,	mScreenHeight(0.f)

,	mNumIndices(0)
,	mVtxClipFlagsUnion( 0 )

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
,	mNumTrisRendered( 0 )
,	mNumTrisClipped( 0 )
,	mNumRect( 0 )
,	mNastyTexture(false)
#endif
{
	DAEDALUS_ASSERT( IsPointerAligned( &mTnL, 16 ), "Oops, mTnL should be 16-byte aligned" );

	for ( u32 i = 0; i < kNumBoundTextures; i++ )
	{
		mTileTopLeft[i].x = 0.0f;
		mTileTopLeft[i].y = 0.0f;
		mTexWrap[i].u = 0;
		mTexWrap[i].v = 0;
		mTexShift[i].s = 0;
		mTexShift[i].t = 0;
	}

	mTnL.Flags._u32 = 0;
	mTnL.NumLights = 0;
	mTnL.TextureScaleX = 1.0f;
	mTnL.TextureScaleY = 1.0f;

	memset( mTnL.Lights, 0, sizeof(mTnL.Lights) );
}

//*****************************************************************************
//
//*****************************************************************************
BaseRenderer::~BaseRenderer()
{
}

//*****************************************************************************
//
//*****************************************************************************
void BaseRenderer::SetVIScales()
{
	u32 width = Memory_VI_GetRegister( VI_WIDTH_REG );

	u32 ScaleX = Memory_VI_GetRegister( VI_X_SCALE_REG ) & 0xFFF;
	u32 ScaleY = Memory_VI_GetRegister( VI_Y_SCALE_REG ) & 0xFFF;

	f32 fScaleX = (f32)ScaleX / 1024.0f;
	f32 fScaleY = (f32)ScaleY / 2048.0f;

	u32 HStartReg = Memory_VI_GetRegister( VI_H_START_REG );
	u32 VStartReg = Memory_VI_GetRegister( VI_V_START_REG );

	u32	hstart = HStartReg >> 16;
	u32	hend = HStartReg & 0xffff;

	u32	vstart = VStartReg >> 16;
	u32	vend = VStartReg & 0xffff;

	// Sometimes HStartReg can be zero.. ex PD, Lode Runner, Cyber Tiger
	if (hend == hstart)
	{
		hend = (u32)(width / fScaleX);
	}

	fViWidth  =  (hend-hstart)    * fScaleX;
	fViHeight =  (vend-vstart)    * fScaleY * 1.0126582f;

	// XXX Need to check PAL games.
	//if(g_ROM.TvType != OS_TV_NTSC) sRatio = 9/11.0f;

	//This corrects height in various games ex : Megaman 64, CyberTiger
	if( width > 0x300 )
	{
		fViHeight *= 2.0f;
	}

	//Used to set a limit on Scissors //Corn
	uViWidth  = (u32)fViWidth - 1;
	uViHeight = (u32)fViHeight - 1;
}

//*****************************************************************************
// Reset for a new frame
//*****************************************************************************
void BaseRenderer::Reset()
{
	mNumIndices = 0;
	mVtxClipFlagsUnion = 0;

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
	mNumTrisRendered = 0;
	mNumTrisClipped = 0;
	mNumRect = 0;
#endif

}

//*****************************************************************************
//
//*****************************************************************************
void BaseRenderer::BeginScene()
{
	CGraphicsContext::Get()->BeginFrame();

	RestoreRenderStates();

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
	ResetDebugState();
#endif

	InitViewport();
}

//*****************************************************************************
//
//*****************************************************************************
void BaseRenderer::EndScene()
{
	CGraphicsContext::Get()->EndFrame();

	//
	//	Clear this, to ensure we're force to check for updates to it on the next frame
	for( u32 i = 0; i < kNumBoundTextures; i++ )
	{
		mBoundTextureInfo[ i ] = TextureInfo();
		mBoundTexture[ i ]     = NULL;
	}
}

//*****************************************************************************
//
//*****************************************************************************
void BaseRenderer::InitViewport()
{
	// Init the N64 viewport.
	mVpScale = v2( 640.f*0.25f, 480.f*0.25f );
	mVpTrans = v2( 640.f*0.25f, 480.f*0.25f );

	// Get the current display dimensions. This might change frame by frame e.g. if the window is resized.
	u32 display_width  = 0;
	u32 display_height = 0;
	CGraphicsContext::Get()->ViewportType(&display_width, &display_height);

	DAEDALUS_ASSERT( display_width && display_height, "Unhandled viewport type" );

	mScreenWidth  = (f32)display_width;
	mScreenHeight = (f32)display_height;

#ifdef DAEDALUS_PSP
	// Centralise the viewport in the display.
	u32 frame_width  = gGlobalPreferences.TVEnable ? 720 : 480;
	u32 frame_height = gGlobalPreferences.TVEnable ? 480 : 272;

	s32 display_x = (s32)(frame_width  - display_width)  / 2;
	s32 display_y = (s32)(frame_height - display_height) / 2;
#else
	s32 display_x = 0;
	s32 display_y = 0;
#endif

	mN64ToScreenScale.x = gZoomX * mScreenWidth  / fViWidth;
	mN64ToScreenScale.y = gZoomX * mScreenHeight / fViHeight;

	mN64ToScreenTranslate.x  = (f32)display_x - Round(0.55f * (gZoomX - 1.0f) * fViWidth);
	mN64ToScreenTranslate.y  = (f32)display_y - Round(0.55f * (gZoomX - 1.0f) * fViHeight);

	if( gRumblePakActive )
	{
	    mN64ToScreenTranslate.x += (FastRand() & 3);
		mN64ToScreenTranslate.y += (FastRand() & 3);
	}

#if defined(DAEDALUS_GL)
	f32 w = mScreenWidth;
	f32 h = mScreenHeight;

	mScreenToDevice = Matrix4x4(
		2.f / w,       0.f,     0.f,     0.f,
		    0.f,  -2.f / h,     0.f,     0.f,
		    0.f,       0.f,     1.f,     0.f,
		  -1.0f,       1.f,     0.f,     1.f
	);
#endif

	UpdateViewport();
}

//*****************************************************************************
//
//*****************************************************************************
void BaseRenderer::SetN64Viewport( const v2 & scale, const v2 & trans )
{
	// Only Update viewport when it actually changed, this happens rarely
	//
	if( mVpScale.x == scale.x && mVpScale.y == scale.y &&
		mVpTrans.x == trans.x && mVpTrans.y == trans.y )
		return;

	mVpScale.x = scale.x;
	mVpScale.y = scale.y;

	mVpTrans.x = trans.x;
	mVpTrans.y = trans.y;

	UpdateViewport();
}

//*****************************************************************************
//
//*****************************************************************************
void BaseRenderer::UpdateViewport()
{
	v2		n64_min( mVpTrans.x - mVpScale.x, mVpTrans.y - mVpScale.y );
	v2		n64_max( mVpTrans.x + mVpScale.x, mVpTrans.y + mVpScale.y );

	v2		psp_min;
	v2		psp_max;
	ConvertN64ToScreen( n64_min, psp_min );
	ConvertN64ToScreen( n64_max, psp_max );

	s32		vp_x = s32( psp_min.x );
	s32		vp_y = s32( psp_min.y );
	s32		vp_w = s32( psp_max.x - psp_min.x );
	s32		vp_h = s32( psp_max.y - psp_min.y );

	//DBGConsole_Msg(0, "[WViewport Changed (%d) (%d)]",vp_w,vp_h );

#if defined(DAEDALUS_PSP)
	const u32 vx = 2048;
	const u32 vy = 2048;

	sceGuOffset(vx - (vp_w/2),vy - (vp_h/2));
	sceGuViewport(vx + vp_x, vy + vp_y, vp_w, vp_h);
#elif defined(DAEDALUS_GL)
	glViewport(vp_x, mScreenHeight - (vp_h + vp_y), vp_w, vp_h);
#else
	DAEDALUS_ERROR("Code to set viewport not implemented on this platform");
#endif
}

//*****************************************************************************
// Returns true if triangle visible and rendered, false otherwise
//*****************************************************************************
bool BaseRenderer::AddTri(u32 v0, u32 v1, u32 v2)
{
	//DAEDALUS_PROFILE( "BaseRenderer::AddTri" );

	DAEDALUS_ASSERT( v0 < kMaxN64Vertices, "Vertex index is out of bounds (%d)", v0 );
	DAEDALUS_ASSERT( v1 < kMaxN64Vertices, "Vertex index is out of bounds (%d)", v1 );
	DAEDALUS_ASSERT( v2 < kMaxN64Vertices, "Vertex index is out of bounds (%d)", v2 );

	const u32 & f0 = mVtxProjected[v0].ClipFlags;
	const u32 & f1 = mVtxProjected[v1].ClipFlags;
	const u32 & f2 = mVtxProjected[v2].ClipFlags;

	if ( f0 & f1 & f2 )
	{
#ifdef DAEDALUS_DEBUG_DISPLAYLIST
		DL_PF("    Tri: %d,%d,%d (Culled -> NDC box)", v0, v1, v2);
		++mNumTrisClipped;
#endif
		return false;
	}

	//
	//Cull BACK or FRONT faceing tris early in the pipeline //Corn
	//
	if( mTnL.Flags.TriCull )
	{
		const v4 & A = mVtxProjected[v0].ProjectedPos;
		const v4 & B = mVtxProjected[v1].ProjectedPos;
		const v4 & C = mVtxProjected[v2].ProjectedPos;

		//Avoid using 1/w, will use five more mults but save three divides //Corn
		//Precalc reused w combos so compiler does a proper job
		const f32 ABw  = A.w*B.w;
		const f32 ACw  = A.w*C.w;
		const f32 BCw  = B.w*C.w;
		const f32 AxBC = A.x*BCw;
		const f32 AyBC = A.y*BCw;

		if( (((B.x*ACw - AxBC)*(C.y*ABw - AyBC) - (C.x*ABw - AxBC)*(B.y*ACw - AyBC)) * ABw * C.w) <= 0.f )
		{
			if( mTnL.Flags.CullBack )
			{
#ifdef DAEDALUS_DEBUG_DISPLAYLIST
				DL_PF("    Tri: %d,%d,%d (Culled -> Back Face)", v0, v1, v2);
				++mNumTrisClipped;
#endif
				return false;
			}
		}
		else if( !mTnL.Flags.CullBack )
		{
#ifdef DAEDALUS_DEBUG_DISPLAYLIST
			DL_PF("    Tri: %d,%d,%d (Culled -> Front Face)", v0, v1, v2);
			++mNumTrisClipped;
#endif
			return false;
		}
	}

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
	DL_PF("    Tri: %d,%d,%d (Rendered)", v0, v1, v2);
	++mNumTrisRendered;
#endif

	DAEDALUS_ASSERT( mNumIndices + 3 < MAX_VERTICES, " Array overflow, to many Indices" );

	m_swIndexBuffer[ mNumIndices++ ] = (u16)v0;
	m_swIndexBuffer[ mNumIndices++ ] = (u16)v1;
	m_swIndexBuffer[ mNumIndices++ ] = (u16)v2;

	mVtxClipFlagsUnion |= f0 | f1 | f2;

	return true;
}

//*****************************************************************************
//
//*****************************************************************************
void BaseRenderer::FlushTris()
{
	DAEDALUS_PROFILE( "BaseRenderer::FlushTris" );
	/*
	if ( mNumIndices == 0 )
	{
		DAEDALUS_ERROR("Call to FlushTris() with nothing to render");
		mVtxClipFlagsUnion = 0; // Reset software clipping detector
		return;
	}
	*/
	DAEDALUS_ASSERT( mNumIndices, "Call to FlushTris() with nothing to render" );

	TempVerts temp_verts;

	// If any bit is set here it means we have to clip the trianlges since PSP HW clipping sux!
	if(mVtxClipFlagsUnion != 0)
	{
		PrepareTrisClipped( &temp_verts );
	}
	else
	{
		PrepareTrisUnclipped( &temp_verts );
	}

	// No vertices to render? //Corn
	if( temp_verts.Count == 0 )
	{
		mNumIndices = 0;
		mVtxClipFlagsUnion = 0;
		return;
	}

	// Hack for Pilotwings 64
	/*static bool skipNext=false;
	if( g_ROM.GameHacks == PILOT_WINGS )
	{
		if ( (g_DI.Address == g_CI.Address) && gRDPOtherMode.z_cmp+gRDPOtherMode.z_upd > 0 )
		{
			DAEDALUS_ERROR("Warning: using Flushtris to write Zbuffer" );
			mNumIndices = 0;
			mVtxClipFlagsUnion = 0;
			skipNext = true;
			return;
		}
		else if( skipNext )
		{
			skipNext = false;
			mNumIndices = 0;
			mVtxClipFlagsUnion = 0;
			return;
		}
	}*/

	//
	// Check for depth source, this is for Nascar games, hopefully won't mess up anything
	DAEDALUS_ASSERT( !gRDPOtherMode.depth_source, " Warning : Using depth source in flushtris" );

	//
	//	Render out our vertices
	RenderTriangles( temp_verts.Verts, temp_verts.Count, gRDPOtherMode.depth_source ? true : false );

	mNumIndices = 0;
	mVtxClipFlagsUnion = 0;
}

//*****************************************************************************
//
//	The following clipping code was taken from The Irrlicht Engine.
//	See http://irrlicht.sourceforge.net/ for more information.
//	Copyright (C) 2002-2006 Nikolaus Gebhardt/Alten Thomas
//
//Croping triangles just outside the NDC box and let PSP HW do the final crop
//improves quality but fails in some games (Rocket Robot/Lego racers)//Corn
//*****************************************************************************
ALIGNED_TYPE(const v4, NDCPlane[6], 16) =
{
	v4(  0.f,  0.f, -1.f, -1.f ),	// near
	v4(  0.f,  0.f,  1.f, -1.f ),	// far
	v4(  1.f,  0.f,  0.f, -1.f ),	// left
	v4( -1.f,  0.f,  0.f, -1.f ),	// right
	v4(  0.f,  1.f,  0.f, -1.f ),	// bottom
	v4(  0.f, -1.f,  0.f, -1.f )	// top
};

//*****************************************************************************
//VFPU tris clip(fast)
//*****************************************************************************
#ifdef DAEDALUS_PSP_USE_VFPU
u32 clip_tri_to_frustum( DaedalusVtx4 * v0, DaedalusVtx4 * v1 )
{
	u32 vOut( 3 );

	vOut = _ClipToHyperPlane( v1, v0, &NDCPlane[0], vOut ); if( vOut < 3 ) return vOut;		// near
	vOut = _ClipToHyperPlane( v0, v1, &NDCPlane[1], vOut ); if( vOut < 3 ) return vOut;		// far
	vOut = _ClipToHyperPlane( v1, v0, &NDCPlane[2], vOut ); if( vOut < 3 ) return vOut;		// left
	vOut = _ClipToHyperPlane( v0, v1, &NDCPlane[3], vOut ); if( vOut < 3 ) return vOut;		// right
	vOut = _ClipToHyperPlane( v1, v0, &NDCPlane[4], vOut ); if( vOut < 3 ) return vOut;		// bottom
	vOut = _ClipToHyperPlane( v0, v1, &NDCPlane[5], vOut );									// top

	return vOut;
}

#else	// FPU/CPU(slower)

//*****************************************************************************
//CPU interpolate line parameters
//*****************************************************************************
void DaedalusVtx4::Interpolate( const DaedalusVtx4 & lhs, const DaedalusVtx4 & rhs, float factor )
{
	ProjectedPos = lhs.ProjectedPos + (rhs.ProjectedPos - lhs.ProjectedPos) * factor;
	TransformedPos = lhs.TransformedPos + (rhs.TransformedPos - lhs.TransformedPos) * factor;
	Colour = lhs.Colour + (rhs.Colour - lhs.Colour) * factor;
	Texture = lhs.Texture + (rhs.Texture - lhs.Texture) * factor;
	ClipFlags = 0;
}

//*****************************************************************************
//CPU line clip to plane
//*****************************************************************************
static u32 clipToHyperPlane( DaedalusVtx4 * dest, const DaedalusVtx4 * source, u32 inCount, const v4 &plane )
{
	u32 outCount(0);
	DaedalusVtx4 * out(dest);

	const DaedalusVtx4 * a;
	const DaedalusVtx4 * b(source);

	f32 bDotPlane = b->ProjectedPos.Dot( plane );

	for( u32 i = 1; i < inCount + 1; ++i)
	{
		//a = &source[i%inCount];
		const s32 condition = i - inCount;
		const s32 index = (( ( condition >> 31 ) & ( i ^ condition ) ) ^ condition );
		a = &source[index];

		f32 aDotPlane = a->ProjectedPos.Dot( plane );

		// current point inside
		if ( aDotPlane <= 0.f )
		{
			// last point outside
			if ( bDotPlane > 0.f )
			{
				// intersect line segment with plane
				out->Interpolate( *b, *a, bDotPlane / (b->ProjectedPos - a->ProjectedPos).Dot( plane ) );
				out++;
				outCount++;
			}
			// copy current to out
			*out = *a;
			b = out;

			out++;
			outCount++;
		}
		else
		{
			// current point outside
			if ( bDotPlane <= 0.f )
			{
				// previous was inside, intersect line segment with plane
				out->Interpolate( *b, *a, bDotPlane / (b->ProjectedPos - a->ProjectedPos).Dot( plane ) );
				out++;
				outCount++;
			}
			b = a;
		}

		bDotPlane = aDotPlane;
	}

	return outCount;
}

//*****************************************************************************
//CPU tris clip to frustum
//*****************************************************************************
u32 clip_tri_to_frustum( DaedalusVtx4 * v0, DaedalusVtx4 * v1 )
{
	u32 vOut = 3;

	vOut = clipToHyperPlane( v1, v0, vOut, NDCPlane[0] ); if ( vOut < 3 ) return vOut;		// near
	vOut = clipToHyperPlane( v0, v1, vOut, NDCPlane[1] ); if ( vOut < 3 ) return vOut;		// far
	vOut = clipToHyperPlane( v1, v0, vOut, NDCPlane[2] ); if ( vOut < 3 ) return vOut;		// left
	vOut = clipToHyperPlane( v0, v1, vOut, NDCPlane[3] ); if ( vOut < 3 ) return vOut;		// right
	vOut = clipToHyperPlane( v1, v0, vOut, NDCPlane[4] ); if ( vOut < 3 ) return vOut;		// bottom
	vOut = clipToHyperPlane( v0, v1, vOut, NDCPlane[5] );									// top

	return vOut;
}
#endif	//CPU clip

//*****************************************************************************
//
//*****************************************************************************
namespace
{
	DaedalusVtx4		temp_a[ 8 ];
	DaedalusVtx4		temp_b[ 8 ];
	// Flying Dragon clips more than 256
	const u32			MAX_CLIPPED_VERTS = 320;
	DaedalusVtx			clip_vtx[MAX_CLIPPED_VERTS];
}

//*****************************************************************************
//
//*****************************************************************************
void BaseRenderer::PrepareTrisClipped( TempVerts * temp_verts ) const
{
	DAEDALUS_PROFILE( "BaseRenderer::PrepareTrisClipped" );

	//
	//	At this point all vertices are lit/projected and have both transformed and projected
	//	vertex positions. For the best results we clip against the projected vertex positions,
	//	but use the resulting intersections to interpolate the transformed positions.
	//	The clipping is more efficient in normalised device coordinates, but rendering these
	//	directly prevents the PSP performing perspective correction. We could invert the projection
	//	matrix and use this to back-project the clip planes into world coordinates, but this
	//	suffers from various precision issues. Carrying around both sets of coordinates gives
	//	us the best of both worlds :)
	//
	//  Convert directly to PSP hardware format, that way we only copy 24 bytes instead of 64 bytes //Corn
	//
	u32 num_vertices = 0;

	for(u32 i = 0; i < (mNumIndices - 2);)
	{
		const u32 & idx0 = m_swIndexBuffer[ i++ ];
		const u32 & idx1 = m_swIndexBuffer[ i++ ];
		const u32 & idx2 = m_swIndexBuffer[ i++ ];

		//Check if any of the vertices are outside the clipbox (NDC), if so we need to clip the triangle
		if(mVtxProjected[idx0].ClipFlags | mVtxProjected[idx1].ClipFlags | mVtxProjected[idx2].ClipFlags)
		{
			temp_a[ 0 ] = mVtxProjected[ idx0 ];
			temp_a[ 1 ] = mVtxProjected[ idx1 ];
			temp_a[ 2 ] = mVtxProjected[ idx2 ];

			u32 out = clip_tri_to_frustum( temp_a, temp_b );
			//If we have less than 3 vertices left after the clipping
			//we can't make a triangle so we bail and skip rendering it.
			DL_PF("    Clip & re-tesselate [%d,%d,%d] with %d vertices", i-3, i-2, i-1, out);
			DL_PF("    %#5.3f, %#5.3f, %#5.3f", mVtxProjected[ idx0 ].ProjectedPos.x/mVtxProjected[ idx0 ].ProjectedPos.w, mVtxProjected[ idx0 ].ProjectedPos.y/mVtxProjected[ idx0 ].ProjectedPos.w, mVtxProjected[ idx0 ].ProjectedPos.z/mVtxProjected[ idx0 ].ProjectedPos.w);
			DL_PF("    %#5.3f, %#5.3f, %#5.3f", mVtxProjected[ idx1 ].ProjectedPos.x/mVtxProjected[ idx1 ].ProjectedPos.w, mVtxProjected[ idx1 ].ProjectedPos.y/mVtxProjected[ idx1 ].ProjectedPos.w, mVtxProjected[ idx1 ].ProjectedPos.z/mVtxProjected[ idx1 ].ProjectedPos.w);
			DL_PF("    %#5.3f, %#5.3f, %#5.3f", mVtxProjected[ idx2 ].ProjectedPos.x/mVtxProjected[ idx2 ].ProjectedPos.w, mVtxProjected[ idx2 ].ProjectedPos.y/mVtxProjected[ idx2 ].ProjectedPos.w, mVtxProjected[ idx2 ].ProjectedPos.z/mVtxProjected[ idx2 ].ProjectedPos.w);

			if( out < 3 )
				continue;

			// Retesselate
			u32 new_num_vertices( num_vertices + (out - 3) * 3 );
			if( new_num_vertices > MAX_CLIPPED_VERTS )
			{
				DAEDALUS_ERROR( "Too many clipped verts: %d", new_num_vertices );
				break;
			}
			//Make new triangles from the vertices we got back from clipping the original triangle
			for( u32 j = 0; j <= out - 3; ++j)
			{
#ifdef DAEDALUS_PSP_USE_VFPU
				_ConvertVertice( &clip_vtx[ num_vertices++ ], &temp_a[ 0 ]);
				_ConvertVertice( &clip_vtx[ num_vertices++ ], &temp_a[ j + 1 ]);
				_ConvertVertice( &clip_vtx[ num_vertices++ ], &temp_a[ j + 2 ]);
#else
				clip_vtx[ num_vertices ].Texture = temp_a[ 0 ].Texture;
				clip_vtx[ num_vertices ].Colour = c32( temp_a[ 0 ].Colour );
				clip_vtx[ num_vertices ].Position.x = temp_a[ 0 ].TransformedPos.x;
				clip_vtx[ num_vertices ].Position.y = temp_a[ 0 ].TransformedPos.y;
				clip_vtx[ num_vertices++ ].Position.z = temp_a[ 0 ].TransformedPos.z;

				clip_vtx[ num_vertices ].Texture = temp_a[ j + 1 ].Texture;
				clip_vtx[ num_vertices ].Colour = c32( temp_a[ j + 1 ].Colour );
				clip_vtx[ num_vertices ].Position.x = temp_a[ j + 1 ].TransformedPos.x;
				clip_vtx[ num_vertices ].Position.y = temp_a[ j + 1 ].TransformedPos.y;
				clip_vtx[ num_vertices++ ].Position.z = temp_a[ j + 1 ].TransformedPos.z;

				clip_vtx[ num_vertices ].Texture = temp_a[ j + 2 ].Texture;
				clip_vtx[ num_vertices ].Colour = c32( temp_a[ j + 2 ].Colour );
				clip_vtx[ num_vertices ].Position.x = temp_a[ j + 2 ].TransformedPos.x;
				clip_vtx[ num_vertices ].Position.y = temp_a[ j + 2 ].TransformedPos.y;
				clip_vtx[ num_vertices++ ].Position.z = temp_a[ j + 2 ].TransformedPos.z;
#endif
			}
		}
		else	//Triangle is inside the clipbox so we just add it as it is.
		{
			if( num_vertices > (MAX_CLIPPED_VERTS - 3) )
			{
				DAEDALUS_ERROR( "Too many clipped verts: %d", num_vertices + 3 );
				break;
			}

#ifdef DAEDALUS_PSP_USE_VFPU
			_ConvertVertice( &clip_vtx[ num_vertices++ ], &mVtxProjected[ idx0 ]);
			_ConvertVertice( &clip_vtx[ num_vertices++ ], &mVtxProjected[ idx1 ]);
			_ConvertVertice( &clip_vtx[ num_vertices++ ], &mVtxProjected[ idx2 ]);
#else
			clip_vtx[ num_vertices ].Texture = mVtxProjected[ idx0 ].Texture;
			clip_vtx[ num_vertices ].Colour = c32( mVtxProjected[ idx0 ].Colour );
			clip_vtx[ num_vertices ].Position.x = mVtxProjected[ idx0 ].TransformedPos.x;
			clip_vtx[ num_vertices ].Position.y = mVtxProjected[ idx0 ].TransformedPos.y;
			clip_vtx[ num_vertices++ ].Position.z = mVtxProjected[ idx0 ].TransformedPos.z;

			clip_vtx[ num_vertices ].Texture = mVtxProjected[ idx1 ].Texture;
			clip_vtx[ num_vertices ].Colour = c32( mVtxProjected[ idx1 ].Colour );
			clip_vtx[ num_vertices ].Position.x = mVtxProjected[ idx1 ].TransformedPos.x;
			clip_vtx[ num_vertices ].Position.y = mVtxProjected[ idx1 ].TransformedPos.y;
			clip_vtx[ num_vertices++ ].Position.z = mVtxProjected[ idx1 ].TransformedPos.z;

			clip_vtx[ num_vertices ].Texture = mVtxProjected[ idx2 ].Texture;
			clip_vtx[ num_vertices ].Colour = c32( mVtxProjected[ idx2 ].Colour );
			clip_vtx[ num_vertices ].Position.x = mVtxProjected[ idx2 ].TransformedPos.x;
			clip_vtx[ num_vertices ].Position.y = mVtxProjected[ idx2 ].TransformedPos.y;
			clip_vtx[ num_vertices++ ].Position.z = mVtxProjected[ idx2 ].TransformedPos.z;
#endif
		}
	}

	//
	//	Now the vertices have been clipped we need to write them into
	//	a buffer we obtain this from the display list.

	DaedalusVtx * p_vertices = temp_verts->Alloc(num_vertices);

	memcpy( p_vertices, clip_vtx, num_vertices * sizeof(DaedalusVtx) );	//std memcpy() is as fast as VFPU here!
}

//*****************************************************************************
//
//*****************************************************************************
void BaseRenderer::PrepareTrisUnclipped( TempVerts * temp_verts ) const
{
	DAEDALUS_PROFILE( "BaseRenderer::PrepareTrisUnclipped" );
	DAEDALUS_ASSERT( mNumIndices > 0, "The number of indices should have been checked" );

	const u32		num_vertices = mNumIndices;
	DaedalusVtx *	p_vertices   = temp_verts->Alloc(num_vertices);

	//
	//	Previously this code set up an index buffer to avoid processing the
	//	same vertices more than once - we avoid this now as there is apparently
	//	quite a large performance penalty associated with using these on the PSP.
	//
	//	http://forums.ps2dev.org/viewtopic.php?t=4703
	//
	//DAEDALUS_STATIC_ASSERT( MAX_CLIPPED_VERTS > ARRAYSIZE(m_swIndexBuffer) );

#ifdef DAEDALUS_PSP_USE_VFPU
	_ConvertVerticesIndexed( p_vertices, mVtxProjected, num_vertices, m_swIndexBuffer );
#else
	//
	//	Now we just shuffle all the data across directly (potentially duplicating verts)
	//
	for( u32 i = 0; i < num_vertices; ++i )
	{
		u32 index = m_swIndexBuffer[ i ];

		p_vertices[ i ].Texture = mVtxProjected[ index ].Texture;
		p_vertices[ i ].Colour = c32( mVtxProjected[ index ].Colour );
		p_vertices[ i ].Position.x = mVtxProjected[ index ].TransformedPos.x;
		p_vertices[ i ].Position.y = mVtxProjected[ index ].TransformedPos.y;
		p_vertices[ i ].Position.z = mVtxProjected[ index ].TransformedPos.z;
	}
 #endif
}

//*****************************************************************************
// Standard rendering pipeline using VFPU(fast)
//*****************************************************************************
#ifdef DAEDALUS_PSP_USE_VFPU
void BaseRenderer::SetNewVertexInfo(u32 address, u32 v0, u32 n)
{
	const FiddledVtx * const pVtxBase( (const FiddledVtx*)(g_pu8RamBase + address) );

	UpdateWorldProject();
	PokeWorldProject();

	const Matrix4x4 & mat_world_project = mWorldProject;
	const Matrix4x4 & mat_world = mModelViewStack[mModelViewTop];

	DL_PF( "    Ambient color RGB[%f][%f][%f] Texture scale X[%f] Texture scale Y[%f]", mTnL.Lights[mTnL.NumLights].Colour.x, mTnL.Lights[mTnL.NumLights].Colour.y, mTnL.Lights[mTnL.NumLights].Colour.z, mTnL.TextureScaleX, mTnL.TextureScaleY);
	DL_PF( "    Light[%s] Texture[%s] EnvMap[%s] Fog[%s]", (mTnL.Flags.Light)? "On":"Off", (mTnL.Flags.Texture)? "On":"Off", (mTnL.Flags.TexGen)? (mTnL.Flags.TexGenLin)? "Linear":"Spherical":"Off", (mTnL.Flags.Fog)? "On":"Off");

	_TnLVFPU( &mat_world, &mat_world_project, pVtxBase, &mVtxProjected[v0], n, &mTnL );
}

//*****************************************************************************
//
//*****************************************************************************
/*void BaseRenderer::TestVFPUVerts( u32 v0, u32 num, const FiddledVtx * verts, const Matrix4x4 & mat_world )
{
	bool	env_map( (mTnL.Flags._u32 & (TNL_LIGHT|TNL_TEXGEN)) == (TNL_LIGHT|TNL_TEXGEN) );

	u32 vend( v0 + num );
	for (u32 i = v0; i < vend; i++)
	{
		const FiddledVtx & vert = verts[i - v0];
		const v4 &	projected( mVtxProjected[i].ProjectedPos );

		if (mTnL.Flags.Fog)
		{
			float eyespace_z = projected.z / projected.w;
			float fog_coeff = (eyespace_z * mTnL.FogMult) + mTnL.FogOffset;

			// Set the alpha
			f32 value = Clamp< f32 >( fog_coeff, 0.0f, 1.0f );

			if( Abs( value - mVtxProjected[i].Colour.w ) > 0.01f )
			{
				printf( "Fog wrong: %f != %f\n", mVtxProjected[i].Colour.w, value );
			}
		}

		if (mTnL.Flags.Texture)
		{
			// Update texture coords n.b. need to divide tu/tv by bogus scale on addition to buffer

			// If the vert is already lit, then there is no normal (and hence we
			// can't generate tex coord)
			float tx, ty;
			if (env_map)
			{
				v3 vecTransformedNormal;		// Used only when TNL_LIGHT set
				v3	model_normal(f32( vert.norm_x ), f32( vert.norm_y ), f32( vert.norm_z ) );

				vecTransformedNormal = mat_world.TransformNormal( model_normal );
				vecTransformedNormal.Normalise();

				const v3 & norm = vecTransformedNormal;

				// Assign the spheremap's texture coordinates
				tx = (0.5f * ( 1.0f + ( norm.x*mat_world.m11 +
										norm.y*mat_world.m21 +
										norm.z*mat_world.m31 ) ));

				ty = (0.5f * ( 1.0f - ( norm.x*mat_world.m12 +
										norm.y*mat_world.m22 +
										norm.z*mat_world.m32 ) ));
			}
			else
			{
				tx = (float)vert.tu * mTnL.TextureScaleX;
				ty = (float)vert.tv * mTnL.TextureScaleY;
			}

			if( Abs(tx - mVtxProjected[i].Texture.x ) > 0.0001f ||
				Abs(ty - mVtxProjected[i].Texture.y ) > 0.0001f )
			{
				printf( "tx/y wrong : %f,%f != %f,%f (%s)\n", mVtxProjected[i].Texture.x, mVtxProjected[i].Texture.y, tx, ty, env_map ? "env" : "scale" );
			}
		}

		//
		//	Initialise the clipping flags (always done on the VFPU, so skip here)
		//
		//u32 flags = CalcClipFlags( projected );
		//if( flags != mVtxProjected[i].ClipFlags )
		//{
		//	printf( "flags wrong: %02x != %02x\n", mVtxProjected[i].ClipFlags, flags );
		//}
	}
}*/

#else	//Transform using VFPU(fast) or FPU/CPU(slow)
//*****************************************************************************
//
//*****************************************************************************
v4 BaseRenderer::LightVert( const v3 & norm ) const
{

	u32 num = mTnL.NumLights;

	v4 result( mTnL.Lights[num].Colour.x,
			   mTnL.Lights[num].Colour.y,
			   mTnL.Lights[num].Colour.z,
			   mTnL.Lights[num].Colour.w ); // 1.0f


	for ( u32 l = 0; l < num; l++ )
	{
		f32 fCosT = norm.Dot( mTnL.Lights[l].Direction );
		if (fCosT > 0.0f)
		{
			result.x += mTnL.Lights[l].Colour.x * fCosT;
			result.y += mTnL.Lights[l].Colour.y * fCosT;
			result.z += mTnL.Lights[l].Colour.z * fCosT;
		}
	}

	//Clamp to 1.0
	if( result.x > 1.0f ) result.x = 1.0f;
	if( result.y > 1.0f ) result.y = 1.0f;
	if( result.z > 1.0f ) result.z = 1.0f;
	//result.w = 1.0f;

	return result;
}

//*****************************************************************************
// Standard rendering pipeline using FPU/CPU
//*****************************************************************************
void BaseRenderer::SetNewVertexInfo(u32 address, u32 v0, u32 n)
{
	const FiddledVtx * pVtxBase = (const FiddledVtx*)(g_pu8RamBase + address);
	UpdateWorldProject();
	PokeWorldProject();

	const Matrix4x4 & mat_world_project = mWorldProject;
	const Matrix4x4 & mat_world = mModelViewStack[mModelViewTop];

	DL_PF( "    Ambient color RGB[%f][%f][%f] Texture scale X[%f] Texture scale Y[%f]", mTnL.Lights[mTnL.NumLights].Colour.x, mTnL.Lights[mTnL.NumLights].Colour.y, mTnL.Lights[mTnL.NumLights].Colour.z, mTnL.TextureScaleX, mTnL.TextureScaleY);
	DL_PF( "    Light[%s] Texture[%s] EnvMap[%s] Fog[%s]", (mTnL.Flags.Light)? "On":"Off", (mTnL.Flags.Texture)? "On":"Off", (mTnL.Flags.TexGen)? (mTnL.Flags.TexGenLin)? "Linear":"Spherical":"Off", (mTnL.Flags.Fog)? "On":"Off");

	// Transform and Project + Lighting or Transform and Project with Colour
	//
	for (u32 i = v0; i < v0 + n; i++)
	{
		const FiddledVtx & vert = pVtxBase[i - v0];

		v4 w( f32( vert.x ), f32( vert.y ), f32( vert.z ), 1.0f );

		// VTX Transform
		//
		v4 & projected( mVtxProjected[i].ProjectedPos );
		projected = mat_world_project.Transform( w );
		mVtxProjected[i].TransformedPos = mat_world.Transform( w );

		//	Initialise the clipping flags
		//
		u32 clip_flags = 0;
		if		(projected.x < -projected.w)	clip_flags |= X_POS;
		else if (projected.x > projected.w)		clip_flags |= X_NEG;

		if		(projected.y < -projected.w)	clip_flags |= Y_POS;
		else if (projected.y > projected.w)		clip_flags |= Y_NEG;

		if		(projected.z < -projected.w)	clip_flags |= Z_POS;
		else if (projected.z > projected.w)		clip_flags |= Z_NEG;
		mVtxProjected[i].ClipFlags = clip_flags;

		// LIGHTING OR COLOR
		//
		if ( mTnL.Flags.Light )
		{
			v3	model_normal(f32( vert.norm_x ), f32( vert.norm_y ), f32( vert.norm_z ) );

			v3 vecTransformedNormal;
			vecTransformedNormal = mat_world.TransformNormal( model_normal );
			vecTransformedNormal.Normalise();

			mVtxProjected[i].Colour = LightVert(vecTransformedNormal);
			mVtxProjected[i].Colour.w = vert.rgba_a * (1.0f / 255.0f);

			// ENV MAPPING
			//
			if ( mTnL.Flags.TexGen )
			{
				// Update texture coords n.b. need to divide tu/tv by bogus scale on addition to buffer
				// If the vert is already lit, then there is no normal (and hence we can't generate tex coord)
#if 1			// 1->Lets use mat_world_project instead of mat_world for nicer effect (see SSV space ship) //Corn
				vecTransformedNormal = mat_world_project.TransformNormal( model_normal );
				vecTransformedNormal.Normalise();
#endif

				const v3 & norm = vecTransformedNormal;

				if( mTnL.Flags.TexGenLin )
				{
					mVtxProjected[i].Texture.x = 0.5f * ( 1.0f + norm.x );
					mVtxProjected[i].Texture.y = 0.5f * ( 1.0f + norm.y );
				}
				else
				{
					//Cheap way to do Acos(x)/Pi (abs() fixes star in SM64, sort of) //Corn
					f32 NormX = Abs( norm.x );
					f32 NormY = Abs( norm.y );
					mVtxProjected[i].Texture.x =  0.5f - 0.25f * NormX - 0.25f * NormX * NormX * NormX;
					mVtxProjected[i].Texture.y =  0.5f - 0.25f * NormY - 0.25f * NormY * NormY * NormY;
				}
			}
			else
			{
				//Set Texture coordinates
				mVtxProjected[i].Texture.x = (float)vert.tu * mTnL.TextureScaleX;
				mVtxProjected[i].Texture.y = (float)vert.tv * mTnL.TextureScaleY;
			}
		}
		else
		{
			//if( mTnL.Flags.Shade )	//FLAT shade
			{
				mVtxProjected[i].Colour = v4( vert.rgba_r * (1.0f / 255.0f), vert.rgba_g * (1.0f / 255.0f), vert.rgba_b * (1.0f / 255.0f), vert.rgba_a * (1.0f / 255.0f) );
			}
			/*else //Shade is disabled, doesn't work, is it even needed>?
			{
				mVtxProjected[i].Colour = mPrimitiveColour.GetColourV4();
			}*/

			//Set Texture coordinates
			mVtxProjected[i].Texture.x = (float)vert.tu * mTnL.TextureScaleX;
			mVtxProjected[i].Texture.y = (float)vert.tv * mTnL.TextureScaleY;
		}

		/*
		// FOG
		//
		if ( mTnL.Flags.Fog )
		{
			float	fog_coeff;
			//if(fabsf(projected.w) > 0.0f)
			{
				float eyespace_z = projected.z / projected.w;
				fog_coeff = (eyespace_z * mTnL.FogMult) + mTnL.FogOffset;
			}
			//else
			//{
			//	fog_coeff = m_fFogOffset;
			//}

			// Set the alpha
			mVtxProjected[i].Colour.w = Clamp< f32 >( fog_coeff, 0.0f, 1.0f );
		}
		*/
	}
}

#endif	//Transform VFPU/FPU

//*****************************************************************************
// Conker Bad Fur Day rendering pipeline
//*****************************************************************************
#ifdef DAEDALUS_PSP_USE_VFPU
void BaseRenderer::SetNewVertexInfoConker(u32 address, u32 v0, u32 n)
{
	const FiddledVtx * const pVtxBase( (const FiddledVtx*)(g_pu8RamBase + address) );
	const Matrix4x4 & mat_project = mProjectionMat;
	const Matrix4x4 & mat_world = mModelViewStack[mModelViewTop];

	DL_PF( "    Ambient color RGB[%f][%f][%f] Texture scale X[%f] Texture scale Y[%f]", mTnL.Lights[mTnL.NumLights].Colour.x, mTnL.Lights[mTnL.NumLights].Colour.y, mTnL.Lights[mTnL.NumLights].Colour.z, mTnL.TextureScaleX, mTnL.TextureScaleY);
	DL_PF( "    Light[%s] Texture[%s] EnvMap[%s] Fog[%s]", (mTnL.Flags.Light)? "On":"Off", (mTnL.Flags.Texture)? "On":"Off", (mTnL.Flags.TexGen)? (mTnL.Flags.TexGenLin)? "Linear":"Spherical":"Off", (mTnL.Flags.Fog)? "On":"Off");

	// Light is not handled for Conker
	//
	const s8 *mn = (s8*)(gAuxAddr);
	_TnLVFPUCBFD( &mat_world, &mat_project, pVtxBase, &mVtxProjected[v0], n, &mTnL, mn, v0<<1 );
}

#else
//FPU/CPU version //Corn
//extern f32 gCoord_Mod[16];

void BaseRenderer::SetNewVertexInfoConker(u32 address, u32 v0, u32 n)
{
	//DBGConsole_Msg(0, "In SetNewVertexInfo");
	const FiddledVtx * const pVtxBase( (const FiddledVtx*)(g_pu8RamBase + address) );
	const Matrix4x4 & mat_project = mProjectionMat;
	const Matrix4x4 & mat_world = mModelViewStack[mModelViewTop];

	DL_PF( "    Ambient color RGB[%f][%f][%f] Texture scale X[%f] Texture scale Y[%f]", mTnL.Lights[mTnL.NumLights].Colour.x, mTnL.Lights[mTnL.NumLights].Colour.y, mTnL.Lights[mTnL.NumLights].Colour.z, mTnL.TextureScaleX, mTnL.TextureScaleY);
	DL_PF( "    Light[%s] Texture[%s] EnvMap[%s] Fog[%s]", (mTnL.Flags.Light)? "On":"Off", (mTnL.Flags.Texture)? "On":"Off", (mTnL.Flags.TexGen)? (mTnL.Flags.TexGenLin)? "Linear":"Spherical":"Off", (mTnL.Flags.Fog)? "On":"Off");

	//Model normal base vector
	const s8 *mn = (s8*)(gAuxAddr);

	// Transform and Project + Lighting or Transform and Project with Colour
	//
	for (u32 i = v0; i < v0 + n; i++)
	{
		const FiddledVtx & vert = pVtxBase[i - v0];

		v4 w( f32( vert.x ), f32( vert.y ), f32( vert.z ), 1.0f );

		// VTX Transform
		//
		v4 & transformed( mVtxProjected[i].TransformedPos );
		transformed = mat_world.Transform( w );
		v4 & projected( mVtxProjected[i].ProjectedPos );
		projected = mat_project.Transform( transformed );

		//	Initialise the clipping flags
		//
		u32 clip_flags = 0;
		if		(projected.x < -projected.w)	clip_flags |= X_POS;
		else if (projected.x > projected.w)		clip_flags |= X_NEG;

		if		(projected.y < -projected.w)	clip_flags |= Y_POS;
		else if (projected.y > projected.w)		clip_flags |= Y_NEG;

		if		(projected.z < -projected.w)	clip_flags |= Z_POS;
		else if (projected.z > projected.w)		clip_flags |= Z_NEG;
		mVtxProjected[i].ClipFlags = clip_flags;

		// LIGHTING OR COLOR
		//
		if ( mTnL.Flags.Light )
		{
			v4	result( mTnL.Lights[mTnL.NumLights].Colour );

			for ( u32 k = 1; k < mTnL.NumLights; k++ )
			{
				result.x += mTnL.Lights[k].Colour.x;
				result.y += mTnL.Lights[k].Colour.y;
				result.z += mTnL.Lights[k].Colour.z;
			}

			//Clamp to 1.0
			if( result.x > 1.0f ) result.x = 1.0f;
			if( result.y > 1.0f ) result.y = 1.0f;
			if( result.z > 1.0f ) result.z = 1.0f;

			result.x *= (f32)vert.rgba_r * (1.0f / 255.0f);
			result.y *= (f32)vert.rgba_g * (1.0f / 255.0f);
			result.z *= (f32)vert.rgba_b * (1.0f / 255.0f);
			result.w  = (f32)vert.rgba_a * (1.0f / 255.0f);

			mVtxProjected[i].Colour = result;

			// ENV MAPPING
			//
			if ( mTnL.Flags.TexGen )
			{
				v3 model_normal( mn[((i<<1)+0)^3], mn[((i<<1)+1)^3], vert.normz );
				v3 vecTransformedNormal = mat_world.TransformNormal( model_normal );
				vecTransformedNormal.Normalise();

				const v3 & norm = vecTransformedNormal;

				if( mTnL.Flags.TexGenLin )
				{
					//Cheap way to do Acos(x)/Pi //Corn
					mVtxProjected[i].Texture.x =  0.5f - 0.25f * norm.x - 0.25f * norm.x * norm.x * norm.x;
					mVtxProjected[i].Texture.y =  0.5f - 0.25f * norm.y - 0.25f * norm.y * norm.y * norm.y;
				}
				else
				{
					mVtxProjected[i].Texture.x = 0.5f * ( 1.0f + norm.x );
					mVtxProjected[i].Texture.y = 0.5f * ( 1.0f + norm.y );
				}
			}
			else
			{	//TEXTURE
				mVtxProjected[i].Texture.x = (float)vert.tu * mTnL.TextureScaleX;
				mVtxProjected[i].Texture.y = (float)vert.tv * mTnL.TextureScaleY;
			}
		}
		else
		{
			if( mTnL.Flags.Shade )
			{	//FLAT shade
				mVtxProjected[i].Colour.x = (f32)vert.rgba_r * (1.0f / 255.0f);
				mVtxProjected[i].Colour.y = (f32)vert.rgba_g * (1.0f / 255.0f);
				mVtxProjected[i].Colour.z = (f32)vert.rgba_b * (1.0f / 255.0f);
				mVtxProjected[i].Colour.w = (f32)vert.rgba_a * (1.0f / 255.0f);
			}
			else
			{	//Shade is disabled
				mVtxProjected[i].Colour = mPrimitiveColour.GetColourV4();
			}

			//TEXTURE
			mVtxProjected[i].Texture.x = (float)vert.tu * mTnL.TextureScaleX;
			mVtxProjected[i].Texture.y = (float)vert.tv * mTnL.TextureScaleY;
		}
	}
}
#endif

//*****************************************************************************
// Assumes address has already been checked!
// DKR/Jet Force Gemini rendering pipeline
//*****************************************************************************
void BaseRenderer::SetNewVertexInfoDKR(u32 address, u32 v0, u32 n, bool billboard)
{
	u32 pVtxBase = u32(g_pu8RamBase + address);
	const Matrix4x4 & mat_world_project = mModelViewStack[mDKRMatIdx];

	DL_PF( "    Ambient color RGB[%f][%f][%f] Texture scale X[%f] Texture scale Y[%f]", mTnL.Lights[mTnL.NumLights].Colour.x, mTnL.Lights[mTnL.NumLights].Colour.y, mTnL.Lights[mTnL.NumLights].Colour.z, mTnL.TextureScaleX, mTnL.TextureScaleY);
	DL_PF( "    Light[%s] Texture[%s] EnvMap[%s] Fog[%s]", (mTnL.Flags.Light)? "On":"Off", (mTnL.Flags.Texture)? "On":"Off", (mTnL.Flags.TexGen)? (mTnL.Flags.TexGenLin)? "Linear":"Spherical":"Off", (mTnL.Flags.Fog)? "On":"Off");
	DL_PF( "    CMtx[%d] Add base[%s]", mDKRMatIdx, billboard? "On":"Off");

	if( billboard )
	{	//Copy vertices adding base vector and the color data
		mWPmodified = false;

#ifdef DAEDALUS_PSP_USE_VFPU
		_TnLVFPUDKRB( n, &mModelViewStack[0], (const FiddledVtx*)pVtxBase, &mVtxProjected[v0] );
#else
		v4 & BaseVec( mVtxProjected[0].TransformedPos );

		//Hack to worldproj matrix to scale and rotate billbords //Corn
		Matrix4x4 mat( mModelViewStack[0]);
		mat.mRaw[0] *= mModelViewStack[2].mRaw[0] * 0.5f;
		mat.mRaw[4] *= mModelViewStack[2].mRaw[0] * 0.5f;
		mat.mRaw[8] *= mModelViewStack[2].mRaw[0] * 0.5f;
		mat.mRaw[1] *= mModelViewStack[2].mRaw[0] * 0.375f;
		mat.mRaw[5] *= mModelViewStack[2].mRaw[0] * 0.375f;
		mat.mRaw[9] *= mModelViewStack[2].mRaw[0] * 0.375f;
		mat.mRaw[2] *= mModelViewStack[2].mRaw[10] * 0.5f;
		mat.mRaw[6] *= mModelViewStack[2].mRaw[10] * 0.5f;
		mat.mRaw[10] *= mModelViewStack[2].mRaw[10] * 0.5f;

		for (u32 i = v0; i < v0 + n; i++)
		{
			v3 w;
			w.x = *(s16*)((pVtxBase + 0) ^ 2);
			w.y = *(s16*)((pVtxBase + 2) ^ 2);
			w.z = *(s16*)((pVtxBase + 4) ^ 2);

			w = mat.TransformNormal( w );

			v4 & transformed( mVtxProjected[i].TransformedPos );
			transformed.x = BaseVec.x + w.x;
			transformed.y = BaseVec.y + w.y;
			transformed.z = BaseVec.z + w.z;
			transformed.w = 1.0f;

			// Set Clipflags, zero clippflags if billbording //Corn
			mVtxProjected[i].ClipFlags = 0;

			// Assign true vert colour
			const u32 WL = *(u16*)((pVtxBase + 6) ^ 2);
			const u32 WH = *(u16*)((pVtxBase + 8) ^ 2);

			mVtxProjected[i].Colour.x = (1.0f / 255.0f) * (WL >> 8);
			mVtxProjected[i].Colour.y = (1.0f / 255.0f) * (WL & 0xFF);
			mVtxProjected[i].Colour.z = (1.0f / 255.0f) * (WH >> 8);
			mVtxProjected[i].Colour.w = (1.0f / 255.0f) * (WH & 0xFF);

			pVtxBase += 10;
		}
#endif
	}
	else
	{	//Normal path for transform of triangles
		if( mWPmodified )
		{	//Only reload matrix if it has been changed and no billbording //Corn
			mWPmodified = false;
			sceGuSetMatrix( GU_PROJECTION, reinterpret_cast< const ScePspFMatrix4 * >( &mat_world_project) );
		}
#ifdef DAEDALUS_PSP_USE_VFPU
		_TnLVFPUDKR( n, &mat_world_project, (const FiddledVtx*)pVtxBase, &mVtxProjected[v0] );
#else
		for (u32 i = v0; i < v0 + n; i++)
		{
			v4 & transformed( mVtxProjected[i].TransformedPos );
			transformed.x = *(s16*)((pVtxBase + 0) ^ 2);
			transformed.y = *(s16*)((pVtxBase + 2) ^ 2);
			transformed.z = *(s16*)((pVtxBase + 4) ^ 2);
			transformed.w = 1.0f;

			v4 & projected( mVtxProjected[i].ProjectedPos );
			projected = mat_world_project.Transform( transformed );	//Do projection

			// Set Clipflags
			u32 clip_flags = 0;
			if		(projected.x < -projected.w)	clip_flags |= X_POS;
			else if (projected.x > projected.w)		clip_flags |= X_NEG;

			if		(projected.y < -projected.w)	clip_flags |= Y_POS;
			else if (projected.y > projected.w)		clip_flags |= Y_NEG;

			if		(projected.z < -projected.w)	clip_flags |= Z_POS;
			else if (projected.z > projected.w)		clip_flags |= Z_NEG;
			mVtxProjected[i].ClipFlags = clip_flags;

			// Assign true vert colour
			const u32 WL = *(u16*)((pVtxBase + 6) ^ 2);
			const u32 WH = *(u16*)((pVtxBase + 8) ^ 2);

			mVtxProjected[i].Colour.x = (1.0f / 255.0f) * (WL >> 8);
			mVtxProjected[i].Colour.y = (1.0f / 255.0f) * (WL & 0xFF);
			mVtxProjected[i].Colour.z = (1.0f / 255.0f) * (WH >> 8);
			mVtxProjected[i].Colour.w = (1.0f / 255.0f) * (WH & 0xFF);

			pVtxBase += 10;
		}
#endif
	}
}

//*****************************************************************************
// Perfect Dark rendering pipeline
//*****************************************************************************
#ifdef DAEDALUS_PSP_USE_VFPU
void BaseRenderer::SetNewVertexInfoPD(u32 address, u32 v0, u32 n)
{
	const FiddledVtxPD * const pVtxBase = (const FiddledVtxPD*)(g_pu8RamBase + address);

	const Matrix4x4 & mat_world = mModelViewStack[mModelViewTop];
	const Matrix4x4 & mat_project = mProjectionMat;

	DL_PF( "    Ambient color RGB[%f][%f][%f] Texture scale X[%f] Texture scale Y[%f]", mTnL.Lights[mTnL.NumLights].Colour.x, mTnL.Lights[mTnL.NumLights].Colour.y, mTnL.Lights[mTnL.NumLights].Colour.z, mTnL.TextureScaleX, mTnL.TextureScaleY);
	DL_PF( "    Light[%s] Texture[%s] EnvMap[%s] Fog[%s]", (mTnL.Flags.Light)? "On":"Off", (mTnL.Flags.Texture)? "On":"Off", (mTnL.Flags.TexGen)? (mTnL.Flags.TexGenLin)? "Linear":"Spherical":"Off", (mTnL.Flags.Fog)? "On":"Off");

	//Model & Color base vector
	const u8 *mn = (u8*)gAuxAddr;

	_TnLVFPUPD( &mat_world, &mat_project, pVtxBase, &mVtxProjected[v0], n, &mTnL, mn );
}

#else
void BaseRenderer::SetNewVertexInfoPD(u32 address, u32 v0, u32 n)
{
	const FiddledVtxPD * const pVtxBase = (const FiddledVtxPD*)(g_pu8RamBase + address);

	const Matrix4x4 & mat_world = mModelViewStack[mModelViewTop];
	const Matrix4x4 & mat_project = mProjectionMat;

	DL_PF( "    Ambient color RGB[%f][%f][%f] Texture scale X[%f] Texture scale Y[%f]", mTnL.Lights[mTnL.NumLights].Colour.x, mTnL.Lights[mTnL.NumLights].Colour.y, mTnL.Lights[mTnL.NumLights].Colour.z, mTnL.TextureScaleX, mTnL.TextureScaleY);
	DL_PF( "    Light[%s] Texture[%s] EnvMap[%s] Fog[%s]", (mTnL.Flags.Light)? "On":"Off", (mTnL.Flags.Texture)? "On":"Off", (mTnL.Flags.TexGen)? (mTnL.Flags.TexGenLin)? "Linear":"Spherical":"Off", (mTnL.Flags.Fog)? "On":"Off");

	//Model normal base vector
	const s8 *mn  = (s8*)(gAuxAddr);
	//Color base vector
	const u8 *col = (u8*)(gAuxAddr);

	for (u32 i = v0; i < v0 + n; i++)
	{
		const FiddledVtxPD & vert = pVtxBase[i - v0];

		v4 w( f32( vert.x ), f32( vert.y ), f32( vert.z ), 1.0f );

		// VTX Transform
		//
		v4 & transformed( mVtxProjected[i].TransformedPos );
		transformed = mat_world.Transform( w );
		v4 & projected( mVtxProjected[i].ProjectedPos );
		projected = mat_project.Transform( transformed );


		// Set Clipflags //Corn
		u32 clip_flags = 0;
		if		(projected.x < -projected.w)	clip_flags |= X_POS;
		else if (projected.x > projected.w)		clip_flags |= X_NEG;

		if		(projected.y < -projected.w)	clip_flags |= Y_POS;
		else if (projected.y > projected.w)		clip_flags |= Y_NEG;

		if		(projected.z < -projected.w)	clip_flags |= Z_POS;
		else if (projected.z > projected.w)		clip_flags |= Z_NEG;
		mVtxProjected[i].ClipFlags = clip_flags;

		if( mTnL.Flags.Light )
		{
			v3	model_normal((f32)mn[vert.cidx+3], (f32)mn[vert.cidx+2], (f32)mn[vert.cidx+1] );

			v3 vecTransformedNormal;
			vecTransformedNormal = mat_world.TransformNormal( model_normal );
			vecTransformedNormal.Normalise();

			mVtxProjected[i].Colour = LightVert(vecTransformedNormal);
			mVtxProjected[i].Colour.w = (f32)col[vert.cidx+0] * (1.0f / 255.0f);

			if ( mTnL.Flags.TexGen )
			{
				const v3 & norm = vecTransformedNormal;

				//Env mapping
				if( mTnL.Flags.TexGenLin )
				{	//Cheap way to do Acos(x)/Pi //Corn
					mVtxProjected[i].Texture.x =  0.5f - 0.25f * norm.x - 0.25f * norm.x * norm.x * norm.x;
					mVtxProjected[i].Texture.y =  0.5f - 0.25f * norm.y - 0.25f * norm.y * norm.y * norm.y;
				}
				else
				{
					mVtxProjected[i].Texture.x = 0.5f * ( 1.0f + norm.x );
					mVtxProjected[i].Texture.y = 0.5f * ( 1.0f + norm.y );
				}
			}
			else
			{
				mVtxProjected[i].Texture.x = (float)vert.tu * mTnL.TextureScaleX;
				mVtxProjected[i].Texture.y = (float)vert.tv * mTnL.TextureScaleY;
			}
		}
		else
		{
			if( mTnL.Flags.Shade )
			{	//FLAT shade
				mVtxProjected[i].Colour.x = (f32)col[vert.cidx+3] * (1.0f / 255.0f);
				mVtxProjected[i].Colour.y = (f32)col[vert.cidx+2] * (1.0f / 255.0f);
				mVtxProjected[i].Colour.z = (f32)col[vert.cidx+1] * (1.0f / 255.0f);
				mVtxProjected[i].Colour.w = (f32)col[vert.cidx+0] * (1.0f / 255.0f);
			}
			else
			{	//Shade is disabled
				mVtxProjected[i].Colour = mPrimitiveColour.GetColourV4();
			}

			mVtxProjected[i].Texture.x = (float)vert.tu * mTnL.TextureScaleX;
			mVtxProjected[i].Texture.y = (float)vert.tv * mTnL.TextureScaleY;
		}
	}
}
#endif

//*****************************************************************************
//
//*****************************************************************************
void BaseRenderer::ModifyVertexInfo(u32 whered, u32 vert, u32 val)
{
	switch ( whered )
	{
		case G_MWO_POINT_RGBA:
			{
				DL_PF("    Setting RGBA to 0x%08x", val);
				SetVtxColor( vert, c32( val ) );
			}
			break;

		case G_MWO_POINT_ST:
			{
				s16 tu = s16(val >> 16);
				s16 tv = s16(val & 0xFFFF);
				DL_PF( "    Setting tu/tv to %f, %f", tu/32.0f, tv/32.0f );
				SetVtxTextureCoord( vert, tu, tv );
			}
			break;

		case G_MWO_POINT_XYSCREEN:
			{
				if( g_ROM.GameHacks == TARZAN ) return;

				s16 x = (u16)(val >> 16) >> 2;
				s16 y = (u16)(val & 0xFFFF) >> 2;

				// Fixes the blocks lining up backwards in New Tetris
				//
				x -= uViWidth / 2;
				y = uViHeight / 2 - y;

				DL_PF("    Modify vert %d: x=%d, y=%d", vert, x, y);

#if 1
				// Megaman and other games
				SetVtxXY( vert, f32(x<<1) / fViWidth, f32(y<<1) / fViHeight );
#else
				u32 current_scale = Memory_VI_GetRegister(VI_X_SCALE_REG);
				if((current_scale&0xF) != 0 )
				{
					// Tarzan... I don't know why is so different...
					SetVtxXY( vert, f32(x) / fViWidth, f32(y) / fViHeight );
				}
				else
				{
					// Megaman and other games
					SetVtxXY( vert, f32(x<<1) / fViWidth, f32(y<<1) / fViHeight );
				}
#endif
			}
			break;

		case G_MWO_POINT_ZSCREEN:
			{
				//s32 z = val >> 16;
				//DL_PF( "      Setting ZScreen to 0x%08x", z );
				DL_PF( "    Setting ZScreen");
				//Not sure about the scaling here //Corn
				//SetVtxZ( vert, (( (f32)z / 0x03FF ) + 0.5f ) / 2.0f );
				//SetVtxZ( vert, (( (f32)z ) + 0.5f ) / 2.0f );
			}
			break;

		default:
			DBGConsole_Msg( 0, "ModifyVtx - Setting vert data where: 0x%02x, vert: 0x%08x, val: 0x%08x", whered, vert, val );
			DL_PF( "    Setting unknown value: where: 0x%02x, vert: 0x%08x, val: 0x%08x", whered, vert, val );
			break;
	}
}

//*****************************************************************************
//
//*****************************************************************************
inline void BaseRenderer::SetVtxColor( u32 vert, c32 color )
{
	DAEDALUS_ASSERT( vert < kMaxN64Vertices, "Vertex index is out of bounds (%d)", vert );

	mVtxProjected[vert].Colour = color.GetColourV4();
}

//*****************************************************************************
//
//*****************************************************************************
/*
inline void BaseRenderer::SetVtxZ( u32 vert, float z )
{
	DAEDALUS_ASSERT( vert < kMaxN64Vertices, "Vertex index is out of bounds (%d)", vert );

	mVtxProjected[vert].TransformedPos.z = z;
}
*/
//*****************************************************************************
//
//*****************************************************************************
inline void BaseRenderer::SetVtxXY( u32 vert, float x, float y )
{
	DAEDALUS_ASSERT( vert < kMaxN64Vertices, "Vertex index is out of bounds (%d)", vert );

	mVtxProjected[vert].TransformedPos.x = x;
	mVtxProjected[vert].TransformedPos.y = y;
}

//*****************************************************************************
// Init matrix stack to identity matrices (called once per frame)
//*****************************************************************************
void BaseRenderer::ResetMatrices(u32 size)
{
	//Tigger's Honey Hunt
	if(size == 0)
		size = MATRIX_STACK_SIZE;

	mMatStackSize = (size > MATRIX_STACK_SIZE) ? MATRIX_STACK_SIZE : size;
	mModelViewTop = 0;
	mProjectionMat = mModelViewStack[0] = gMatrixIdentity;
	mWorldProjectValid = false;
}

//*****************************************************************************
//
//*****************************************************************************
void BaseRenderer::UpdateTileSnapshots( u32 tile_idx )
{
	UpdateTileSnapshot( 0, tile_idx );

#if defined(DAEDALUS_PSP)
	if ( g_ROM.LOAD_T1_HACK & !gRDPOtherMode.text_lod )
	{
		// LOD is disabled - use two textures
		UpdateTileSnapshot( 1, tile_idx + 1 );
	}
#elif defined(DAEDALUS_GL) || defined(RDP_USE_TEXEL1)
// FIXME(strmnnrmn): What's RDP_USE_TEXEL1? Can we remove it?

	if (gRDPOtherMode.cycle_type == CYCLE_2CYCLE)
	{
		u32 t1_tile = (tile_idx + 1) & 7;

		// NB: I don't think we need to do this. lod_frac is set to 0.0 in the
		// OSX pixel shader, so it'll always use Texel 0 when mipmapping.
		// LOD is enabled - use the highest detail texture in texel1
		// if ( gRDPOtherMode.text_lod )
		// 	t1_tile = tile_idx;

		if ( !gRDPStateManager.IsTileInitialised(t1_tile) )
		{
			// FIXME(strmnnrmn): This happens a lot - not just for Tony Hawk.
			// DAEDALUS_DL_ERROR("Using T1, but it's not been set up");

			// FIXME(strmnnrmn): This is required so that Tony Hawk's text renders correctly.
			// It's odd. It calls TexRect with tile 1, and has
			// a color combiner that uses Texel 1 but not Texel 0.
			// But tile 2 has never been initialised.
			t1_tile = tile_idx;
		}

		UpdateTileSnapshot( 1, t1_tile );
	}
#endif
}

#ifdef DAEDALUS_PSP
static void T1Hack(const TextureInfo & ti0, CNativeTexture * texture0,
				   const TextureInfo & ti1, CNativeTexture * texture1)
{
	if((ti0.GetFormat() == G_IM_FMT_RGBA) &&
	   (ti1.GetFormat() == G_IM_FMT_I) &&
	   (ti1.GetWidth()  == ti0.GetWidth()) &&
	   (ti1.GetHeight() == ti0.GetHeight()))
	{
		if( g_ROM.T1_HACK )
		{
			const u32 * src = static_cast<const u32*>(texture0->GetData());
			u32 * dst       = static_cast<      u32*>(texture1->GetData());

			//Merge RGB + I -> RGBA in texture 1
			//We do two pixels in one go since its 16bit (RGBA_4444) //Corn
			u32 size = texture1->GetWidth() * texture1->GetHeight() >> 1;
			for(u32 i=0; i < size ; i++)
			{
				*dst = (*dst & 0xF000F000) | (*src & 0x0FFF0FFF);
				dst++;
				src++;
			}
		}
		else
		{
			const u32* src = static_cast<const u32*>(texture1->GetData());
			u32* dst       = static_cast<      u32*>(texture0->GetData());

			//Merge RGB + I -> RGBA in texture 0
			//We do two pixels in one go since its 16bit (RGBA_4444) //Corn
			u32 size = texture1->GetWidth() * texture1->GetHeight() >> 1;
			for(u32 i=0; i < size ; i++)
			{
				*dst = (*dst & 0x0FFF0FFF) | (*src & 0xF000F000);
				dst++;
				src++;
			}
		}
	}
}
#endif // DAEDALUS_PSP

//*****************************************************************************
// This captures the state of the RDP tiles in:
//   mTexWrap
//   mTexShift
//   mTileTopLeft
//   mBoundTexture
//*****************************************************************************
void BaseRenderer::UpdateTileSnapshot( u32 index, u32 tile_idx )
{
	DAEDALUS_PROFILE( "BaseRenderer::UpdateTileSnapshot" );

	DAEDALUS_ASSERT( tile_idx < 8, "Invalid tile index %d", tile_idx );
	DAEDALUS_ASSERT( index < kNumBoundTextures, "Invalid texture index %d", index );

	// This hapens a lot! Even for index 0 (i.e. the main texture!)
	// It might just be code that lazily does a texrect with Primcolour (i.e. not using either T0 or T1)?
	// DAEDALUS_ASSERT( gRDPStateManager.IsTileInitialised( tile_idx ), "Tile %d hasn't been set up (index %d)", tile_idx, index );

	const TextureInfo &  ti        = gRDPStateManager.GetUpdatedTextureDescriptor( tile_idx );
	const RDP_Tile &     rdp_tile  = gRDPStateManager.GetTile( tile_idx );
	const RDP_TileSize & tile_size = gRDPStateManager.GetTileSize( tile_idx );

	// Avoid texture update, if texture is the same as last time around.
	if( mBoundTexture[ index ] == NULL || mBoundTextureInfo[ index ] != ti )
	{
		// Check for 0 width/height textures
		if( ti.GetWidth() == 0 || ti.GetHeight() == 0 )
		{
			DAEDALUS_DL_ERROR( "Loading texture with 0 width/height in slot %d", index );
		}
		else
		{
			CRefPtr<CNativeTexture> texture = CTextureCache::Get()->GetOrCreateTexture( ti );

			if( texture != NULL && texture != mBoundTexture[ index ] )
			{
				mBoundTextureInfo[index] = ti;
				mBoundTexture[index]     = texture;

#ifdef DAEDALUS_PSP
				//If second texture is loaded try to merge two textures RGB(T0) + A(T1) into one RGBA(T1) //Corn
				//If T1 Hack is not enabled index can never be other than 0
				if(index)
				{
					T1Hack(mBoundTextureInfo[0], mBoundTexture[0], mBoundTextureInfo[1], mBoundTexture[1]);
				}
#endif
			}
		}
	}

	// Initialise the clamping state. When the mask is 0, it forces clamp mode.
	//
	u32 mode_u = (rdp_tile.clamp_s | ( rdp_tile.mask_s == 0)) ? GU_CLAMP : GU_REPEAT;
	u32 mode_v = (rdp_tile.clamp_t | ( rdp_tile.mask_t == 0)) ? GU_CLAMP : GU_REPEAT;

	//	In CRDPStateManager::GetTextureDescriptor, we limit the maximum dimension of a
	//	texture to that define by the mask_s/mask_t value.
	//	It this happens, the tile size can be larger than the truncated width/height
	//	as the rom can set clamp_s/clamp_t to wrap up to a certain value, then clamp.
	//	We can't support both wrapping and clamping (without manually repeating a texture...)
	//	so we choose to prefer wrapping.
	//	The castle in the background of the first SSB level is a good example of this behaviour.
	//	It sets up a texture with a mask_s/t of 6/6 (64x64), but sets the tile size to
	//	256*128. clamp_s/t are set, meaning the texture wraps 4x and 2x.
	//
	if( tile_size.GetWidth() > ti.GetWidth() )
	{
		// This breaks the Sun, and other textures in Zelda. Breaks Mario's hat in SSB, and other textures, and foes in Kirby 64's cutscenes
		// ToDo : Find a proper workaround for this, if this disabled the castle in Link's stage in SSB is broken :/
		// Do a hack just for Zelda for now..
		//
		mode_u = g_ROM.ZELDA_HACK ? GU_CLAMP : GU_REPEAT;
	}

	if( tile_size.GetHeight() > ti.GetHeight() )
		mode_v = GU_REPEAT;

	mTexWrap[ index ].u = mode_u;
	mTexWrap[ index ].v = mode_v;

	mTexShift[ index ].s = rdp_tile.shift_s;
	mTexShift[ index ].t = rdp_tile.shift_t;

	mTileTopLeft[ index ].x = f32(tile_size.left) / 4.0f;
	mTileTopLeft[ index ].y = f32(tile_size.top) / 4.0f;

	DL_PF( "    Use Tile[%d] as Texture[%d] [%dx%d] [%s/%dbpp] [%s u, %s v] -> Adr[0x%08x] PAL[0x%x] Hash[0x%08x] Pitch[%d] TopLeft[%0.3f|%0.3f]",
			tile_idx, index, ti.GetWidth(), ti.GetHeight(), ti.GetFormatName(), ti.GetSizeInBits(),
			(mode_u==GU_CLAMP)? "Clamp" : "Repeat", (mode_v==GU_CLAMP)? "Clamp" : "Repeat",
			ti.GetLoadAddress(), ti.GetTlutAddress(), ti.GetHashCode(), ti.GetPitch(),
			mTileTopLeft[ index ].x, mTileTopLeft[ index ].y );
}


// This transforms UVs so that they're positive. The aim is to ensure UVs are in the
// range [(0,0),(w,h)]. If we can do this, we can specify GL_CLAMP_TO_EDGE/GU_CLAMP,
// which fixes some artifacts when rendering, such as bleed from wrapping at the edges
// of textures. E.g. http://imgur.com/db3Adws,dX9vOWE#1
// There are two inputs into the final uvs: the vertex UV and the mTileTopLeft value:
//   final_uv = (vert_uv - mTileTopLeft).
// When rendering a large logo, most games set uv0=(s,t) and mTileTopLeft=(s,t) so
// that the resulting final_uv = (0,0). But some games (e.g. Automobili Lamborghini)
// set uv0=(0,0) but still have mTileTopLeft=(s,t). This results in a final_uv of (-s,-t).
// I think that the only reason this happened to work was because s was some multiple
// of the texture width, and so with GL_REPEAT the texrect rendered ok.
// Anyway the fix is to subtract mTileTopLeft from the uvs, zero it, then add multiples
// of the texture width/height until the uvs are positive. Then if the resulting UVs
// are in the range [(0,0),(w,h)] we can update mTexWrap to GL_CLAMP_TO_EDGE/GU_CLAMP
// and everything works correctly.
inline void FixUV(u32 * wrap, float * c0, float * c1, float offset, float size)
{
	*c0 -= offset;
	*c1 -= offset;

	// Many texrects already have GU_CLAMP set, so avoid some work.
	if (*wrap != GU_CLAMP)
	{
		DAEDALUS_ASSERT(size > 0.f, "Texture has crazy width/height");

		// Check if the coord is negative - if so, offset to the range [0,size]
		if (*c0 < 0.f)
		{
			// If integer, this would simply be (-uv0x + size-1) % size
			// NB! we have to apply the same offset to both coords, to preserve direction of mapping (i.e., don't clamp each independently)
			float v = ceilf(-*c0 / size) * size;
			*c0 += v;
			*c1 += v;
		}

		// If both coords are in the range [0,size], we can clamp safely.
		// If integer, this would be just '(unsigned)c0 <= size & (unsigned)c1 <= size'.
		// NB c0 is guaranteed >= 0 here, so don't bother checking.
		DAEDALUS_ASSERT(*c0 >= 0.0, "Arithmetic error");
		if (*c0 <= size &&
			*c1 <= size && *c1 >= 0.f)
		{
			*wrap = GU_CLAMP;
		}
	}
}

// puv0, puv1 are in/out arguments.
void BaseRenderer::PrepareTexRectUVs(v2 * puv0, v2 * puv1)
{
	v2 		offset = mTileTopLeft[0];
	float 	size_x = mBoundTextureInfo[0].GetWidth();
	float 	size_y = mBoundTextureInfo[0].GetHeight();

#ifdef DAEDALUS_GL
	// If using mTexShift, we need to take it into account here.
	float ss = kShiftScales[mTexShift[0].s];
	float st = kShiftScales[mTexShift[0].t];
	offset.x *= ss;
	offset.y *= st;
	size_x *= ss;
	size_y *= st;
#endif

	FixUV(&mTexWrap[0].u, &puv0->x, &puv1->x, offset.x, size_x);
	FixUV(&mTexWrap[0].v, &puv0->y, &puv1->y, offset.y, size_y);

	mTileTopLeft[0].x = 0.f;
	mTileTopLeft[0].y = 0.f;
}

//*****************************************************************************
//
//*****************************************************************************
CRefPtr<CNativeTexture> BaseRenderer::LoadTextureDirectly( const TextureInfo & ti )
{
	CRefPtr<CNativeTexture> texture = CTextureCache::Get()->GetOrCreateTexture( ti );
	DAEDALUS_ASSERT( texture, "texture is NULL" );

	texture->InstallTexture();

	mBoundTexture[0] = texture;
	mBoundTextureInfo[0] = ti;

	return texture;
}

//*****************************************************************************
//
//*****************************************************************************
void BaseRenderer::SetScissor( u32 x0, u32 y0, u32 x1, u32 y1 )
{
	//Clamp scissor to max N64 screen resolution //Corn
	if( x1 > uViWidth )  x1 = uViWidth;
	if( y1 > uViHeight ) y1 = uViHeight;

	v2 n64_tl( x0, y0 );
	v2 n64_br( x1, y1 );

	v2 screen_tl;
	v2 screen_br;
	ConvertN64ToScreen( n64_tl, screen_tl );
	ConvertN64ToScreen( n64_br, screen_br );

	//Clamp TOP and LEFT values to 0 if < 0 , needed for zooming //Corn
	s32 l = Max<s32>( s32(screen_tl.x), 0 );
	s32 t = Max<s32>( s32(screen_tl.y), 0 );
	s32 r =           s32(screen_br.x);
	s32 b =           s32(screen_br.y);

#if defined(DAEDALUS_PSP)
	// N.B. Think the arguments are x0,y0,x1,y1, and not x,y,w,h as the docs describe
	//printf("%d %d %d %d\n", s32(screen_tl.x),s32(screen_tl.y),s32(screen_br.x),s32(screen_br.y));
	sceGuScissor( l, t, r, b );
#elif defined(DAEDALUS_GL)
	// NB: OpenGL is x,y,w,h. Errors if width or height is negative, so clamp this.
	s32 w = Max( r - l, 0L );
	s32 h = Max( b - t, 0L );
	glScissor( l, mScreenHeight - (t + h), w, h );
#else
	DAEDALUS_ERROR("Need to implement scissor for this platform.")
#endif
}

extern void MatrixFromN64FixedPoint( Matrix4x4 & mat, u32 address );
//*****************************************************************************
//
//*****************************************************************************
void BaseRenderer::SetProjection(const u32 address, bool bReplace)
{
	// Projection
	if (bReplace)
	{
		// Load projection matrix
		MatrixFromN64FixedPoint( mProjectionMat, address);

		//Hack needed to show heart in OOT & MM
		//it renders at Z cordinate = 0.0f that gets clipped away.
		//so we translate them a bit along Z to make them stick :) //Corn
		//
		if( g_ROM.ZELDA_HACK )
			mProjectionMat.mRaw[14] += 0.4f;
		if( gGlobalPreferences.ViewportType == VT_FULLSCREEN_HD )
			mProjectionMat.mRaw[0] *= HD_SCALE;	//proper 16:9 scale
	}
	else
	{
		MatrixFromN64FixedPoint( mTempMat, address);
		MatrixMultiplyAligned( &mProjectionMat, &mTempMat, &mProjectionMat );
	}

	mWorldProjectValid = false;
	sceGuSetMatrix( GU_PROJECTION, reinterpret_cast< const ScePspFMatrix4 * >( &mProjectionMat) );

	DL_PF(
		"	 %#+12.5f %#+12.5f %#+12.7f %#+12.5f\n"
		"    %#+12.5f %#+12.5f %#+12.7f %#+12.5f\n"
		"    %#+12.5f %#+12.5f %#+12.7f %#+12.5f\n"
		"    %#+12.5f %#+12.5f %#+12.7f %#+12.5f\n",
		mProjectionMat.m[0][0], mProjectionMat.m[0][1], mProjectionMat.m[0][2], mProjectionMat.m[0][3],
		mProjectionMat.m[1][0], mProjectionMat.m[1][1], mProjectionMat.m[1][2], mProjectionMat.m[1][3],
		mProjectionMat.m[2][0], mProjectionMat.m[2][1], mProjectionMat.m[2][2], mProjectionMat.m[2][3],
		mProjectionMat.m[3][0], mProjectionMat.m[3][1], mProjectionMat.m[3][2], mProjectionMat.m[3][3]);
}

//*****************************************************************************
//
//*****************************************************************************
void BaseRenderer::SetDKRMat(const u32 address, bool mul, u32 idx)
{
	mDKRMatIdx = idx;
	mWPmodified = true;

	if( mul )
	{
		MatrixFromN64FixedPoint( mTempMat, address );
		MatrixMultiplyAligned( &mModelViewStack[idx], &mTempMat, &mModelViewStack[0] );
	}
	else
	{
		MatrixFromN64FixedPoint( mModelViewStack[idx], address );
	}

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
	const Matrix4x4 & mtx( mModelViewStack[idx] );
	DL_PF("    Mtx_DKR: Index %d %s Address 0x%08x\n"
			"    %#+12.5f %#+12.5f %#+12.5f %#+12.5f\n"
			"    %#+12.5f %#+12.5f %#+12.5f %#+12.5f\n"
			"    %#+12.5f %#+12.5f %#+12.5f %#+12.5f\n"
			"    %#+12.5f %#+12.5f %#+12.5f %#+12.5f\n",
			idx, mul ? "Mul" : "Load", address,
			mtx.m[0][0], mtx.m[0][1], mtx.m[0][2], mtx.m[0][3],
			mtx.m[1][0], mtx.m[1][1], mtx.m[1][2], mtx.m[1][3],
			mtx.m[2][0], mtx.m[2][1], mtx.m[2][2], mtx.m[2][3],
			mtx.m[3][0], mtx.m[3][1], mtx.m[3][2], mtx.m[3][3]);
#endif
}
//*****************************************************************************
//
//*****************************************************************************
void BaseRenderer::SetWorldView(const u32 address, bool bPush, bool bReplace)
{
	// ModelView
	if (bPush && (mModelViewTop < mMatStackSize))
	{
		++mModelViewTop;

		// We should store the current projection matrix...
		if (bReplace)
		{
			// Load ModelView matrix
			MatrixFromN64FixedPoint( mModelViewStack[mModelViewTop], address);
			//Hack to make GEX games work, need to multiply all elements with 2.0 //Corn
			if( g_ROM.GameHacks == GEX_GECKO ) for(u32 i=0;i<16;i++) mModelViewStack[mModelViewTop].mRaw[i] += mModelViewStack[mModelViewTop].mRaw[i];
		}
		else	// Multiply ModelView matrix
		{
			MatrixFromN64FixedPoint( mTempMat, address);
			MatrixMultiplyAligned( &mModelViewStack[mModelViewTop], &mTempMat, &mModelViewStack[mModelViewTop-1] );
		}
	}
	else	// NoPush
	{
		if (bReplace)
		{
			// Load ModelView matrix
			MatrixFromN64FixedPoint( mModelViewStack[mModelViewTop], address);
		}
		else
		{
			// Multiply ModelView matrix
			MatrixFromN64FixedPoint( mTempMat, address);
			MatrixMultiplyAligned( &mModelViewStack[mModelViewTop], &mTempMat, &mModelViewStack[mModelViewTop] );
		}
	}

	mWorldProjectValid = false;

	DL_PF("    Level = %d\n"
		"    %#+12.5f %#+12.5f %#+12.5f %#+12.5f\n"
		"    %#+12.5f %#+12.5f %#+12.5f %#+12.5f\n"
		"    %#+12.5f %#+12.5f %#+12.5f %#+12.5f\n"
		"    %#+12.5f %#+12.5f %#+12.5f %#+12.5f\n",
		mModelViewTop,
		mModelViewStack[mModelViewTop].m[0][0], mModelViewStack[mModelViewTop].m[0][1], mModelViewStack[mModelViewTop].m[0][2], mModelViewStack[mModelViewTop].m[0][3],
		mModelViewStack[mModelViewTop].m[1][0], mModelViewStack[mModelViewTop].m[1][1], mModelViewStack[mModelViewTop].m[1][2], mModelViewStack[mModelViewTop].m[1][3],
		mModelViewStack[mModelViewTop].m[2][0], mModelViewStack[mModelViewTop].m[2][1], mModelViewStack[mModelViewTop].m[2][2], mModelViewStack[mModelViewTop].m[2][3],
		mModelViewStack[mModelViewTop].m[3][0], mModelViewStack[mModelViewTop].m[3][1], mModelViewStack[mModelViewTop].m[3][2], mModelViewStack[mModelViewTop].m[3][3]);
}

//*****************************************************************************
//
//*****************************************************************************
inline void BaseRenderer::UpdateWorldProject()
{
	if( !mWorldProjectValid )
	{
		mWorldProjectValid = true;
		if( mReloadProj )
		{
			mReloadProj = false;
			sceGuSetMatrix( GU_PROJECTION, reinterpret_cast< const ScePspFMatrix4 * >( &mProjectionMat) );
		}
		MatrixMultiplyAligned( &mWorldProject, &mModelViewStack[mModelViewTop], &mProjectionMat );
	}
}

//If WoldProjectmatrix has been modified due to insert or force matrix (Kirby, SSB / Tarzan, Rayman2, Donald duck, SW racer, Robot on wheels)
//we need to update sceGU projmtx //Corn
inline void BaseRenderer::PokeWorldProject()
{
	if( mWPmodified )
	{
		mWPmodified = false;
		mReloadProj = true;
		if( gGlobalPreferences.ViewportType == VT_FULLSCREEN_HD )
		{	//proper 16:9 scale
			mWorldProject.mRaw[0] *= HD_SCALE;
			mWorldProject.mRaw[4] *= HD_SCALE;
			mWorldProject.mRaw[8] *= HD_SCALE;
			mWorldProject.mRaw[12] *= HD_SCALE;
		}
		sceGuSetMatrix( GU_PROJECTION, reinterpret_cast< const ScePspFMatrix4 * >( &mWorldProject ) );
		mModelViewStack[mModelViewTop] = gMatrixIdentity;
	}
}


//*****************************************************************************
//
//*****************************************************************************
#ifdef DAEDALUS_DEBUG_DISPLAYLIST
void BaseRenderer::PrintActive()
{
	UpdateWorldProject();
	const Matrix4x4 & mat = mWorldProject;

	DL_PF(
		"    %#+12.5f %#+12.5f %#+12.5f %#+12.5f\n"
		"    %#+12.5f %#+12.5f %#+12.5f %#+12.5f\n"
		"    %#+12.5f %#+12.5f %#+12.5f %#+12.5f\n"
		"    %#+12.5f %#+12.5f %#+12.5f %#+12.5f\n",
		mat.m[0][0], mat.m[0][1], mat.m[0][2], mat.m[0][3],
		mat.m[1][0], mat.m[1][1], mat.m[1][2], mat.m[1][3],
		mat.m[2][0], mat.m[2][1], mat.m[2][2], mat.m[2][3],
		mat.m[3][0], mat.m[3][1], mat.m[3][2], mat.m[3][3]);
}
#endif

//*****************************************************************************
//Modify the WorldProject matrix, used by Kirby & SSB //Corn
//*****************************************************************************
void BaseRenderer::InsertMatrix(u32 w0, u32 w1)
{
	mWPmodified = true;	//Signal that Worldproject matrix is changed

	//Make sure WP matrix is up to date before changing WP matrix
	if( !mWorldProjectValid )
	{
		mWorldProject = mModelViewStack[mModelViewTop] * mProjectionMat;
		mWorldProjectValid = true;
	}

	u32 x = (w0 & 0x1F) >> 1;
	u32 y = x >> 2;
	x &= 3;

	if (w0 & 0x20)
	{
		//Change fraction part
		mWorldProject.m[y][x]   = (f32)(s32)mWorldProject.m[y][x] + ((f32)(w1 >> 16) / 65536.0f);
		mWorldProject.m[y][x+1] = (f32)(s32)mWorldProject.m[y][x+1] + ((f32)(w1 & 0xFFFF) / 65536.0f);
	}
	else
	{
		//Change integer part
		mWorldProject.m[y][x]	= (f32)(s16)(w1 >> 16);
		mWorldProject.m[y][x+1] = (f32)(s16)(w1 & 0xFFFF);
	}

	DL_PF(
		"    %#+12.5f %#+12.5f %#+12.5f %#+12.5f\n"
		"    %#+12.5f %#+12.5f %#+12.5f %#+12.5f\n"
		"    %#+12.5f %#+12.5f %#+12.5f %#+12.5f\n"
		"    %#+12.5f %#+12.5f %#+12.5f %#+12.5f\n",
		mWorldProject.m[0][0], mWorldProject.m[0][1], mWorldProject.m[0][2], mWorldProject.m[0][3],
		mWorldProject.m[1][0], mWorldProject.m[1][1], mWorldProject.m[1][2], mWorldProject.m[1][3],
		mWorldProject.m[2][0], mWorldProject.m[2][1], mWorldProject.m[2][2], mWorldProject.m[2][3],
		mWorldProject.m[3][0], mWorldProject.m[3][1], mWorldProject.m[3][2], mWorldProject.m[3][3]);
}

//*****************************************************************************
//Replaces the WorldProject matrix //Corn
//*****************************************************************************
void BaseRenderer::ForceMatrix(const u32 address)
{
	mWorldProjectValid = true;
	mWPmodified = true;	//Signal that Worldproject matrix is changed

	MatrixFromN64FixedPoint( mWorldProject, address );

	DL_PF(
		"    %#+12.5f %#+12.5f %#+12.5f %#+12.5f\n"
		"    %#+12.5f %#+12.5f %#+12.5f %#+12.5f\n"
		"    %#+12.5f %#+12.5f %#+12.5f %#+12.5f\n"
		"    %#+12.5f %#+12.5f %#+12.5f %#+12.5f\n",
		mWorldProject.m[0][0], mWorldProject.m[0][1], mWorldProject.m[0][2], mWorldProject.m[0][3],
		mWorldProject.m[1][0], mWorldProject.m[1][1], mWorldProject.m[1][2], mWorldProject.m[1][3],
		mWorldProject.m[2][0], mWorldProject.m[2][1], mWorldProject.m[2][2], mWorldProject.m[2][3],
		mWorldProject.m[3][0], mWorldProject.m[3][1], mWorldProject.m[3][2], mWorldProject.m[3][3]);
}
