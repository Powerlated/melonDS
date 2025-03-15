/*
    Copyright 2016-2024 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include "GPU2D_Soft.h"
#include "GPU.h"
#include "GPU3D.h"

namespace melonDS
{
    
namespace GPU2D
{

SoftRenderer::SoftRenderer()
{
    // mosaic table is initialized at compile-time
}

u32 SoftRenderer::ColorComposite(const UnitState *state, int i, u32 val1, u32 val2) const
{
    u32 coloreffect = 0;
    u32 eva, evb;

    u32 flag1 = val1 >> 24;
    u32 flag2 = val2 >> 24;

    u32 blendCnt = state->BlendCnt;

    u32 target2;
    if      (flag2 & 0x80) target2 = 0x1000;
    else if (flag2 & 0x40) target2 = 0x0100;
    else                   target2 = flag2 << 8;

    if ((flag1 & 0x80) && (blendCnt & target2))
    {
        // sprite blending

        coloreffect = 1;

        if (flag1 & 0x40)
        {
            eva = flag1 & 0x1F;
            evb = 16 - eva;
        }
        else
        {
            eva = state->EVA;
            evb = state->EVB;
        }
    }
    else if ((flag1 & 0x40) && (blendCnt & target2))
    {
        // 3D layer blending

        coloreffect = 4;
    }
    else
    {
        if      (flag1 & 0x80) flag1 = 0x10;
        else if (flag1 & 0x40) flag1 = 0x01;

        if ((blendCnt & flag1) && (state->Prepared->WindowMask[i] & 0x20))
        {
            coloreffect = (blendCnt >> 6) & 0x3;

            if (coloreffect == 1)
            {
                if (blendCnt & target2)
                {
                    eva = state->EVA;
                    evb = state->EVB;
                }
                else
                    coloreffect = 0;
            }
        }
    }

    switch (coloreffect)
    {
    case 0: return val1;
    case 1: return ColorBlend4(val1, val2, eva, evb);
    case 2: return ColorBrightnessUp(val1, state->EVY, 0x8);
    case 3: return ColorBrightnessDown(val1, state->EVY, 0x7);
    case 4: return ColorBlend5(val1, val2);
    }

    return val1;
}

void SoftRenderer::DrawScanline(const UnitState* state, u32 line)
{
    int stride = state->GPU3D_IsRendererAccelerated ? (256*3 + 1) : 256;
    u32* dst = &state->Framebuffer[stride * line];

    if (state->ForceBlank)
    {
        for (int i = 0; i < 256; i++)
            dst[i] = 0xFFFFFFFF;

        if (state->GPU3D_IsRendererAccelerated)
        {
            dst[256*3] = 0;
        }
        return;
    }

    u32 dispmode = state->DispCnt >> 16;
    dispmode &= (state->Num ? 0x1 : 0x3);

    // always render regular graphics
    DrawScanline_BGOBJ(state, line);

    switch (dispmode)
    {
    case 0: // screen off
        {
            for (int i = 0; i < 256; i++)
                dst[i] = 0x003F3F3F;
        }
        break;

    case 1: // regular display
        {
            int i = 0;
            for (; i < (stride & ~1); i+=2)
                *(u64*)&dst[i] = *(u64*)&BGOBJLine[i];
        }
        break;

    case 2: // VRAM display
        {
            for (int i = 0; i < 256; i++) {
                dst[i] = state->Prepared->RenderedVRAMDisplay[i];
            }
        }
        break;

    case 3: // FIFO display
        {
            for (int i = 0; i < 256; i++)
            {
                u16 color = state->DispFIFOBuffer[i];
                u8 r = (color & 0x001F) << 1;
                u8 g = (color & 0x03E0) >> 4;
                u8 b = (color & 0x7C00) >> 9;

                dst[i] = r | (g << 8) | (b << 16);
            }
        }
        break;
    }

    // capture
    if ((state->Num == 0) && state->CaptureLatch)
    {
        u32 capwidth, capheight;
        switch ((state->CaptureCnt >> 20) & 0x3)
        {
        case 0: capwidth = 128; capheight = 128; break;
        case 1: capwidth = 256; capheight = 64;  break;
        case 2: capwidth = 256; capheight = 128; break;
        case 3: capwidth = 256; capheight = 192; break;
        }

        if (line < capheight)
            DoCapture(state, line, capwidth);
    }

    u32 masterBrightness = state->MasterBrightness;

    if (state->GPU3D_IsRendererAccelerated)
    {
        const u32 xpos = state->GPU3D_RenderXPos;

        dst[256*3] = masterBrightness |
                     (state->DispCnt & 0x30000) |
                     (xpos << 24) | ((xpos & 0x100) << 15);
        return;
    }

    // master brightness
    if (dispmode != 0)
    {
        if ((masterBrightness >> 14) == 1)
        {
            // up
            u32 factor = masterBrightness & 0x1F;
            if (factor > 16) factor = 16;

            for (int i = 0; i < 256; i++)
            {
                dst[i] = ColorBrightnessUp(dst[i], factor, 0x0);
            }
        }
        else if ((masterBrightness >> 14) == 2)
        {
            // down
            u32 factor = masterBrightness & 0x1F;
            if (factor > 16) factor = 16;

            for (int i = 0; i < 256; i++)
            {
                dst[i] = ColorBrightnessDown(dst[i], factor, 0xF);
            }
        }
    }

    // convert to 32-bit BGRA
    // note: 32-bit RGBA would be more straightforward, but
    // BGRA seems to be more compatible (Direct2D soft, cairo...)
    for (int i = 0; i < 256; i+=2)
    {
        u64 c = *(u64*)&dst[i];

        u64 r = (c << 18) & 0xFC000000FC0000;
        u64 g = (c << 2) & 0xFC000000FC00;
        u64 b = (c >> 14) & 0xFC000000FC;
        c = r | g | b;

        *(u64*)&dst[i] = c | ((c & 0x00C0C0C000C0C0C0) >> 6) | 0xFF000000FF000000;
    }
}

void SoftRenderer::VBlankEnd(const UnitState* stateA, const UnitState* stateB)
{

}

void SoftRenderer::DoCapture(const UnitState *state, u32 line, u32 width)
{
    // TODO: Figure out display capture, how to get the VRAM back to the main thread
    u32 captureCnt = state->CaptureCnt;
    u32 dstvram = (captureCnt >> 16) & 0x3;

    // TODO: confirm this
    // it should work like VRAM display mode, which requires VRAM to be mapped to LCDC
    if (!(state->GPU_VRAMMap_LCDC & (1<<dstvram)))
        return;

    u16* dst = (u16*)state->GPU->VRAM[dstvram];
    u32 dstaddr = (((captureCnt >> 18) & 0x3) << 14) + (line * width);

    // TODO: handle 3D in GPU3D::CurrentRenderer->Accelerated mode!!

    u32* srcA;
    if (captureCnt & (1<<24))
    {
        srcA = state->_3DLine;
    }
    else
    {
        srcA = BGOBJLine;
        if (state->GPU3D_IsRendererAccelerated)
        {
            // in GPU3D::CurrentRenderer->Accelerated mode, compositing is normally done on the GPU
            // but when doing display capture, we do need the composited output
            // so we do it here

            for (int i = 0; i < 256; i++)
            {
                u32 val1 = BGOBJLine[i];
                u32 val2 = BGOBJLine[256+i];
                u32 val3 = BGOBJLine[512+i];

                u32 compmode = (val3 >> 24) & 0xF;

                if (compmode == 4)
                {
                    // 3D on top, blending

                    u32 _3dval = state->_3DLine[i];
                    if ((_3dval >> 24) > 0)
                        val1 = ColorBlend5(_3dval, val1);
                    else
                        val1 = val2;
                }
                else if (compmode == 1)
                {
                    // 3D on bottom, blending

                    u32 _3dval = state->_3DLine[i];
                    if ((_3dval >> 24) > 0)
                    {
                        u32 eva = (val3 >> 8) & 0x1F;
                        u32 evb = (val3 >> 16) & 0x1F;

                        val1 = ColorBlend4(val1, _3dval, eva, evb);
                    }
                    else
                        val1 = val2;
                }
                else if (compmode <= 3)
                {
                    // 3D on top, normal/fade

                    u32 _3dval = state->_3DLine[i];
                    if ((_3dval >> 24) > 0)
                    {
                        u32 evy = (val3 >> 8) & 0x1F;

                        val1 = _3dval;
                        if      (compmode == 2) val1 = ColorBrightnessUp(val1, evy, 0x8);
                        else if (compmode == 3) val1 = ColorBrightnessDown(val1, evy, 0x7);
                    }
                    else
                        val1 = val2;
                }

                BGOBJLine[i] = val1;
            }
        }
    }

    const u16* srcB = NULL;
    u32 srcBaddr = line * 256;

    if (captureCnt & (1<<25))
    {
        srcB = &state->DispFIFOBuffer[0];
        srcBaddr = 0;
    }
    else
    {
        u32 srcvram = (state->DispCnt >> 18) & 0x3;
        if (state->GPU_VRAMMap_LCDC & (1<<srcvram))
            srcB = (u16*)state->GPU->VRAM[srcvram];

        if (((state->DispCnt >> 16) & 0x3) != 2)
            srcBaddr += ((captureCnt >> 26) & 0x3) << 14;
    }

    dstaddr &= 0xFFFF;
    srcBaddr &= 0xFFFF;

    static_assert(VRAMDirtyGranularity == 512);
    state->GPU->VRAMDirty[dstvram][(dstaddr * 2) / VRAMDirtyGranularity] = true;

    switch ((captureCnt >> 29) & 0x3)
    {
    case 0: // source A
        {
            for (u32 i = 0; i < width; i++)
            {
                u32 val = srcA[i];

                // TODO: check what happens when alpha=0

                u32 r = (val >> 1) & 0x1F;
                u32 g = (val >> 9) & 0x1F;
                u32 b = (val >> 17) & 0x1F;
                u32 a = ((val >> 24) != 0) ? 0x8000 : 0;

                dst[dstaddr] = r | (g << 5) | (b << 10) | a;
                dstaddr = (dstaddr + 1) & 0xFFFF;
            }
        }
        break;

    case 1: // source B
        {
            if (srcB)
            {
                for (u32 i = 0; i < width; i++)
                {
                    dst[dstaddr] = srcB[srcBaddr];
                    srcBaddr = (srcBaddr + 1) & 0xFFFF;
                    dstaddr = (dstaddr + 1) & 0xFFFF;
                }
            }
            else
            {
                for (u32 i = 0; i < width; i++)
                {
                    dst[dstaddr] = 0;
                    dstaddr = (dstaddr + 1) & 0xFFFF;
                }
            }
        }
        break;

    case 2: // sources A+B
    case 3:
        {
            u32 eva = captureCnt & 0x1F;
            u32 evb = (captureCnt >> 8) & 0x1F;

            // checkme
            if (eva > 16) eva = 16;
            if (evb > 16) evb = 16;

            if (srcB)
            {
                for (u32 i = 0; i < width; i++)
                {
                    u32 val = srcA[i];

                    // TODO: check what happens when alpha=0

                    u32 rA = (val >> 1) & 0x1F;
                    u32 gA = (val >> 9) & 0x1F;
                    u32 bA = (val >> 17) & 0x1F;
                    u32 aA = ((val >> 24) != 0) ? 1 : 0;

                    val = srcB[srcBaddr];

                    u32 rB = val & 0x1F;
                    u32 gB = (val >> 5) & 0x1F;
                    u32 bB = (val >> 10) & 0x1F;
                    u32 aB = val >> 15;

                    u32 rD = ((rA * aA * eva) + (rB * aB * evb) + 8) >> 4;
                    u32 gD = ((gA * aA * eva) + (gB * aB * evb) + 8) >> 4;
                    u32 bD = ((bA * aA * eva) + (bB * aB * evb) + 8) >> 4;
                    u32 aD = (eva>0 ? aA : 0) | (evb>0 ? aB : 0);

                    if (rD > 0x1F) rD = 0x1F;
                    if (gD > 0x1F) gD = 0x1F;
                    if (bD > 0x1F) bD = 0x1F;

                    dst[dstaddr] = rD | (gD << 5) | (bD << 10) | (aD << 15);
                    srcBaddr = (srcBaddr + 1) & 0xFFFF;
                    dstaddr = (dstaddr + 1) & 0xFFFF;
                }
            }
            else
            {
                for (u32 i = 0; i < width; i++)
                {
                    u32 val = srcA[i];

                    // TODO: check what happens when alpha=0

                    u32 rA = (val >> 1) & 0x1F;
                    u32 gA = (val >> 9) & 0x1F;
                    u32 bA = (val >> 17) & 0x1F;
                    u32 aA = ((val >> 24) != 0) ? 1 : 0;

                    u32 rD = ((rA * aA * eva) + 8) >> 4;
                    u32 gD = ((gA * aA * eva) + 8) >> 4;
                    u32 bD = ((bA * aA * eva) + 8) >> 4;
                    u32 aD = (eva>0 ? aA : 0);

                    dst[dstaddr] = rD | (gD << 5) | (bD << 10) | (aD << 15);
                    dstaddr = (dstaddr + 1) & 0xFFFF;
                }
            }
        }
        break;
    }
}

#define DoDrawBG(type, line, num) \
    do \
    { \
        if ((bgCnt[num] & 0x0040) && (state->BGMosaicSize[0] > 0)) \
        { \
            if (state->GPU3D_IsRendererAccelerated) DrawBG_##type<true, DrawPixel_Accel>(state, line, num); \
            else DrawBG_##type<true, DrawPixel_Normal>(state, line, num); \
        } \
        else \
        { \
            if (state->GPU3D_IsRendererAccelerated) DrawBG_##type<false, DrawPixel_Accel>(state, line, num); \
            else DrawBG_##type<false, DrawPixel_Normal>(state, line, num); \
        } \
    } while (false)

#define DoDrawBG_Large(line) \
    do \
    { \
        if ((bgCnt[2] & 0x0040) && (state->BGMosaicSize[0] > 0)) \
        { \
            if (state->GPU3D_IsRendererAccelerated) DrawBG_Large<true, DrawPixel_Accel>(state, line); \
            else DrawBG_Large<true, DrawPixel_Normal>(state, line); \
        } \
        else \
        { \
            if (state->GPU3D_IsRendererAccelerated) DrawBG_Large<false, DrawPixel_Accel>(state, line); \
            else DrawBG_Large<false, DrawPixel_Normal>(state, line); \
        } \
    } while (false)

#define DoInterleaveSprites(prio) \
    if (state->GPU3D_IsRendererAccelerated) InterleaveSprites<DrawPixel_Accel>(state, prio); else InterleaveSprites<DrawPixel_Normal>(state, prio);

template<u32 bgmode>
void SoftRenderer::DrawScanlineBGMode(const UnitState *state, u32 line)
{
    u32 dispCnt = state->DispCnt;
    const u16* bgCnt = state->BGCnt;
    for (int i = 3; i >= 0; i--)
    {
        if ((bgCnt[3] & 0x3) == i)
        {
            if (dispCnt & 0x0800)
            {
                if (bgmode >= 3)
                    DoDrawBG(Extended, line, 3);
                else if (bgmode >= 1)
                    DoDrawBG(Affine, line, 3);
                else
                    DoDrawBG(Text, line, 3);
            }
        }
        if ((bgCnt[2] & 0x3) == i)
        {
            if (dispCnt & 0x0400)
            {
                if (bgmode == 5)
                    DoDrawBG(Extended, line, 2);
                else if (bgmode == 4 || bgmode == 2)
                    DoDrawBG(Affine, line, 2);
                else
                    DoDrawBG(Text, line, 2);
            }
        }
        if ((bgCnt[1] & 0x3) == i)
        {
            if (dispCnt & 0x0200)
            {
                DoDrawBG(Text, line, 1);
            }
        }
        if ((bgCnt[0] & 0x3) == i)
        {
            if (dispCnt & 0x0100)
            {
                if (!state->Num && (dispCnt & 0x8))
                    DrawBG_3D(state);
                else
                    DoDrawBG(Text, line, 0);
            }
        }
        if ((dispCnt & 0x1000) && state->Shared->NumSprites)
        {
            DoInterleaveSprites(0x40000 | (i<<16));
        }

    }
}

void SoftRenderer::DrawScanlineBGMode6(const UnitState *state, u32 line)
{
    u32 dispCnt = state->DispCnt;
    const u16* bgCnt = state->BGCnt;
    for (int i = 3; i >= 0; i--)
    {
        if ((bgCnt[2] & 0x3) == i)
        {
            if (dispCnt & 0x0400)
            {
                DoDrawBG_Large(line);
            }
        }
        if ((bgCnt[0] & 0x3) == i)
        {
            if (dispCnt & 0x0100)
            {
                if ((!state->Num) && (dispCnt & 0x8))
                    DrawBG_3D(state);
            }
        }
        if ((dispCnt & 0x1000) && state->Shared->NumSprites)
        {
            DoInterleaveSprites(0x40000 | (i<<16))
        }
    }
}

void SoftRenderer::DrawScanlineBGMode7(const UnitState *state, u32 line)
{
    u32 dispCnt = state->DispCnt;
    const u16* bgCnt = state->BGCnt;
    // mode 7 only has text-mode BG0 and BG1

    for (int i = 3; i >= 0; i--)
    {
        if ((bgCnt[1] & 0x3) == i)
        {
            if (dispCnt & 0x0200)
            {
                DoDrawBG(Text, line, 1);
            }
        }
        if ((bgCnt[0] & 0x3) == i)
        {
            if (dispCnt & 0x0100)
            {
                if (!state->Num && (dispCnt & 0x8))
                    DrawBG_3D(state);
                else
                    DoDrawBG(Text, line, 0);
            }
        }
        if ((dispCnt & 0x1000) && state->Shared->NumSprites)
        {
            DoInterleaveSprites(0x40000 | (i<<16))
        }
    }
}

void SoftRenderer::DrawScanline_BGOBJ(const UnitState *state, u32 line)
{
    // forced blank disables BG/OBJ compositing
    if (state->DispCnt & (1<<7))
    {
        for (int i = 0; i < 256; i++)
            BGOBJLine[i] = 0xFF3F3F3F;
        return;
    }

    u64 backdrop = *(u16*)&state->Palette[0];

    {
        u8 r = (backdrop & 0x001F) << 1;
        u8 g = (backdrop & 0x03E0) >> 4;
        u8 b = (backdrop & 0x7C00) >> 9;

        backdrop = r | (g << 8) | (b << 16) | 0x20000000;
        backdrop |= (backdrop << 32);

        for (int i = 0; i < 256; i+=2)
            *(u64*)&BGOBJLine[i] = backdrop;
    }

    ApplySpriteMosaicX(state);
    CurBGXMosaicTable = MosaicTable[state->BGMosaicSize[0]].data();

    switch (state->DispCnt & 0x7)
    {
    case 0: DrawScanlineBGMode<0>(state, line); break;
    case 1: DrawScanlineBGMode<1>(state, line); break;
    case 2: DrawScanlineBGMode<2>(state, line); break;
    case 3: DrawScanlineBGMode<3>(state, line); break;
    case 4: DrawScanlineBGMode<4>(state, line); break;
    case 5: DrawScanlineBGMode<5>(state, line); break;
    case 6: DrawScanlineBGMode6(state, line); break;
    case 7: DrawScanlineBGMode7(state, line); break;
    }

    // color special effects
    // can likely be optimized

    if (!state->GPU3D_IsRendererAccelerated)
    {
        for (int i = 0; i < 256; i++)
        {
            u32 val1 = BGOBJLine[i];
            u32 val2 = BGOBJLine[256+i];

            BGOBJLine[i] = ColorComposite(state, i, val1, val2);
        }
    }
    else
    {
        if (state->Num == 0)
        {
            for (int i = 0; i < 256; i++)
            {
                u32 val1 = BGOBJLine[i];
                u32 val2 = BGOBJLine[256+i];
                u32 val3 = BGOBJLine[512+i];

                u32 flag1 = val1 >> 24;
                u32 flag2 = val2 >> 24;

                u32 bldcnteffect = (state->BlendCnt >> 6) & 0x3;

                u32 target1;
                if      (flag1 & 0x80) target1 = 0x0010;
                else if (flag1 & 0x40) target1 = 0x0001;
                else                   target1 = flag1;

                u32 target2;
                if      (flag2 & 0x80) target2 = 0x1000;
                else if (flag2 & 0x40) target2 = 0x0100;
                else                   target2 = flag2 << 8;

                if (((flag1 & 0xC0) == 0x40) && (state->BlendCnt & target2))
                {
                    // 3D on top, blending

                    BGOBJLine[i]     = val2;
                    BGOBJLine[256+i] = ColorComposite(state, i, val2, val3);
                    BGOBJLine[512+i] = 0x04000000;
                }
                else if ((flag1 & 0xC0) == 0x40)
                {
                    // 3D on top, normal/fade

                    if (bldcnteffect == 1)             bldcnteffect = 0;
                    if (!(state->BlendCnt & 0x0001)) bldcnteffect = 0;
                    if (!(state->Prepared->WindowMask[i] & 0x20))       bldcnteffect = 0;

                    BGOBJLine[i]     = val2;
                    BGOBJLine[256+i] = ColorComposite(state, i, val2, val3);
                    BGOBJLine[512+i] = (bldcnteffect << 24) | (state->EVY << 8);
                }
                else if (((flag2 & 0xC0) == 0x40) && ((state->BlendCnt & 0x01C0) == 0x0140))
                {
                    // 3D on bottom, blending

                    u32 eva, evb;
                    if ((flag1 & 0xC0) == 0xC0)
                    {
                        eva = flag1 & 0x1F;
                        evb = 16 - eva;
                    }
                    else if (((state->BlendCnt & target1) && (state->Prepared->WindowMask[i] & 0x20)) ||
                            ((flag1 & 0xC0) == 0x80))
                    {
                        eva = state->EVA;
                        evb = state->EVB;
                    }
                    else
                        bldcnteffect = 7;

                    BGOBJLine[i]     = val1;
                    BGOBJLine[256+i] = ColorComposite(state, i, val1, val3);
                    BGOBJLine[512+i] = (bldcnteffect << 24) | (state->EVB << 16) | (state->EVA << 8);
                }
                else
                {
                    // no potential 3D pixel involved

                    BGOBJLine[i]     = ColorComposite(state, i, val1, val2);
                    BGOBJLine[256+i] = 0;
                    BGOBJLine[512+i] = 0x07000000;
                }
            }
        }
        else
        {
            for (int i = 0; i < 256; i++)
            {
                u32 val1 = BGOBJLine[i];
                u32 val2 = BGOBJLine[256+i];

                BGOBJLine[i]     = ColorComposite(state, i, val1, val2);
                BGOBJLine[256+i] = 0;
                BGOBJLine[512+i] = 0x07000000;
            }
        }
    }
}


void SoftRenderer::DrawPixel_Normal(u32* dst, u16 color, u32 flag)
{
    u8 r = (color & 0x001F) << 1;
    u8 g = (color & 0x03E0) >> 4;
    u8 b = (color & 0x7C00) >> 9;
    //g |= ((color & 0x8000) >> 15);

    *(dst+256) = *dst;
    *dst = r | (g << 8) | (b << 16) | flag;
}

void SoftRenderer::DrawPixel_Accel(u32* dst, u16 color, u32 flag)
{
    u8 r = (color & 0x001F) << 1;
    u8 g = (color & 0x03E0) >> 4;
    u8 b = (color & 0x7C00) >> 9;

    *(dst+512) = *(dst+256);
    *(dst+256) = *dst;
    *dst = r | (g << 8) | (b << 16) | flag;
}

void SoftRenderer::DrawBG_3D(const UnitState *state)
{
    int i = 0;

    if (state->GPU3D_IsRendererAccelerated)
    {
        for (i = 0; i < 256; i++)
        {
            if (!(state->Prepared->WindowMask[i] & 0x01)) continue;

            BGOBJLine[i+512] = BGOBJLine[i+256];
            BGOBJLine[i+256] = BGOBJLine[i];
            BGOBJLine[i] = 0x40000000; // 3D-layer placeholder
        }
    }
    else
    {
        for (i = 0; i < 256; i++)
        {
            u32 c = state->_3DLine[i];

            if ((c >> 24) == 0) continue;
            if (!(state->Prepared->WindowMask[i] & 0x01)) continue;

            BGOBJLine[i+256] = BGOBJLine[i];
            BGOBJLine[i] = c | 0x40000000;
        }
    }
}

template<bool mosaic, SoftRenderer::DrawPixel drawPixel>
void SoftRenderer::DrawBG_Text(const UnitState *state, u32 line, u32 bgnum)
{
    // workaround for backgrounds missing on aarch64 with lto build
    asm volatile ("" : : : "memory");

    u16 bgcnt = state->BGCnt[bgnum];

    u32 tilesetaddr, tilemapaddr;
    u16* pal = (u16*)state->Palette;
    u32 extpal, extpalslot;

    u16 xoff = state->BGXPos[bgnum];
    u16 yoff = state->BGYPos[bgnum] + line;

    if (bgcnt & 0x0040)
    {
        // vertical mosaic
        yoff -= state->BGMosaicY;
    }

    u32 widexmask = (bgcnt & 0x4000) ? 0x100 : 0;

    extpal = (state->DispCnt & 0x40000000);
    if (extpal) extpalslot = ((bgnum<2) && (bgcnt&0x2000)) ? (2+bgnum) : bgnum;

    u8* bgvram;
    u32 bgvrammask;
    GetBGVRAM(state, bgvram, bgvrammask);
    if (state->Num)
    {
        tilesetaddr = ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((bgcnt & 0x1F00) << 3);
    }
    else
    {
        tilesetaddr = ((state->DispCnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((state->DispCnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3);
    }

    // adjust Y position in tilemap
    if (bgcnt & 0x8000)
    {
        tilemapaddr += ((yoff & 0x1F8) << 3);
        if (bgcnt & 0x4000)
            tilemapaddr += ((yoff & 0x100) << 3);
    }
    else
        tilemapaddr += ((yoff & 0xF8) << 3);

    u16 curtile;
    u16* curpal;
    u32 pixelsaddr;
    u8 color;
    u32 lastxpos;

    if (bgcnt & 0x0080)
    {
        // 256-color

        // preload shit as needed
        if ((xoff & 0x7) || mosaic)
        {
            curtile = *(u16*)&bgvram[(tilemapaddr + ((xoff & 0xF8) >> 2) + ((xoff & widexmask) << 3)) & bgvrammask];

            if (extpal) curpal = GetBGExtPal(state, extpalslot, curtile>>12);
            else        curpal = pal;

            pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 6)
                                     + (((curtile & 0x0800) ? (7-(yoff&0x7)) : (yoff&0x7)) << 3);
        }

        if (mosaic) lastxpos = xoff;

        for (int i = 0; i < 256; i++)
        {
            u32 xpos;
            if (mosaic) xpos = xoff - CurBGXMosaicTable[i];
            else        xpos = xoff;

            if ((!mosaic && (!(xpos & 0x7))) ||
                (mosaic && ((xpos >> 3) != (lastxpos >> 3))))
            {
                // load a new tile
                curtile = *(u16*)&bgvram[(tilemapaddr + ((xpos & 0xF8) >> 2) + ((xpos & widexmask) << 3)) & bgvrammask];

                if (extpal) curpal = GetBGExtPal(state, extpalslot, curtile>>12);
                else        curpal = pal;

                pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 6)
                                         + (((curtile & 0x0800) ? (7-(yoff&0x7)) : (yoff&0x7)) << 3);

                if (mosaic) lastxpos = xpos;
            }

            // draw pixel
            if (state->Prepared->WindowMask[i] & (1<<bgnum))
            {
                u32 tilexoff = (curtile & 0x0400) ? (7-(xpos&0x7)) : (xpos&0x7);
                color = bgvram[(pixelsaddr + tilexoff) & bgvrammask];

                if (color)
                    drawPixel(&BGOBJLine[i], curpal[color], 0x01000000<<bgnum);
            }

            xoff++;
        }
    }
    else
    {
        // 16-color

        // preload shit as needed
        if ((xoff & 0x7) || mosaic)
        {
            curtile = *(u16*)&bgvram[((tilemapaddr + ((xoff & 0xF8) >> 2) + ((xoff & widexmask) << 3))) & bgvrammask];
            curpal = pal + ((curtile & 0xF000) >> 8);
            pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 5)
                                     + (((curtile & 0x0800) ? (7-(yoff&0x7)) : (yoff&0x7)) << 2);
        }

        if (mosaic) lastxpos = xoff;

        for (int i = 0; i < 256; i++)
        {
            u32 xpos;
            if (mosaic) xpos = xoff - CurBGXMosaicTable[i];
            else        xpos = xoff;

            if ((!mosaic && (!(xpos & 0x7))) ||
                (mosaic && ((xpos >> 3) != (lastxpos >> 3))))
            {
                // load a new tile
                curtile = *(u16*)&bgvram[(tilemapaddr + ((xpos & 0xF8) >> 2) + ((xpos & widexmask) << 3)) & bgvrammask];
                curpal = pal + ((curtile & 0xF000) >> 8);
                pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 5)
                                         + (((curtile & 0x0800) ? (7-(yoff&0x7)) : (yoff&0x7)) << 2);

                if (mosaic) lastxpos = xpos;
            }

            // draw pixel
            if (state->Prepared->WindowMask[i] & (1<<bgnum))
            {
                u32 tilexoff = (curtile & 0x0400) ? (7-(xpos&0x7)) : (xpos&0x7);
                if (tilexoff & 0x1)
                {
                    color = bgvram[(pixelsaddr + (tilexoff >> 1)) & bgvrammask] >> 4;
                }
                else
                {
                    color = bgvram[(pixelsaddr + (tilexoff >> 1)) & bgvrammask] & 0x0F;
                }

                if (color)
                    drawPixel(&BGOBJLine[i], curpal[color], 0x01000000<<bgnum);
            }

            xoff++;
        }
    }
}

template<bool mosaic, SoftRenderer::DrawPixel drawPixel>
void SoftRenderer::DrawBG_Affine(const UnitState *state, u32 line, u32 bgnum)
{
    u16 bgcnt = state->BGCnt[bgnum];

    u32 tilesetaddr, tilemapaddr;
    u16* pal = (u16*)state->Palette;

    u32 coordmask;
    u32 yshift;
    switch (bgcnt & 0xC000)
    {
    case 0x0000: coordmask = 0x07800; yshift = 7; break;
    case 0x4000: coordmask = 0x0F800; yshift = 8; break;
    case 0x8000: coordmask = 0x1F800; yshift = 9; break;
    case 0xC000: coordmask = 0x3F800; yshift = 10; break;
    }

    u32 overflowmask;
    if (bgcnt & 0x2000) overflowmask = 0;
    else                overflowmask = ~(coordmask | 0x7FF);

    s16 rotA = state->BGRotA[bgnum-2];
    s16 rotB = state->BGRotB[bgnum-2];
    s16 rotC = state->BGRotC[bgnum-2];
    s16 rotD = state->BGRotD[bgnum-2];

    s32 rotX = state->BGXRefInternal[bgnum-2];
    s32 rotY = state->BGYRefInternal[bgnum-2];

    if (bgcnt & 0x0040)
    {
        // vertical mosaic
        rotX -= (state->BGMosaicY * rotB);
        rotY -= (state->BGMosaicY * rotD);
    }

    u8* bgvram;
    u32 bgvrammask;
    GetBGVRAM(state, bgvram, bgvrammask);

    if (state->Num)
    {
        tilesetaddr = ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((bgcnt & 0x1F00) << 3);
    }
    else
    {
        tilesetaddr = ((state->DispCnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((state->DispCnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3);
    }

    u16 curtile;
    u8 color;

    yshift -= 3;

    for (int i = 0; i < 256; i++)
    {
        if (state->Prepared->WindowMask[i] & (1<<bgnum))
        {
            s32 finalX, finalY;
            if (mosaic)
            {
                int im = CurBGXMosaicTable[i];
                finalX = rotX - (im * rotA);
                finalY = rotY - (im * rotC);
            }
            else
            {
                finalX = rotX;
                finalY = rotY;
            }

            if ((!((finalX|finalY) & overflowmask)))
            {
                curtile = bgvram[(tilemapaddr + ((((finalY & coordmask) >> 11) << yshift) + ((finalX & coordmask) >> 11))) & bgvrammask];

                // draw pixel
                u32 tilexoff = (finalX >> 8) & 0x7;
                u32 tileyoff = (finalY >> 8) & 0x7;

                color = bgvram[(tilesetaddr + (curtile << 6) + (tileyoff << 3) + tilexoff) & bgvrammask];

                if (color)
                    drawPixel(&BGOBJLine[i], pal[color], 0x01000000<<bgnum);
            }
        }

        rotX += rotA;
        rotY += rotC;
    }
}

template<bool mosaic, SoftRenderer::DrawPixel drawPixel>
void SoftRenderer::DrawBG_Extended(const UnitState *state, u32 line, u32 bgnum)
{
    u16 bgcnt = state->BGCnt[bgnum];

    u32 tilesetaddr, tilemapaddr;
    u16* pal = (u16*)state->Palette;
    u32 extpal;

    u8* bgvram;
    u32 bgvrammask;
    GetBGVRAM(state, bgvram, bgvrammask);

    extpal = (state->DispCnt & 0x40000000);

    s16 rotA = state->BGRotA[bgnum-2];
    s16 rotB = state->BGRotB[bgnum-2];
    s16 rotC = state->BGRotC[bgnum-2];
    s16 rotD = state->BGRotD[bgnum-2];

    s32 rotX = state->BGXRefInternal[bgnum-2];
    s32 rotY = state->BGYRefInternal[bgnum-2];

    if (bgcnt & 0x0040)
    {
        // vertical mosaic
        rotX -= (state->BGMosaicY * rotB);
        rotY -= (state->BGMosaicY * rotD);
    }

    if (bgcnt & 0x0080)
    {
        // bitmap modes

        u32 xmask, ymask;
        u32 yshift;
        switch (bgcnt & 0xC000)
        {
        case 0x0000: xmask = 0x07FFF; ymask = 0x07FFF; yshift = 7; break;
        case 0x4000: xmask = 0x0FFFF; ymask = 0x0FFFF; yshift = 8; break;
        case 0x8000: xmask = 0x1FFFF; ymask = 0x0FFFF; yshift = 9; break;
        case 0xC000: xmask = 0x1FFFF; ymask = 0x1FFFF; yshift = 9; break;
        }

        u32 ofxmask, ofymask;
        if (bgcnt & 0x2000)
        {
            ofxmask = 0;
            ofymask = 0;
        }
        else
        {
            ofxmask = ~xmask;
            ofymask = ~ymask;
        }

        if (state->Num) tilemapaddr = ((bgcnt & 0x1F00) << 6);
        else              tilemapaddr = ((bgcnt & 0x1F00) << 6);

        if (bgcnt & 0x0004)
        {
            // direct color bitmap

            u16 color;

            for (int i = 0; i < 256; i++)
            {
                if (state->Prepared->WindowMask[i] & (1<<bgnum))
                {
                    s32 finalX, finalY;
                    if (mosaic)
                    {
                        int im = CurBGXMosaicTable[i];
                        finalX = rotX - (im * rotA);
                        finalY = rotY - (im * rotC);
                    }
                    else
                    {
                        finalX = rotX;
                        finalY = rotY;
                    }

                    if (!(finalX & ofxmask) && !(finalY & ofymask))
                    {
                        color = *(u16*)&bgvram[(tilemapaddr + (((((finalY & ymask) >> 8) << yshift) + ((finalX & xmask) >> 8)) << 1)) & bgvrammask];

                        if (color & 0x8000)
                            drawPixel(&BGOBJLine[i], color, 0x01000000<<bgnum);
                    }
                }

                rotX += rotA;
                rotY += rotC;
            }
        }
        else
        {
            // 256-color bitmap

            u8 color;

            for (int i = 0; i < 256; i++)
            {
                if (state->Prepared->WindowMask[i] & (1<<bgnum))
                {
                    s32 finalX, finalY;
                    if (mosaic)
                    {
                        int im = CurBGXMosaicTable[i];
                        finalX = rotX - (im * rotA);
                        finalY = rotY - (im * rotC);
                    }
                    else
                    {
                        finalX = rotX;
                        finalY = rotY;
                    }

                    if (!(finalX & ofxmask) && !(finalY & ofymask))
                    {
                        color = bgvram[(tilemapaddr + (((finalY & ymask) >> 8) << yshift) + ((finalX & xmask) >> 8)) & bgvrammask];

                        if (color)
                            drawPixel(&BGOBJLine[i], pal[color], 0x01000000<<bgnum);
                    }
                }

                rotX += rotA;
                rotY += rotC;
            }
        }
    }
    else
    {
        // mixed affine/text mode

        u32 coordmask;
        u32 yshift;
        switch (bgcnt & 0xC000)
        {
        case 0x0000: coordmask = 0x07800; yshift = 7; break;
        case 0x4000: coordmask = 0x0F800; yshift = 8; break;
        case 0x8000: coordmask = 0x1F800; yshift = 9; break;
        case 0xC000: coordmask = 0x3F800; yshift = 10; break;
        }

        u32 overflowmask;
        if (bgcnt & 0x2000) overflowmask = 0;
        else                overflowmask = ~(coordmask | 0x7FF);

        if (state->Num)
        {
            tilesetaddr = ((bgcnt & 0x003C) << 12);
            tilemapaddr = ((bgcnt & 0x1F00) << 3);
        }
        else
        {
            tilesetaddr = ((state->DispCnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12);
            tilemapaddr = ((state->DispCnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3);
        }

        u16 curtile;
        u16* curpal;
        u8 color;

        yshift -= 3;

        for (int i = 0; i < 256; i++)
        {
            if (state->Prepared->WindowMask[i] & (1<<bgnum))
            {
                s32 finalX, finalY;
                if (mosaic)
                {
                    int im = CurBGXMosaicTable[i];
                    finalX = rotX - (im * rotA);
                    finalY = rotY - (im * rotC);
                }
                else
                {
                    finalX = rotX;
                    finalY = rotY;
                }

                if ((!((finalX|finalY) & overflowmask)))
                {
                    curtile = *(u16*)&bgvram[(tilemapaddr + (((((finalY & coordmask) >> 11) << yshift) + ((finalX & coordmask) >> 11)) << 1)) & bgvrammask];

                    if (extpal) curpal = GetBGExtPal(state, bgnum, curtile>>12);
                    else        curpal = pal;

                    // draw pixel
                    u32 tilexoff = (finalX >> 8) & 0x7;
                    u32 tileyoff = (finalY >> 8) & 0x7;

                    if (curtile & 0x0400) tilexoff = 7-tilexoff;
                    if (curtile & 0x0800) tileyoff = 7-tileyoff;

                    color = bgvram[(tilesetaddr + ((curtile & 0x03FF) << 6) + (tileyoff << 3) + tilexoff) & bgvrammask];

                    if (color)
                        drawPixel(&BGOBJLine[i], curpal[color], 0x01000000<<bgnum);
                }
            }

            rotX += rotA;
            rotY += rotC;
        }
    }
}

template<bool mosaic, SoftRenderer::DrawPixel drawPixel>
void SoftRenderer::DrawBG_Large(const UnitState *state, u32 line) // BG is always BG2
{
    u16 bgcnt = state->BGCnt[2];

    u16* pal = (u16*)state->Palette;

    // large BG sizes:
    // 0: 512x1024
    // 1: 1024x512
    // 2: 512x256
    // 3: 512x512
    u32 xmask, ymask;
    u32 yshift;
    switch (bgcnt & 0xC000)
    {
    case 0x0000: xmask = 0x1FFFF; ymask = 0x3FFFF; yshift = 9; break;
    case 0x4000: xmask = 0x3FFFF; ymask = 0x1FFFF; yshift = 10; break;
    case 0x8000: xmask = 0x1FFFF; ymask = 0x0FFFF; yshift = 9; break;
    case 0xC000: xmask = 0x1FFFF; ymask = 0x1FFFF; yshift = 9; break;
    }

    u32 ofxmask, ofymask;
    if (bgcnt & 0x2000)
    {
        ofxmask = 0;
        ofymask = 0;
    }
    else
    {
        ofxmask = ~xmask;
        ofymask = ~ymask;
    }

    s16 rotA = state->BGRotA[0];
    s16 rotB = state->BGRotB[0];
    s16 rotC = state->BGRotC[0];
    s16 rotD = state->BGRotD[0];

    s32 rotX = state->BGXRefInternal[0];
    s32 rotY = state->BGYRefInternal[0];

    if (bgcnt & 0x0040)
    {
        // vertical mosaic
        rotX -= (state->BGMosaicY * rotB);
        rotY -= (state->BGMosaicY * rotD);
    }

    u8* bgvram;
    u32 bgvrammask;
    GetBGVRAM(state, bgvram, bgvrammask);

    // 256-color bitmap

    u8 color;

    for (int i = 0; i < 256; i++)
    {
        if (state->Prepared->WindowMask[i] & (1<<2))
        {
            s32 finalX, finalY;
            if (mosaic)
            {
                int im = CurBGXMosaicTable[i];
                finalX = rotX - (im * rotA);
                finalY = rotY - (im * rotC);
            }
            else
            {
                finalX = rotX;
                finalY = rotY;
            }

            if (!(finalX & ofxmask) && !(finalY & ofymask))
            {
                color = bgvram[((((finalY & ymask) >> 8) << yshift) + ((finalX & xmask) >> 8)) & bgvrammask];

                if (color)
                    drawPixel(&BGOBJLine[i], pal[color], 0x01000000<<2);
            }
        }

        rotX += rotA;
        rotY += rotC;
    }
}

// OBJ line buffer:
// * bit0-15: color (bit15=1: direct color, bit15=0: palette index, bit12=0 to indicate extpal)
// * bit16-17: BG-relative priority
// * bit18: non-transparent sprite pixel exists here
// * bit19: X mosaic should be applied here
// * bit24-31: compositor flags

void SoftRenderer::ApplySpriteMosaicX(const UnitState *state)
{
    // apply X mosaic if needed
    // X mosaic for sprites is applied after all sprites are rendered

    if (state->OBJMosaicSize[0] == 0) return;

    u32* objLine = state->Shared->OBJLine;

    u8* curOBJXMosaicTable = MosaicTable[state->OBJMosaicSize[0]].data();

    u32 lastcolor = objLine[0];

    for (u32 i = 1; i < 256; i++)
    {
        u32 currentcolor = objLine[i];

        if (!(lastcolor & currentcolor & 0x100000) || curOBJXMosaicTable[i] == 0)
            lastcolor = currentcolor;
        else
            objLine[i] = lastcolor;
    }
}

template <SoftRenderer::DrawPixel drawPixel>
void SoftRenderer::InterleaveSprites(const UnitState *state, u32 prio)
{
    u32* objLine = state->Shared->OBJLine;
    u16* pal = (u16*)&state->Palette[0x200];

    if (state->DispCnt & 0x80000000)
    {
        u16* extpal = GetOBJExtPal(state);

        for (u32 i = 0; i < 256; i++)
        {
            if ((objLine[i] & 0x70000) != prio) continue;
            if (!(state->Prepared->WindowMask[i] & 0x10))        continue;

            u16 color;
            u32 pixel = objLine[i];

            if (pixel & 0x8000)
                color = pixel & 0x7FFF;
            else if (pixel & 0x1000)
                color = pal[pixel & 0xFF];
            else
                color = extpal[pixel & 0xFFF];

            drawPixel(&BGOBJLine[i], color, pixel & 0xFF000000);
        }
    }
    else
    {
        // optimized no-extpal version

        for (u32 i = 0; i < 256; i++)
        {
            if ((objLine[i] & 0x70000) != prio) continue;
            if (!(state->Prepared->WindowMask[i] & 0x10))        continue;

            u16 color;
            u32 pixel = objLine[i];

            if (pixel & 0x8000)
                color = pixel & 0x7FFF;
            else
                color = pal[pixel & 0xFF];

            drawPixel(&BGOBJLine[i], color, pixel & 0xFF000000);
        }
    }
}

#define DoDrawSprite(type, ...) \
    if (iswin) \
    { \
        DrawSprite_##type<true>(__VA_ARGS__); \
    } \
    else \
    { \
        DrawSprite_##type<false>(__VA_ARGS__); \
    }

void SoftRenderer::DrawSprites(const UnitState *state, u32 line)
{
    state->Shared->NumSprites = 0;
    memset(state->Shared->OBJLine, 0, 256*4);
    memset(state->Shared->OBJWindow, 0, 256);
    if (!(state->DispCnt & 0x1000)) return;

    u16* oam = (u16*)state->OAM;

    const s32 spritewidth[16] =
    {
        8, 16, 8, 8,
        16, 32, 8, 8,
        32, 32, 16, 8,
        64, 64, 32, 8
    };
    const s32 spriteheight[16] =
    {
        8, 8, 16, 8,
        16, 8, 32, 8,
        32, 16, 32, 8,
        64, 32, 64, 8
    };

    for (int bgnum = 0x0C00; bgnum >= 0x0000; bgnum -= 0x0400)
    {
        for (int sprnum = 127; sprnum >= 0; sprnum--)
        {
            u16* attrib = &oam[sprnum*4];

            if ((attrib[2] & 0x0C00) != bgnum)
                continue;

            bool iswin = (((attrib[0] >> 10) & 0x3) == 2);

            u32 sprline;
            if ((attrib[0] & 0x1000) && !iswin)
            {
                // apply Y mosaic
                sprline = state->OBJMosaicY;
            }
            else
                sprline = line;

            if (attrib[0] & 0x0100)
            {
                u32 sizeparam = (attrib[0] >> 14) | ((attrib[1] & 0xC000) >> 12);
                s32 width = spritewidth[sizeparam];
                s32 height = spriteheight[sizeparam];
                s32 boundwidth = width;
                s32 boundheight = height;

                if (attrib[0] & 0x0200)
                {
                    boundwidth <<= 1;
                    boundheight <<= 1;
                }

                u32 ypos = attrib[0] & 0xFF;
                if (((line - ypos) & 0xFF) >= (u32)boundheight)
                    continue;
                ypos = (sprline - ypos) & 0xFF;

                s32 xpos = (s32)(attrib[1] << 23) >> 23;
                if (xpos <= -boundwidth)
                    continue;

                u32 rotparamgroup = (attrib[1] >> 9) & 0x1F;

                DoDrawSprite(Rotscale, state, sprnum, boundwidth, boundheight, width, height, xpos, ypos);

                state->Shared->NumSprites++;
            }
            else
            {
                if (attrib[0] & 0x0200)
                    continue;

                u32 sizeparam = (attrib[0] >> 14) | ((attrib[1] & 0xC000) >> 12);
                s32 width = spritewidth[sizeparam];
                s32 height = spriteheight[sizeparam];

                u32 ypos = attrib[0] & 0xFF;
                if (((line - ypos) & 0xFF) >= (u32)height)
                    continue;
                ypos = (sprline - ypos) & 0xFF;

                s32 xpos = (s32)(attrib[1] << 23) >> 23;
                if (xpos <= -width)
                    continue;

                DoDrawSprite(Normal, state, sprnum, width, height, xpos, ypos);

                state->Shared->NumSprites++;
            }
        }
    }
}

template<bool window>
void SoftRenderer::DrawSprite_Rotscale(const UnitState *state, u32 num, u32 boundwidth, u32 boundheight, u32 width, u32 height, s32 xpos, s32 ypos)
{
    u16* oam = (u16*)state->OAM;
    u16* attrib = &oam[num * 4];
    u16* rotparams = &oam[(((attrib[1] >> 9) & 0x1F) * 16) + 3];

    u32 pixelattr = ((attrib[2] & 0x0C00) << 6) | 0xC0000;
    u32 tilenum = attrib[2] & 0x03FF;
    u32 spritemode = window ? 0 : ((attrib[0] >> 10) & 0x3);

    u32 ytilefactor;

    u8* objvram;
    u32 objvrammask;
    GetOBJVRAM(state, objvram, objvrammask);

    u32* objLine = state->Shared->OBJLine;
    u8* objWindow = state->Shared->OBJWindow;

    s32 centerX = boundwidth >> 1;
    s32 centerY = boundheight >> 1;

    if ((attrib[0] & 0x1000) && !window)
    {
        // apply Y mosaic
        pixelattr |= 0x100000;
    }

    u32 xoff;
    if (xpos >= 0)
    {
        xoff = 0;
        if ((xpos+boundwidth) > 256)
            boundwidth = 256-xpos;
    }
    else
    {
        xoff = -xpos;
        xpos = 0;
    }

    s16 rotA = (s16)rotparams[0];
    s16 rotB = (s16)rotparams[4];
    s16 rotC = (s16)rotparams[8];
    s16 rotD = (s16)rotparams[12];

    s32 rotX = ((xoff-centerX) * rotA) + ((ypos-centerY) * rotB) + (width << 7);
    s32 rotY = ((xoff-centerX) * rotC) + ((ypos-centerY) * rotD) + (height << 7);

    width <<= 8;
    height <<= 8;

    u16 color = 0; // transparent in all cases

    if (spritemode == 3)
    {
        u32 alpha = attrib[2] >> 12;
        if (!alpha) return;
        alpha++;

        pixelattr |= (0xC0000000 | (alpha << 24));

        u32 pixelsaddr;
        if (state->DispCnt & 0x40)
        {
            if (state->DispCnt & 0x20)
            {
                // 'reserved'
                // draws nothing

                return;
            }
            else
            {
                pixelsaddr = tilenum << (7 + ((state->DispCnt >> 22) & 0x1));
                ytilefactor = ((width >> 8) * 2);
            }
        }
        else
        {
            if (state->DispCnt & 0x20)
            {
                pixelsaddr = ((tilenum & 0x01F) << 4) + ((tilenum & 0x3E0) << 7);
                ytilefactor = (256 * 2);
            }
            else
            {
                pixelsaddr = ((tilenum & 0x00F) << 4) + ((tilenum & 0x3F0) << 7);
                ytilefactor = (128 * 2);
            }
        }

        for (; xoff < boundwidth;)
        {
            if ((u32)rotX < width && (u32)rotY < height)
            {
                color = *(u16*)&objvram[(pixelsaddr + ((rotY >> 8) * ytilefactor) + ((rotX >> 8) << 1)) & objvrammask];

                if (color & 0x8000)
                {
                    if (window) objWindow[xpos] = 1;
                    else        objLine[xpos] = color | pixelattr;
                }
                else if (!window)
                {
                    if (objLine[xpos] == 0)
                        objLine[xpos] = pixelattr & 0x180000;
                }
            }

            rotX += rotA;
            rotY += rotC;
            xoff++;
            xpos++;
        }
    }
    else
    {
        u32 pixelsaddr = tilenum;
        if (state->DispCnt & 0x10)
        {
            pixelsaddr <<= ((state->DispCnt >> 20) & 0x3);
            ytilefactor = (width >> 11) << ((attrib[0] & 0x2000) ? 1:0);
        }
        else
        {
            ytilefactor = 0x20;
        }

        if (spritemode == 1) pixelattr |= 0x80000000;
        else                 pixelattr |= 0x10000000;

        ytilefactor <<= 5;
        pixelsaddr <<= 5;

        if (attrib[0] & 0x2000)
        {
            // 256-color

            if (!window)
            {
                if (!(state->DispCnt & 0x80000000))
                    pixelattr |= 0x1000;
                else
                    pixelattr |= ((attrib[2] & 0xF000) >> 4);
            }

            for (; xoff < boundwidth;)
            {
                if ((u32)rotX < width && (u32)rotY < height)
                {
                    color = objvram[(pixelsaddr + ((rotY>>11)*ytilefactor) + ((rotY&0x700)>>5) + ((rotX>>11)*64) + ((rotX&0x700)>>8)) & objvrammask];

                    if (color)
                    {
                        if (window) objWindow[xpos] = 1;
                        else        objLine[xpos] = color | pixelattr;
                    }
                    else if (!window)
                    {
                        if (objLine[xpos] == 0)
                            objLine[xpos] = pixelattr & 0x180000;
                    }
                }

                rotX += rotA;
                rotY += rotC;
                xoff++;
                xpos++;
            }
        }
        else
        {
            // 16-color
            if (!window)
            {
                pixelattr |= 0x1000;
                pixelattr |= ((attrib[2] & 0xF000) >> 8);
            }

            for (; xoff < boundwidth;)
            {
                if ((u32)rotX < width && (u32)rotY < height)
                {
                    color = objvram[(pixelsaddr + ((rotY>>11)*ytilefactor) + ((rotY&0x700)>>6) + ((rotX>>11)*32) + ((rotX&0x700)>>9)) & objvrammask];
                    if (rotX & 0x100)
                        color >>= 4;
                    else
                        color &= 0x0F;

                    if (color)
                    {
                        if (window) objWindow[xpos] = 1;
                        else        objLine[xpos] = color | pixelattr;
                    }
                    else if (!window)
                    {
                        if (objLine[xpos] == 0)
                            objLine[xpos] = pixelattr & 0x180000;
                    }
                }

                rotX += rotA;
                rotY += rotC;
                xoff++;
                xpos++;
            }
        }
    }
}

template<bool window>
void SoftRenderer::DrawSprite_Normal(const UnitState *state, u32 num, u32 width, u32 height, s32 xpos, s32 ypos)
{
    u16* oam = (u16*)state->OAM;
    u16* attrib = &oam[num * 4];

    u32 pixelattr = ((attrib[2] & 0x0C00) << 6) | 0xC0000;
    u32 tilenum = attrib[2] & 0x03FF;
    u32 spritemode = window ? 0 : ((attrib[0] >> 10) & 0x3);

    u32 wmask = width - 8; // really ((width - 1) & ~0x7)

    if ((attrib[0] & 0x1000) && !window)
    {
        // apply Y mosaic
        pixelattr |= 0x100000;
    }

    u8* objvram;
    u32 objvrammask;
    GetOBJVRAM(state, objvram, objvrammask);

    u32* objLine = state->Shared->OBJLine;
    u8* objWindow = state->Shared->OBJWindow;

    // yflip
    if (attrib[1] & 0x2000)
        ypos = height-1 - ypos;

    u32 xoff;
    u32 xend = width;
    if (xpos >= 0)
    {
        xoff = 0;
        if ((xpos+xend) > 256)
            xend = 256-xpos;
    }
    else
    {
        xoff = -xpos;
        xpos = 0;
    }

    u16 color = 0; // transparent in all cases

    if (spritemode == 3)
    {
        // bitmap sprite

        u32 alpha = attrib[2] >> 12;
        if (!alpha) return;
        alpha++;

        pixelattr |= (0xC0000000 | (alpha << 24));

        u32 pixelsaddr = tilenum;
        if (state->DispCnt & 0x40)
        {
            if (state->DispCnt & 0x20)
            {
                // 'reserved'
                // draws nothing

                return;
            }
            else
            {
                pixelsaddr <<= (7 + ((state->DispCnt >> 22) & 0x1));
                pixelsaddr += (ypos * width * 2);
            }
        }
        else
        {
            if (state->DispCnt & 0x20)
            {
                pixelsaddr = ((tilenum & 0x01F) << 4) + ((tilenum & 0x3E0) << 7);
                pixelsaddr += (ypos * 256 * 2);
            }
            else
            {
                pixelsaddr = ((tilenum & 0x00F) << 4) + ((tilenum & 0x3F0) << 7);
                pixelsaddr += (ypos * 128 * 2);
            }
        }

        s32 pixelstride;

        if (attrib[1] & 0x1000) // xflip
        {
            pixelsaddr += ((width-1) << 1);
            pixelsaddr -= (xoff << 1);
            pixelstride = -2;
        }
        else
        {
            pixelsaddr += (xoff << 1);
            pixelstride = 2;
        }

        for (; xoff < xend;)
        {
            color = *(u16*)&objvram[pixelsaddr & objvrammask];

            pixelsaddr += pixelstride;

            if (color & 0x8000)
            {
                if (window) objWindow[xpos] = 1;
                else        objLine[xpos] = color | pixelattr;
            }
            else if (!window)
            {
                if (objLine[xpos] == 0)
                    objLine[xpos] = pixelattr & 0x180000;
            }

            xoff++;
            xpos++;
        }
    }
    else
    {
        u32 pixelsaddr = tilenum;
        if (state->DispCnt & 0x10)
        {
            pixelsaddr <<= ((state->DispCnt >> 20) & 0x3);
            pixelsaddr += ((ypos >> 3) * (width >> 3)) << ((attrib[0] & 0x2000) ? 1:0);
        }
        else
        {
            pixelsaddr += ((ypos >> 3) * 0x20);
        }

        if (spritemode == 1) pixelattr |= 0x80000000;
        else                 pixelattr |= 0x10000000;

        if (attrib[0] & 0x2000)
        {
            // 256-color
            pixelsaddr <<= 5;
            pixelsaddr += ((ypos & 0x7) << 3);
            s32 pixelstride;

            if (!window)
            {
                if (!(state->DispCnt & 0x80000000))
                    pixelattr |= 0x1000;
                else
                    pixelattr |= ((attrib[2] & 0xF000) >> 4);
            }

            if (attrib[1] & 0x1000) // xflip
            {
                pixelsaddr += (((width-1) & wmask) << 3);
                pixelsaddr += ((width-1) & 0x7);
                pixelsaddr -= ((xoff & wmask) << 3);
                pixelsaddr -= (xoff & 0x7);
                pixelstride = -1;
            }
            else
            {
                pixelsaddr += ((xoff & wmask) << 3);
                pixelsaddr += (xoff & 0x7);
                pixelstride = 1;
            }

            for (; xoff < xend;)
            {
                color = objvram[pixelsaddr & objvrammask];

                pixelsaddr += pixelstride;

                if (color)
                {
                    if (window) objWindow[xpos] = 1;
                    else        objLine[xpos] = color | pixelattr;
                }
                else if (!window)
                {
                    if (objLine[xpos] == 0)
                        objLine[xpos] = pixelattr & 0x180000;
                }

                xoff++;
                xpos++;
                if (!(xoff & 0x7)) pixelsaddr += (56 * pixelstride);
            }
        }
        else
        {
            // 16-color
            pixelsaddr <<= 5;
            pixelsaddr += ((ypos & 0x7) << 2);
            s32 pixelstride;

            if (!window)
            {
                pixelattr |= 0x1000;
                pixelattr |= ((attrib[2] & 0xF000) >> 8);
            }

            // TODO: optimize VRAM access!!
            // TODO: do xflip better? the 'two pixels per byte' thing makes it a bit shitty

            if (attrib[1] & 0x1000) // xflip
            {
                pixelsaddr += (((width-1) & wmask) << 2);
                pixelsaddr += (((width-1) & 0x7) >> 1);
                pixelsaddr -= ((xoff & wmask) << 2);
                pixelsaddr -= ((xoff & 0x7) >> 1);
                pixelstride = -1;
            }
            else
            {
                pixelsaddr += ((xoff & wmask) << 2);
                pixelsaddr += ((xoff & 0x7) >> 1);
                pixelstride = 1;
            }

            for (; xoff < xend;)
            {
                if (attrib[1] & 0x1000)
                {
                    if (xoff & 0x1) { color = objvram[pixelsaddr & objvrammask] & 0x0F; pixelsaddr--; }
                    else              color = objvram[pixelsaddr & objvrammask] >> 4;
                }
                else
                {
                    if (xoff & 0x1) { color = objvram[pixelsaddr & objvrammask] >> 4; pixelsaddr++; }
                    else              color = objvram[pixelsaddr & objvrammask] & 0x0F;
                }

                if (color)
                {
                    if (window) objWindow[xpos] = 1;
                    else        objLine[xpos] = color | pixelattr;
                }
                else if (!window)
                {
                    if (objLine[xpos] == 0)
                        objLine[xpos] = pixelattr & 0x180000;
                }

                xoff++;
                xpos++;
                if (!(xoff & 0x7)) pixelsaddr += ((attrib[1] & 0x1000) ? -28 : 28);
            }
        }
    }
}


u16* SoftRenderer::GetBGExtPal(const UnitState *state, u32 slot, u32 pal)
{
    const u32 PaletteSize = 256 * 2;
    const u32 SlotSize = PaletteSize * 16;
    return (u16*)&(state->Num == 0
         ? state->GPU->VRAMFlat_ABGExtPal
         : state->GPU->VRAMFlat_BBGExtPal)[slot * SlotSize + pal * PaletteSize];
}

u16* SoftRenderer::GetOBJExtPal(const UnitState *state)
{
    return state->Num == 0
         ? (u16*)state->GPU->VRAMFlat_AOBJExtPal
         : (u16*)state->GPU->VRAMFlat_BOBJExtPal;
}

void SoftRenderer::GetBGVRAM(const UnitState *state, u8*& data, u32& mask) const
{
    if (state->Num == 0)
    {
        data = state->GPU->VRAMFlat_ABG;
        mask = 0x7FFFF;
    }
    else
    {
        data = state->GPU->VRAMFlat_BBG;
        mask = 0x1FFFF;
    }
}

void SoftRenderer::GetOBJVRAM(const UnitState *state, u8*& data, u32& mask) const
{
    if (state->Num == 0)
    {
        data = state->GPU->VRAMFlat_AOBJ;
        mask = 0x3FFFF;
    }
    else
    {
        data = state->GPU->VRAMFlat_BOBJ;
        mask = 0x1FFFF;
    }
}


}
}
