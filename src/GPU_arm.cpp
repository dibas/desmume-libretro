/*
	Copyright (C) 2006 yopyop
	Copyright (C) 2006-2007 Theo Berkau
	Copyright (C) 2007 shash
	Copyright (C) 2008-2012 DeSmuME team

	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.

	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with the this software.  If not, see <http://www.gnu.org/licenses/>.
*/


#include <algorithm>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <iostream>
#include "MMU.h"
#include "GPU.h"
#include "debug.h"
#include "render3D.h"
#include "gfx3d.h"
#include "GPU_osd.h"
#include "NDSSystem.h"
#include "readwrite.h"

//#include "GPU_arm_neon.h"

#include "../libretro/performance.h"

#ifdef FASTBUILD
	#undef FORCEINLINE
	#define FORCEINLINE
#endif

#define SCREEN_WIDTH 256

extern BOOL click;
NDS_Screen MainScreen;
NDS_Screen SubScreen;

//#define DEBUG_TRI

CACHE_ALIGN u8 GPU_screen[4*256*192];
CACHE_ALIGN u8 sprWin[256];

//dont ever think of changing these to bits because you could avoid the multiplies in the main tile blitter.
//it doesnt really help any
static const short sizeTab[8][4][2] =
{
	{{0, 0}, {0, 0}, {0, 0}, {0, 0}}, //Invalid
    {{256,256}, {512,256}, {256,512}, {512,512}}, //text
    {{128,128}, {256,256}, {512,512}, {1024,1024}}, //affine
    {{512,1024}, {1024,512}, {0,0}, {0,0}}, //large 8bpp
	{{0, 0}, {0, 0}, {0, 0}, {0, 0}}, //affine ext (to be elaborated with another value)
	{{128,128}, {256,256}, {512,512}, {1024,1024}}, //affine ext 256x16
	{{128,128}, {256,256}, {512,256}, {512,512}}, //affine ext 256x1
	{{128,128}, {256,256}, {512,256}, {512,512}}, //affine ext direct
};

static const CACHE_ALIGN u8 win_empty[256] = {
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
static CACHE_ALIGN u16 fadeInColors[17][0x8000];
CACHE_ALIGN u16 fadeOutColors[17][0x8000];

//this should be public, because it gets used somewhere else
CACHE_ALIGN u8 gpuBlendTable555[17][17][32][32];


/*****************************************************************************/
//			INITIALIZATION
/*****************************************************************************/


static void GPU_InitFadeColors()
{
	/*
	NOTE: gbatek (in the reference above) seems to expect 6bit values 
	per component, but as desmume works with 5bit per component, 
	we use 31 as top, instead of 63. Testing it on a few games, 
	using 63 seems to give severe color wraping, and 31 works
	nicely, so for now we'll just that, until proven wrong.

	i have seen pics of pokemon ranger getting white with 31, with 63 it is nice.
	it could be pb of alpha or blending or...

	MightyMax> created a test NDS to check how the brightness values work,
	and 31 seems to be correct. FactorEx is a override for max brighten/darken
	See: http://mightymax.org/gfx_test_brightness.nds
	The Pokemon Problem could be a problem with 8/32 bit writes not recognized yet,
	i'll add that so you can check back.

	*/

	for(int i = 0; i <= 16; i++)
	{
		for(int j = 0x8000; j < 0x10000; j++)
		{
			COLOR cur;

			cur.val = j;
			cur.bits.red = (cur.bits.red + ((31 - cur.bits.red) * i / 16));
			cur.bits.green = (cur.bits.green + ((31 - cur.bits.green) * i / 16));
			cur.bits.blue = (cur.bits.blue + ((31 - cur.bits.blue) * i / 16));
			cur.bits.alpha = 0;
			fadeInColors[i][j & 0x7FFF] = cur.val;

			cur.val = j;
			cur.bits.red = (cur.bits.red - (cur.bits.red * i / 16));
			cur.bits.green = (cur.bits.green - (cur.bits.green * i / 16));
			cur.bits.blue = (cur.bits.blue - (cur.bits.blue * i / 16));
			cur.bits.alpha = 0;
			fadeOutColors[i][j & 0x7FFF] = cur.val;
		}
	}


	for(int c0=0;c0<=31;c0++) 
		for(int c1=0;c1<=31;c1++) 
			for(int eva=0;eva<=16;eva++)
				for(int evb=0;evb<=16;evb++)
				{
					int blend = ((c0 * eva) + (c1 * evb) ) / 16;
					int final = std::min<int>(31,blend);
					gpuBlendTable555[eva][evb][c0][c1] = final;
				}
}

static CACHE_ALIGN GPU GPU_main, GPU_sub;

GPU * GPU_Init(u8 l)
{
	GPU * g;

	if(l==0) g = &GPU_main;
	else g = &GPU_sub;

	GPU_Reset(g, l);
	GPU_InitFadeColors();

	g->curr_win[0] = win_empty;
	g->curr_win[1] = win_empty;
	g->need_update_winh[0] = true;
	g->need_update_winh[1] = true;
	g->setFinalColorBck_funcNum = 0;
	g->setFinalColor3d_funcNum = 0;
	g->setFinalColorSpr_funcNum = 0;

	return g;
}

void GPU_Reset(GPU *g, u8 l)
{
	memset(g, 0, sizeof(GPU));

	//important for emulator stability for this to initialize, since we have to setup a table based on it
	g->BLDALPHA_EVA = 0;
	g->BLDALPHA_EVB = 0;
	//make sure we have our blend table setup even if the game blends without setting the blend variables
	g->updateBLDALPHA();

	g->setFinalColorBck_funcNum = 0;
	g->setFinalColor3d_funcNum = 0;
	g->setFinalColorSpr_funcNum = 0;
	g->core = l;

   for (int i = 0; i != 4; i ++)
   {
      g->backgrounds[i].parent = g;
      g->backgrounds[i].number = i;
      g->backgrounds[i].set_size(256, 256);
   }

   g->oam.parent = g;
	g->oam.mode = GPU::oam_t::SPRITE_1D;

	if(g->core == GPU_SUB)
	{
      g->palette = (u16*)(MMU.ARM9_VMEM + ADDRESS_STEP_1KB);
		g->oam.oam = (MMU.ARM9_OAM + ADDRESS_STEP_1KB);
		g->oam.memory = MMU_BOBJ;
		// GPU core B
		g->dispx_st = (REG_DISPx*)(&MMU.ARM9_REG[REG_DISPB]);
	}
	else
	{
      g->palette = (u16*)MMU.ARM9_VMEM;
		g->oam.oam = (MMU.ARM9_OAM);
		g->oam.memory = MMU_AOBJ;
		// GPU core A
		g->dispx_st = (REG_DISPx*)(&MMU.ARM9_REG[0]);
	}
}

void GPU_DeInit(GPU * gpu)
{
	if(gpu==&GPU_main || gpu==&GPU_sub) return;
	free(gpu);
}

static FORCEINLINE u16 _blend(u16 colA, u16 colB, GPU::TBlendTable* blendTable)
{
	u8 r = (*blendTable)[colA&0x1F][colB&0x1F];
	u8 g = (*blendTable)[(colA>>5)&0x1F][(colB>>5)&0x1F];
	u8 b = (*blendTable)[(colA>>10)&0x1F][(colB>>10)&0x1F];

	return r|(g<<5)|(b<<10);
}

FORCEINLINE u16 GPU::blend(u16 colA, u16 colB)
{
	return _blend(colA, colB, blendTable);
}

void SetupFinalPixelBlitter (GPU *gpu)
{
	u8 windowUsed = (gpu->WIN0_ENABLED | gpu->WIN1_ENABLED | gpu->WINOBJ_ENABLED);
	u8 blendMode  = (gpu->BLDCNT >> 6)&3;

	gpu->setFinalColorSpr_funcNum = windowUsed*4 + blendMode;
	gpu->setFinalColorBck_funcNum = windowUsed*4 + blendMode;
	gpu->setFinalColor3d_funcNum = windowUsed*4 + blendMode;
	
}
    
//Sets up LCD control variables for Display Engines A and B for quick reading
void GPU::refresh_display_control()
{
   const display_control_t display_control = get_display_control();

	WIN0_ENABLED	  = display_control.Win0_Enable;
	WIN1_ENABLED	  = display_control.Win1_Enable;
	WINOBJ_ENABLED = display_control.WinOBJ_Enable;

	SetupFinalPixelBlitter(this);

   oam.enable = display_control.OBJ_Enable;
   oam.mode = display_control.OBJ_Tile_mapping ? GPU::oam_t::SPRITE_1D : GPU::oam_t::SPRITE_2D;
   oam.boundary = 5 + (display_control.OBJ_Tile_mapping ? display_control.OBJ_Tile_1D_Bound : 0);
   oam.bitmap_boundary = (core == GPU_MAIN && display_control.OBJ_BMP_1D_Bound) ? 8 : 7;

   for (int i = 0; i != 4; i ++)
   {
      refresh_background_control(i);
   }
}

void GPU::resort_backgrounds()
{
   const display_control_t display_control = get_display_control();

	LayersEnable[0] = CommonSettings.dispLayers[core][0] ^ !(display_control.BG0_Enable);
	LayersEnable[1] = CommonSettings.dispLayers[core][1] ^ !(display_control.BG1_Enable);
	LayersEnable[2] = CommonSettings.dispLayers[core][2] ^ !(display_control.BG2_Enable);
	LayersEnable[3] = CommonSettings.dispLayers[core][3] ^ !(display_control.BG3_Enable);
	LayersEnable[4] = CommonSettings.dispLayers[core][4] ^ !(display_control.OBJ_Enable);

	for (int i = 0; i < NB_PRIORITIES; i ++)
   {
      itemsForPriority[i].nbBGs = 0;
      itemsForPriority[i].nbPixelsX = 0;
	}

	for (int i = NB_BG - 1; i >= 0; i --)
   {
      if (LayersEnable[i])
      {
   		itemsForPriority_t* item = &itemsForPriority[backgrounds[i].get_control().Priority];
         item->BGs[item->nbBGs ++] = i;
      }
	}
}


/*****************************************************************************/
//		ROUTINES FOR INSIDE / OUTSIDE WINDOW CHECKS
/*****************************************************************************/
template<int WIN_NUM>
FORCEINLINE u8 GPU::withinRect(u16 x) const
{
	assert(x<256); //only way to be >256 is in debug views, and mosaic shouldnt be enabled for those
	return curr_win[WIN_NUM][x];
}

//  Now assumes that *draw and *effect are different from 0 when called, so we can avoid
// setting some values twice
FORCEINLINE void GPU::renderline_checkWindows(u16 x, bool &draw, bool &effect) const
{
	// Check if win0 if enabled, and only check if it is
	// howevever, this has already been taken care of by the window precalculation
	//if (WIN0_ENABLED)
	{
		// it is in win0, do we display ?
		// high priority	
		if (withinRect<0>(x))
		{
			//INFO("bg%i passed win0 : (%i %i) was within (%i %i)(%i %i)\n", bgnum, x, gpu->currLine, gpu->WIN0H0, gpu->WIN0V0, gpu->WIN0H1, gpu->WIN0V1);
			draw = (WININ0 >> currBgNum)&1;
			effect = (WININ0_SPECIAL);
			return;
		}
	}

	// Check if win1 if enabled, and only check if it is
	//if (WIN1_ENABLED)
	// howevever, this has already been taken care of by the window precalculation
	{
		// it is in win1, do we display ?
		// mid priority
		if(withinRect<1>(x))
		{
			//INFO("bg%i passed win1 : (%i %i) was within (%i %i)(%i %i)\n", bgnum, x, gpu->currLine, gpu->WIN1H0, gpu->WIN1V0, gpu->WIN1H1, gpu->WIN1V1);
			draw	= (WININ1 >> currBgNum)&1;
			effect = (WININ1_SPECIAL);
			return;
		}
	}

	//if(true) //sprwin test hack
	if (WINOBJ_ENABLED)
	{
		// it is in winOBJ, do we display ?
		// low priority
		if (sprWin[x])
		{
			draw	= (WINOBJ >> currBgNum)&1;
			effect	= (WINOBJ_SPECIAL);
			return;
		}
	}

	if (WINOBJ_ENABLED | WIN1_ENABLED | WIN0_ENABLED)
	{
		draw	= (WINOUT >> currBgNum) & 1;
		effect	= (WINOUT_SPECIAL);
	}
}

/*****************************************************************************/
//			PIXEL RENDERING
/*****************************************************************************/
template<BlendFunc FUNC, bool WINDOW>
FORCEINLINE FASTCALL void GPU::_master_setFinal3dColor(int dstX, int srcX)
{
	int x = dstX;
	int passing = dstX<<1;
	u8* color = &_3dColorLine[srcX<<2];
	u8 red = color[0];
	u8 green = color[1];
	u8 blue = color[2];
	u8 alpha = color[3];
	u8* dst = currDst;
	u16 final;

	bool windowEffect = blend1; //bomberman land touch dialogbox will fail without setting to blend1
	
	//TODO - should we do an alpha==0 -> bail out entirely check here?

	if(WINDOW)
	{
		bool windowDraw = false;
		renderline_checkWindows(dstX, windowDraw, windowEffect);

		//we never have anything more to do if the window rejected us
		if(!windowDraw) return;
	}

	int bg_under = bgPixels[dstX];
	if(blend2[bg_under])
	{
		alpha++;
		if(alpha<32)
		{
			//if the layer underneath is a blend bottom layer, then 3d always alpha blends with it
			COLOR c2, cfinal;

			c2.val = HostReadWord(dst, passing);

			cfinal.bits.red = ((red * alpha) + ((c2.bits.red<<1) * (32 - alpha)))>>6;
			cfinal.bits.green = ((green * alpha) + ((c2.bits.green<<1) * (32 - alpha)))>>6;
			cfinal.bits.blue = ((blue * alpha) + ((c2.bits.blue<<1) * (32 - alpha)))>>6;

			final = cfinal.val;
		}
		else final = R6G6B6TORGB15(red,green,blue);
	}
	else
	{
		final = R6G6B6TORGB15(red,green,blue);
		//perform the special effect
		if(windowEffect)
			switch(FUNC) {
				case Increase: final = currentFadeInColors[final&0x7FFF]; break;
				case Decrease: final = currentFadeOutColors[final&0x7FFF]; break;
				case NoBlend: 
				case Blend:
					break;
			}
	}

	HostWriteWord(dst, passing, (final | 0x8000));
	bgPixels[x] = 0;
}


template<bool BACKDROP, BlendFunc FUNC, bool WINDOW>
FORCEINLINE FASTCALL bool GPU::_master_setFinalBGColor(u16 &color, const u32 x)
{
	//no further analysis for no special effects. on backdrops. just draw it.
	if(FUNC==NoBlend && BACKDROP) return true;

	//blend backdrop with what?? this doesn't make sense
	if(FUNC==Blend && BACKDROP) return true;

	bool windowEffect = true;

	if(WINDOW)
	{
		bool windowDraw = false;
		renderline_checkWindows(x, windowDraw, windowEffect);

		//backdrop must always be drawn
		if(BACKDROP) windowDraw = true;

		//we never have anything more to do if the window rejected us
		if(!windowDraw) return false;
	}

	//special effects rejected. just draw it.
	if(!(blend1 && windowEffect))
		return true;

	const u8 bg_under = bgPixels[x];

	//perform the special effect
	switch(FUNC) {
		case Blend: if(blend2[bg_under]) color = blend(color,HostReadWord(currDst, x<<1)); break;
		case Increase: color = currentFadeInColors[color]; break;
		case Decrease: color = currentFadeOutColors[color]; break;
		case NoBlend: break;
	}
	return true;
}

template<BlendFunc FUNC, bool WINDOW>
static FORCEINLINE void _master_setFinalOBJColor(GPU *gpu, u8 *dst, u16 color, u8 alpha, u8 type, u16 x)
{
	bool windowDraw = true, windowEffect = true;

	if(WINDOW)
	{
		gpu->renderline_checkWindows(x, windowDraw, windowEffect);
		if(!windowDraw)
			return;
	}

	const bool sourceEffectSelected = gpu->blend1;

	//note that the fadein and fadeout is done here before blending, 
	//so that a fade and blending can be applied at the same time (actually, I don't think that is legal..)
	bool forceBlendingForNormal = false;
	if(windowEffect && sourceEffectSelected)
		switch(FUNC) 
		{
			//zero 13-jun-2010 : if(allowBlend) was removed from these;
			//it should be possible to increase/decrease and also blend
			//(the effect would be increase, but the obj properties permit blending and the target layers are configured correctly)
		case Increase: color = gpu->currentFadeInColors[color&0x7FFF]; break;
		case Decrease: color = gpu->currentFadeOutColors[color&0x7FFF]; break;

		//only when blend color effect is selected, ordinarily opaque sprites are blended with the color effect params
		case Blend: forceBlendingForNormal = true; break;
		case NoBlend: break;
		}

	//this inspects the layer beneath the sprite to see if the current blend flags make it a candidate for blending
	const int bg_under = gpu->bgPixels[x];
	const bool allowBlend = (bg_under != 4) && gpu->blend2[bg_under];

	if(allowBlend)
	{
		u16 backColor = HostReadWord(dst,x<<1);
		//this hasn't been tested: this blending occurs without regard to the color effect,
		//but rather purely from the sprite's alpha
		if(type == GPU_OBJ_MODE_Bitmap)
			color = _blend(color,backColor,&gpuBlendTable555[alpha+1][15-alpha]);
		else if(type == GPU_OBJ_MODE_Transparent || forceBlendingForNormal)
			color = gpu->blend(color,backColor);
	}

	HostWriteWord(dst, x<<1, (color | 0x8000));
	gpu->bgPixels[x] = 4;	
}

//FUNCNUM is only set for backdrop, for an optimization of looking it up early
template<bool BACKDROP, int FUNCNUM> 
FORCEINLINE void GPU::setFinalColorBG(u16 color, const u32 x)
{
	//It is not safe to assert this here.
	//This is probably the best place to enforce it, since almost every single color that comes in here
	//will be pulled from a palette that needs the top bit stripped off anyway.
	//assert((color&0x8000)==0);
	if(!BACKDROP) color &= 0x7FFF; //but for the backdrop we can easily guarantee earlier that theres no bit here

	bool draw;

	const int test = BACKDROP?FUNCNUM:setFinalColorBck_funcNum;
	switch(test)
	{
		case 0: draw = _master_setFinalBGColor<BACKDROP,NoBlend,false>(color,x); break;
		case 1: draw = _master_setFinalBGColor<BACKDROP,Blend,false>(color,x); break;
		case 2: draw = _master_setFinalBGColor<BACKDROP,Increase,false>(color,x); break;
		case 3: draw = _master_setFinalBGColor<BACKDROP,Decrease,false>(color,x); break;
		case 4: draw = _master_setFinalBGColor<BACKDROP,NoBlend,true>(color,x); break;
		case 5: draw = _master_setFinalBGColor<BACKDROP,Blend,true>(color,x); break;
		case 6: draw = _master_setFinalBGColor<BACKDROP,Increase,true>(color,x); break;
		case 7: draw = _master_setFinalBGColor<BACKDROP,Decrease,true>(color,x); break;
		default: draw = false; break;
	};

	if(BACKDROP || draw) //backdrop must always be drawn
	{
		HostWriteWord(currDst, x<<1, color | 0x8000);
		if(!BACKDROP) bgPixels[x] = currBgNum; //lets do this in the backdrop drawing loop, should be faster
	}
}


FORCEINLINE void GPU::setFinalColor3d(int dstX, int srcX)
{
	switch(setFinalColor3d_funcNum)
	{
	case 0x0: _master_setFinal3dColor<NoBlend,false>(dstX,srcX); break;
	case 0x1: _master_setFinal3dColor<Blend,false>(dstX,srcX); break;
	case 0x2: _master_setFinal3dColor<Increase,false>(dstX,srcX); break;
	case 0x3: _master_setFinal3dColor<Decrease,false>(dstX,srcX); break;
	case 0x4: _master_setFinal3dColor<NoBlend,true>(dstX,srcX); break;
	case 0x5: _master_setFinal3dColor<Blend,true>(dstX,srcX); break;
	case 0x6: _master_setFinal3dColor<Increase,true>(dstX,srcX); break;
	case 0x7: _master_setFinal3dColor<Decrease,true>(dstX,srcX); break;
	};
}

FORCEINLINE void setFinalColorSpr(GPU* gpu, u8 *dst, u16 color, u8 alpha, u8 type, u16 x)
{
	switch(gpu->setFinalColorSpr_funcNum)
	{
	case 0x0: _master_setFinalOBJColor<NoBlend,false>(gpu, dst, color, alpha, type, x); break;
	case 0x1: _master_setFinalOBJColor<Blend,false>(gpu, dst, color, alpha, type, x); break;
	case 0x2: _master_setFinalOBJColor<Increase,false>(gpu, dst, color, alpha, type, x); break;
	case 0x3: _master_setFinalOBJColor<Decrease,false>(gpu, dst, color, alpha, type, x); break;
	case 0x4: _master_setFinalOBJColor<NoBlend,true>(gpu, dst, color, alpha, type, x); break;
	case 0x5: _master_setFinalOBJColor<Blend,true>(gpu, dst, color, alpha, type, x); break;
	case 0x6: _master_setFinalOBJColor<Increase,true>(gpu, dst, color, alpha, type, x); break;
	case 0x7: _master_setFinalOBJColor<Decrease,true>(gpu, dst, color, alpha, type, x); break;
	};
}


/*****************************************************************************/
//			SCREEN FUNCTIONS
/*****************************************************************************/

int Screen_Init()
{
	MainScreen.gpu = GPU_Init(0);
	SubScreen.gpu = GPU_Init(1);

	memset(GPU_screen, 0, sizeof(GPU_screen));
	for(int i = 0; i < (256*192*2); i++)
		((u16*)GPU_screen)[i] = 0x7FFF;
	disp_fifo.head = disp_fifo.tail = 0;

	if (osd)  {delete osd; osd =NULL; }
	osd  = new OSDCLASS(-1);

	return 0;
}

void Screen_Reset(void)
{
	GPU_Reset(MainScreen.gpu, 0);
	GPU_Reset(SubScreen.gpu, 1);

	memset(GPU_screen, 0, sizeof(GPU_screen));
	for(int i = 0; i < (256*192*2); i++)
		((u16*)GPU_screen)[i] = 0x7FFF;

	disp_fifo.head = disp_fifo.tail = 0;
	osd->clear();
}

void Screen_DeInit(void)
{
	GPU_DeInit(MainScreen.gpu);
	GPU_DeInit(SubScreen.gpu);

	if (osd)  {delete osd; osd =NULL; }
}


/*****************************************************************************/
//			GPU_RenderLine
/*****************************************************************************/

void GPU_set_DISPCAPCNT(u32 val)
{
	GPU * gpu = MainScreen.gpu;	
   const display_control_t display_control = gpu->get_display_control();

	gpu->dispCapCnt.val = val;
	gpu->dispCapCnt.EVA = std::min((u32)16, (val & 0x1F));
	gpu->dispCapCnt.EVB = std::min((u32)16, ((val >> 8) & 0x1F));
	gpu->dispCapCnt.writeBlock =  (val >> 16) & 0x03;
	gpu->dispCapCnt.writeOffset = (val >> 18) & 0x03;
	gpu->dispCapCnt.readBlock = display_control.VRAM_Block;

	if (display_control.DisplayMode == 2)
		gpu->dispCapCnt.readOffset = 0;
	else
		gpu->dispCapCnt.readOffset = (val >> 26) & 0x03;
	
	gpu->dispCapCnt.srcA = (val >> 24) & 0x01;
	gpu->dispCapCnt.srcB = (val >> 25) & 0x01;
	gpu->dispCapCnt.capSrc = (val >> 29) & 0x03;

	switch((val >> 20) & 0x03)
	{
		case 0:
			gpu->dispCapCnt.capx = DISPCAPCNT::_128;
			gpu->dispCapCnt.capy = 128;
			break;
		case 1:
			gpu->dispCapCnt.capx = DISPCAPCNT::_256;
			gpu->dispCapCnt.capy = 64;
			break;
		case 2:
			gpu->dispCapCnt.capx = DISPCAPCNT::_256;
			gpu->dispCapCnt.capy = 128;
			break;
		case 3:
			gpu->dispCapCnt.capx = DISPCAPCNT::_256;
			gpu->dispCapCnt.capy = 192;
			break;
	}

	/*INFO("Capture 0x%X:\n EVA=%i, EVB=%i, wBlock=%i, wOffset=%i, capX=%i, capY=%i\n rBlock=%i, rOffset=%i, srcCap=%i, dst=0x%X, src=0x%X\n srcA=%i, srcB=%i\n\n",
			val, gpu->dispCapCnt.EVA, gpu->dispCapCnt.EVB, gpu->dispCapCnt.writeBlock, gpu->dispCapCnt.writeOffset,
			gpu->dispCapCnt.capx, gpu->dispCapCnt.capy, gpu->dispCapCnt.readBlock, gpu->dispCapCnt.readOffset, 
			gpu->dispCapCnt.capSrc, gpu->dispCapCnt.dst - MMU.ARM9_LCD, gpu->dispCapCnt.src - MMU.ARM9_LCD,
			gpu->dispCapCnt.srcA, gpu->dispCapCnt.srcB);*/
}

static void render_backdrop(GPU* gpu, u16 color)
{
	gpu->currBgNum = 5;

	//we need to write backdrop colors in the same way as we do BG pixels in order to do correct window processing
	//this is currently eating up 2fps or so. it is a reasonable candidate for optimization. 
   switch (gpu->setFinalColorBck_funcNum)
   {
      case 2: color = (gpu->BLDCNT & 0x20) ? gpu->currentFadeInColors[color] : color; break;
      case 3: color = (gpu->BLDCNT & 0x20) ? gpu->currentFadeOutColors[color] : color; break;
      case 4: for(int x = 0; x < 256; x ++) gpu->setFinalColorBG<true,4>(color, x); return;
      case 5: for(int x = 0; x < 256; x ++) gpu->setFinalColorBG<true,5>(color, x); return;
      case 6: for(int x = 0; x < 256; x ++) gpu->setFinalColorBG<true,6>(color, x); return;
      case 7: for(int x = 0; x < 256; x ++) gpu->setFinalColorBG<true,7>(color, x); return;
   }

   memset_u16_le<256>(gpu->currDst, color); 
	memset(gpu->bgPixels, 5, 256);
}

static void render_3d_line(GPU* gpu, u32 line)
{
   const u16 hofs = gpu->getHOFS(0);

   gfx3d_GetLineData(line, &gpu->_3dColorLine);
   u8* colorLine = gpu->_3dColorLine;

   for(int i = 0; i < 256; i ++)
   {
      int x = ((i + hofs) & 0x1FF);

      if(x >= 0 && x < 256 && colorLine[x * 4 + 3])
      {
         gpu->setFinalColor3d(i, x);
      }

      continue;
   }
}

static void GPU_RenderLine_layer(NDS_Screen * screen, u16 l)
{
	GPU * gpu = screen->gpu;
	const display_control_t display_control = gpu->get_display_control();

	gpu->currentFadeInColors = &fadeInColors[gpu->BLDY_EVY][0];
	gpu->currentFadeOutColors = &fadeOutColors[gpu->BLDY_EVY][0];

	const u16 backdrop_color = T1ReadWord(MMU.ARM9_VMEM, gpu->core * 0x400) & 0x7FFF;
   render_backdrop(gpu, backdrop_color);

	// init pixels priorities
	assert(NB_PRIORITIES==4);
	gpu->itemsForPriority[0].nbPixelsX = 0;
	gpu->itemsForPriority[1].nbPixelsX = 0;
	gpu->itemsForPriority[2].nbPixelsX = 0;
	gpu->itemsForPriority[3].nbPixelsX = 0;

   // reset blending state
	for(int i = 0; i < 8; i ++)
   {
		gpu->blend2[i] = (gpu->BLDCNT & (0x100 << i)) != 0;
   }

	// calculate sprite pixels and priorities for the line
   bool has_sprites = false;

	if (gpu->LayersEnable[4] && gpu->oam.render_line())
   {
      has_sprites = true;

      // assign them to the good priority item
      for(int i = 0; i < 256; i++) 
      {
         const u32 prio = gpu->oam.priority_at<false>(i);
         if (prio < 4)
         {
            itemsForPriority_t* item = &(gpu->itemsForPriority[prio]);
            item->PixelsX[item->nbPixelsX ++] = i;
         }
      }
	}

   // Draw all layers
   const bool all_bg_disabled = (!gpu->LayersEnable[0] && !gpu->LayersEnable[1] && !gpu->LayersEnable[2] && !gpu->LayersEnable[3]);

	for(int prio = NB_PRIORITIES - 1; prio >= 0; prio --)
	{
		itemsForPriority_t* item = &(gpu->itemsForPriority[prio]);

		// render BGs
		if (!all_bg_disabled)
		{
			for (int i = 0; i < item->nbBGs; i ++) 
			{
            const u32 bg_number = item->BGs[i];

				if (gpu->LayersEnable[bg_number])
				{
               gpu->currBgNum = bg_number;
					gpu->blend1 = (gpu->BLDCNT & (1 << bg_number)) != 0;

					if (bg_number == 0 && gpu->core == GPU_MAIN && display_control.BG0_3D)
					{
                  render_3d_line(gpu, l);
					}
					else 
               {
                  gpu->get_current_background().render_pixels(gpu->currLine);
               }
				}
			}
		}

      // render sprite Pixels
		if (has_sprites)
		{
			gpu->currBgNum = 4;
			gpu->blend1 = (gpu->BLDCNT & (1 << gpu->currBgNum))!=0;
			
			for (int i=0; i < item->nbPixelsX; i++)
			{
            PIXEL p = gpu->oam.line_buffer[item->PixelsX[i]];
				setFinalColorSpr(gpu, gpu->currDst, p.color, p.alpha, p.type, item->PixelsX[i]);
			}
		}
	}
}

template<bool SKIP> static void GPU_RenderLine_DispCapture(u16 l)
{
	//this macro takes advantage of the fact that there are only two possible values for capx
	#define CAPCOPY(SRC,DST,SETALPHABIT) \
	switch(gpu->dispCapCnt.capx) { \
		case DISPCAPCNT::_128: \
			for (int i = 0; i < 128; i++)  \
				HostWriteWord(DST, i << 1, HostReadWord(SRC, i << 1) | (SETALPHABIT?(1<<15):0)); \
			break; \
		case DISPCAPCNT::_256: \
			for (int i = 0; i < 256; i++)  \
				HostWriteWord(DST, i << 1, HostReadWord(SRC, i << 1) | (SETALPHABIT?(1<<15):0)); \
			break; \
			default: assert(false); \
		}
	
	GPU * gpu = MainScreen.gpu;

	if (l == 0)
	{
		if (gpu->dispCapCnt.val & 0x80000000)
		{
			gpu->dispCapCnt.enabled = TRUE;
			T1WriteLong(MMU.ARM9_REG, 0x64, gpu->dispCapCnt.val);
		}
	}

	bool skip = SKIP;

	if (gpu->dispCapCnt.enabled)
	{
		//128-wide captures should write linearly into memory, with no gaps
		//this is tested by hotel dusk
		u32 ofsmul = gpu->dispCapCnt.capx==DISPCAPCNT::_128?256:512;
		u32 cap_src_adr = gpu->dispCapCnt.readOffset * 0x8000 + (l * 512);
		u32 cap_dst_adr = gpu->dispCapCnt.writeOffset * 0x8000 + (l * ofsmul);

		//Read/Write block wrap to 00000h when exceeding 1FFFFh (128k)
		//this has not been tested yet (I thought I needed it for hotel dusk, but it was fixed by the above)
		cap_src_adr &= 0x1FFFF;
		cap_dst_adr &= 0x1FFFF;

		cap_src_adr += gpu->dispCapCnt.readBlock * 0x20000;
		cap_dst_adr += gpu->dispCapCnt.writeBlock * 0x20000;

		u8* cap_src = MMU.ARM9_LCD + cap_src_adr;
		u8* cap_dst = MMU.ARM9_LCD + cap_dst_adr;

		//we must block captures when the capture dest is not mapped to LCDC
		if(vramConfiguration.banks[gpu->dispCapCnt.writeBlock].purpose != VramConfiguration::LCDC)
			skip = true;

		//we must return zero from reads from memory not mapped to lcdc
		if(vramConfiguration.banks[gpu->dispCapCnt.readBlock].purpose != VramConfiguration::LCDC)
			cap_src = MMU.blank_memory;

		if(!skip)
		if (l < gpu->dispCapCnt.capy)
		{
			switch (gpu->dispCapCnt.capSrc)
			{
				case 0:		// Capture source is SourceA
					{
						//INFO("Capture source is SourceA\n");
						switch (gpu->dispCapCnt.srcA)
						{
							case 0:			// Capture screen (BG + OBJ + 3D)
								{
									//INFO("Capture screen (BG + OBJ + 3D)\n");

									u8 *src;
									src = (u8*)(gpu->currDst);
									CAPCOPY(src,cap_dst,true);
								}
							break;
							case 1:			// Capture 3D
								{
									//INFO("Capture 3D\n");
									u16* colorLine;
									gfx3d_GetLineData15bpp(l, &colorLine);
									CAPCOPY(((u8*)colorLine),cap_dst,false);
								}
							break;
						}
					}
				break;
				case 1:		// Capture source is SourceB
					{
						//INFO("Capture source is SourceB\n");
						switch (gpu->dispCapCnt.srcB)
						{
							case 0:	
								//Capture VRAM
								CAPCOPY(cap_src,cap_dst,true);
								break;
							case 1:
								//capture dispfifo
								//(not yet tested)
								for(int i=0; i < 128; i++)
									T1WriteLong(cap_dst, i << 2, DISP_FIFOrecv());
								break;
						}
					}
				break;
				default:	// Capture source is SourceA+B blended
					{
						//INFO("Capture source is SourceA+B blended\n");
						u16 *srcA = NULL;
						u16 *srcB = NULL;

						if (gpu->dispCapCnt.srcA == 0)
						{
							// Capture screen (BG + OBJ + 3D)
							srcA = (u16*)(gpu->currDst);
						}
						else
						{
							gfx3d_GetLineData15bpp(l, &srcA);
						}

						static u16 fifoLine[256];

						if (gpu->dispCapCnt.srcB == 0)			// VRAM screen
							srcB = (u16 *)cap_src;
						else
						{
							//fifo - tested by splinter cell chaos theory thermal view
							srcB = fifoLine;
							for (int i=0; i < 128; i++)
								T1WriteLong((u8*)srcB, i << 2, DISP_FIFOrecv());
						}


						const int todo = (gpu->dispCapCnt.capx==DISPCAPCNT::_128?128:256);

						for(u16 i = 0; i < todo; i++) 
						{
							u16 a,r,g,b;

							u16 a_alpha = srcA[i] & 0x8000;
							u16 b_alpha = srcB[i] & 0x8000;

							if(a_alpha)
							{
								a = 0x8000;
								r = ((srcA[i] & 0x1F) * gpu->dispCapCnt.EVA);
								g = (((srcA[i] >>  5) & 0x1F) * gpu->dispCapCnt.EVA);
								b = (((srcA[i] >>  10) & 0x1F) * gpu->dispCapCnt.EVA);
							} 
							else
								a = r = g = b = 0;

							if(b_alpha)
							{
								a = 0x8000;
								r += ((srcB[i] & 0x1F) * gpu->dispCapCnt.EVB);
								g += (((srcB[i] >>  5) & 0x1F) * gpu->dispCapCnt.EVB);
								b += (((srcB[i] >> 10) & 0x1F) * gpu->dispCapCnt.EVB);
							}

							r >>= 4;
							g >>= 4;
							b >>= 4;

							//freedom wings sky will overflow while doing some fsaa/motionblur effect without this
							r = std::min((u16)31,r);
							g = std::min((u16)31,g);
							b = std::min((u16)31,b);

							HostWriteWord(cap_dst, i << 1, a | (b << 10) | (g << 5) | r);
						}
					}
				break;
			}
		}

		if (l>=191)
		{
			gpu->dispCapCnt.enabled = FALSE;
			gpu->dispCapCnt.val &= 0x7FFFFFFF;
			T1WriteLong(MMU.ARM9_REG, 0x64, gpu->dispCapCnt.val);
			return;
		}
	}
}

static INLINE void GPU_RenderLine_MasterBrightness(NDS_Screen * screen, u16 l)
{
   // NOTE: This is a good candidate for vectorization

   const master_bright_t& master_bright = screen->gpu->dispx_st->master_bright;
	u16* dst = (u16*)(GPU_screen + (screen->offset + l) * 512);

   // It's turned off, nothing to do
   if (master_bright.mode == 0 || master_bright.mode == 3 || (master_bright.max == 0 && master_bright.factor == 0))
   {
      return;
   }
   // It's saturated to a single color
   else if (master_bright.max)
   {
      const u16 value = (master_bright.mode == 1) ? 0x7FFF : 0;
      for (int i = 0; i < 256; i ++)
      {
         dst[i] = value;
      }
   }
   // Needs lookup
   else
   {
      RARCH_PERFORMANCE_INIT(mbP);
      RARCH_PERFORMANCE_START(mbP);

#if 0
      if (master_bright.mode == 2)
      {
	      uint16x8_t* vector_dst = (uint16x8_t*)dst;

         for (int i = 0; i < 32; i ++)
         {
            NEONdarkenColor(&vector_dst[i], master_bright.factor);
         }
      }
      else
#endif
      {
         const u16* const fade_table = (master_bright.mode == 1) ? fadeInColors[master_bright.factor] : fadeOutColors[master_bright.factor];
         for (int i = 0; i < 256; i ++)
         {
            dst[i] = fade_table[dst[i] & 0x7FFF];
         }
      }

      RARCH_PERFORMANCE_STOP(mbP);
   }
}

template<int WIN_NUM>
FORCEINLINE void GPU::setup_windows()
{
	u8 y = currLine;
	u16 startY,endY;

	if(WIN_NUM==0)
	{
		startY = dispx_st->window_rects.win_0_y1;
		endY = dispx_st->window_rects.win_0_y2;
	}
	else
	{
		startY = dispx_st->window_rects.win_1_y1;
		endY = dispx_st->window_rects.win_1_y2;
	}

	if(WIN_NUM == 0 && !WIN0_ENABLED) goto allout;
	if(WIN_NUM == 1 && !WIN1_ENABLED) goto allout;

	if(startY > endY)
	{
		if((y < startY) && (y > endY)) goto allout;
	}
	else
	{
		if((y < startY) || (y >= endY)) goto allout;
	}

	//the x windows will apply for this scanline
	curr_win[WIN_NUM] = h_win[WIN_NUM];
	return;
	
allout:
	curr_win[WIN_NUM] = win_empty;
}

void GPU::update_winh(int WIN_NUM)
{
	//dont even waste any time in here if the window isnt enabled
	if(WIN_NUM==0 && !WIN0_ENABLED) return;
	if(WIN_NUM==1 && !WIN1_ENABLED) return;

	need_update_winh[WIN_NUM] = false;
	u16 startX,endX;

	if(WIN_NUM==0)
	{
		startX = dispx_st->window_rects.win_0_x1;
		endX = dispx_st->window_rects.win_0_x2;
	}
	else
	{
		startX = dispx_st->window_rects.win_1_x1;
		endX = dispx_st->window_rects.win_1_x2;
	}

	//the original logic: if you doubt the window code, please check it against the newer implementation below
	//if(startX > endX)
	//{
	//	if((x < startX) && (x > endX)) return false;
	//}
	//else
	//{
	//	if((x < startX) || (x >= endX)) return false;
	//}

	if(startX > endX)
	{
		for(int i=0;i<=endX;i++)
			h_win[WIN_NUM][i] = 1;
		for(int i=endX+1;i<startX;i++)
			h_win[WIN_NUM][i] = 0;
		for(int i=startX;i<256;i++)
			h_win[WIN_NUM][i] = 1;
	} else
	{
		for(int i=0;i<startX;i++)
			h_win[WIN_NUM][i] = 0;
		for(int i=startX;i<endX;i++)
			h_win[WIN_NUM][i] = 1;
		for(int i=endX;i<256;i++)
			h_win[WIN_NUM][i] = 0;
	}
}

void GPU_RenderLine(NDS_Screen * screen, u16 l, bool skip)
{
	GPU * gpu = screen->gpu;

	//here is some setup which is only done on line 0
	if(l == 0) {
		//this is speculative. the idea is as follows:
		//whenever the user updates the affine start position regs, it goes into the active regs immediately
		//(this is handled on the set event from MMU)
		//maybe it shouldnt take effect until the next hblank or something..
		//this is a based on a combination of:
		//heroes of mana intro FMV
		//SPP level 3-8 rotoscale room
		//NSMB raster fx backdrops
		//bubble bobble revolution classic mode
		//NOTE:
		//I am REALLY unsatisfied with this logic now. But it seems to be working..
		gpu->refreshAffineStartRegs(-1,-1);
	}

	if(skip)
	{
		gpu->currLine = l;
		if (gpu->core == GPU_MAIN) 
		{
			GPU_RenderLine_DispCapture<true>(l);
			if (l == 191) { disp_fifo.head = disp_fifo.tail = 0; }
		}
		return;
	}

	//blacken the screen if it is turned off by the user
	if(!CommonSettings.showGpu.screens[gpu->core])
	{
		u8 * dst =  GPU_screen + (screen->offset + l) * 512;
		memset(dst,0,512);

		return;
	}

	// skip some work if master brightness makes the screen completely white or completely black
   const master_bright_t& master_bright = gpu->dispx_st->master_bright;
	if(master_bright.max && (master_bright.mode == 1 || master_bright.mode == 2))
	{
		// except if it could cause any side effects (for example if we're capturing), then don't skip anything
		if(!(gpu->core == GPU_MAIN && (gpu->dispCapCnt.enabled || l == 0 || l == 191)))
		{
			gpu->currLine = l;
			GPU_RenderLine_MasterBrightness(screen, l);

			return;
		}
	}

   RARCH_PERFORMANCE_INIT(renderlineP);
   RARCH_PERFORMANCE_START(renderlineP);

	//cache some parameters which are assumed to be stable throughout the rendering of the entire line
	gpu->currLine = l;

	if(gpu->need_update_winh[0]) gpu->update_winh(0);
	if(gpu->need_update_winh[1]) gpu->update_winh(1);

	gpu->setup_windows<0>();
	gpu->setup_windows<1>();

	//generate the 2d engine output
   const display_control_t display_control = gpu->get_display_control();
   const u32 display_mode = display_control.DisplayMode & ((gpu->core) ? 1 : 3);

   gpu->currDst = (display_mode == 1) ? (u8*)(GPU_screen) + (screen->offset + l) * 512 : (u8*)gpu->tempScanlineBuffer;

	GPU_RenderLine_layer(screen, l);

	switch (display_mode)
	{
		case 0: // Display Off(Display white)
			{
				u8 * dst =  GPU_screen + (screen->offset + l) * 512;

				for (int i=0; i<256; i++)
					HostWriteWord(dst, i << 1, 0x7FFF);
			}
			break;

		case 1: // Display BG and OBJ layers
			//do nothing: it has already been generated into the right place
			break;

		case 2: // Display vram framebuffer
			{
				u8 * dst = GPU_screen + (screen->offset + l) * 512;
				u8 * src = (u8 *)MMU.ARM9_LCD + (display_control.VRAM_Block * 0x20000) + (l*512);
				memcpy (dst, src, 512);
			}
			break;
		case 3: // Display memory FIFO
			{
				//this has not been tested since the dma timing for dispfifo was changed around the time of
				//newemuloop. it may not work.
				u8 * dst =  GPU_screen + (screen->offset + l) * 512;
				for (int i=0; i < 128; i++)
					T1WriteLong(dst, i << 2, DISP_FIFOrecv() & 0x7FFF7FFF);
			}
			break;
	}

	//capture after displaying so that we can safely display vram before overwriting it here
	if (gpu->core == GPU_MAIN) 
	{
		//BUG!!! if someone is capturing and displaying both from the fifo, then it will have been 
		//consumed above by the display before we get here
		//(is that even legal? i think so)
		GPU_RenderLine_DispCapture<false>(l);
		if (l == 191) { disp_fifo.head = disp_fifo.tail = 0; }
	}


	GPU_RenderLine_MasterBrightness(screen, l);

   RARCH_PERFORMANCE_STOP(renderlineP);
}

void gpu_savestate(EMUFILE* os)
{
	//version
	write32le(1,os);
	
	os->fwrite((char*)GPU_screen,sizeof(GPU_screen));
	
	write32le(MainScreen.gpu->affineInfo[0].x,os);
	write32le(MainScreen.gpu->affineInfo[0].y,os);
	write32le(MainScreen.gpu->affineInfo[1].x,os);
	write32le(MainScreen.gpu->affineInfo[1].y,os);
	write32le(SubScreen.gpu->affineInfo[0].x,os);
	write32le(SubScreen.gpu->affineInfo[0].y,os);
	write32le(SubScreen.gpu->affineInfo[1].x,os);
	write32le(SubScreen.gpu->affineInfo[1].y,os);
}

bool gpu_loadstate(EMUFILE* is, int size)
{
	//read version
	u32 version;

	//sigh.. shouldve used a new version number
	if(size == 256*192*2*2)
		version = 0;
	else if(size== 0x30024)
	{
		read32le(&version,is);
		version = 1;
	}
	else
		if(read32le(&version,is) != 1) return false;
		

	if(version>1) return false;

	is->fread((char*)GPU_screen,sizeof(GPU_screen));

	if(version==1)
	{
		read32le(&MainScreen.gpu->affineInfo[0].x,is);
		read32le(&MainScreen.gpu->affineInfo[0].y,is);
		read32le(&MainScreen.gpu->affineInfo[1].x,is);
		read32le(&MainScreen.gpu->affineInfo[1].y,is);
		read32le(&SubScreen.gpu->affineInfo[0].x,is);
		read32le(&SubScreen.gpu->affineInfo[0].y,is);
		read32le(&SubScreen.gpu->affineInfo[1].x,is);
		read32le(&SubScreen.gpu->affineInfo[1].y,is);
		//removed per nitsuja feedback. anyway, this same thing will happen almost immediately in gpu line=0
		//MainScreen.gpu->refreshAffineStartRegs(-1,-1);
		//SubScreen.gpu->refreshAffineStartRegs(-1,-1);
	}

	MainScreen.gpu->updateBLDALPHA();
	SubScreen.gpu->updateBLDALPHA();
	return !is->fail();
}

u32 GPU::getAffineStart(int layer, int xy)
{
	if(xy==0) return affineInfo[layer-2].x;
	else return affineInfo[layer-2].y;
}

void GPU::setAffineStartWord(int layer, int xy, u16 val, int word)
{
	u32 curr = getAffineStart(layer,xy);
	if(word==0) curr = (curr&0xFFFF0000)|val;
	else curr = (curr&0x0000FFFF)|(((u32)val)<<16);
	setAffineStart(layer,xy,curr);
}

void GPU::setAffineStart(int layer, int xy, u32 val)
{
	if(xy==0) affineInfo[layer-2].x = val;
	else affineInfo[layer-2].y = val;
	refreshAffineStartRegs(layer,xy);
}

void GPU::refreshAffineStartRegs(const int num, const int xy)
{
	if(num==-1)
	{
		refreshAffineStartRegs(2,xy);
		refreshAffineStartRegs(3,xy);
		return;
	}

	if(xy==-1)
	{
		refreshAffineStartRegs(num,0);
		refreshAffineStartRegs(num,1);
		return;
	}

	affine_parameters_t& parms = backgrounds[num].get_affine_parameters();
   parms.X = affineInfo[num - 2].x;
   parms.Y = affineInfo[num - 2].y;
}

#include "GPU/background.inc"
#include "GPU/objects.inc"